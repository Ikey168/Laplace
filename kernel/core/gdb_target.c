/* Laplace core: the gdb target (#227, #228).
 *
 * The operations gdbstub.c dispatches, over the recorded machine:
 *   registers / memory  of any process (a gdb thread per process) at the
 *                       reconstructed position
 *   stepi / reverse-stepi       one instruction forward / backward
 *   continue / reverse-continue to the next / previous hardware breakpoint or
 *                       watchpoint hit, or to the end / start of the recording
 *                       ("replaylog:end" / "replaylog:begin"); continuing at the
 *                       end of the recording leaves history and runs live
 *   Z0/Z1               execution breakpoints (debug registers), Z2-Z4
 *                       watchpoints; they belong to the process gdb has
 *                       selected when they are inserted
 *   monitor <cmd>       where, window, verify, stats, goto E S [I], help
 */

#include "core/gdb_target.h"
#include "core/monitor.h"
#include "core/timetravel.h"
#include "core/machine.h"
#include "core/proc.h"
#include "core/mm.h"
#include "core/console.h"
#include "gdbstub.h"
#include "divergence.h"

void* memset(void* dst, int c, size_t n);
size_t strlen(const char* s);
int strncmp(const char* a, const char* b, size_t n);

static int            g_edge;       /* 0 none, 1 start of the recording, 2 end */
static tt_hit_t       g_hit;
static machine_hwbp_t g_bps[MACHINE_MAX_HWBP];   /* gdb's breakpoints, by debug register */

static void moved(int rc) {
    g_edge = rc == TT_BEGIN ? 1 : rc == TT_END ? 2 : 0;
    g_hit.hit = false;
    monitor_note_navigation();
}

static int op_reverse_step(void* c) {
    (void)c;
    moved(tt_reverse_stepi());
    return 0;
}

static int op_reverse_continue(void* c) {
    (void)c;
    tt_hit_t hit;
    int rc = tt_reverse_continue(&hit);
    moved(rc);
    g_hit = hit;
    if (rc == TT_OK && hit.hit && g_bps[hit.slot].kind != 0) {
        /* Backward, gdb expects a watchpoint to stop before the write, with
         * the old value still in memory (it compares old and new to decide
         * the watchpoint triggered). The search lands just after the write. */
        tt_reverse_stepi();
    }
    return 0;
}

static int op_step(void* c) {
    (void)c;
    if (machine_mode() == MACHINE_LIVE) {
        /* Past the end of the recording: run live to the next kernel entry. */
        machine_request_stop();
        monitor_run_live();
        g_edge = 0;
        g_hit.hit = false;
        return 0;
    }
    moved(tt_stepi());
    return 0;
}

static int op_cont(void* c) {
    (void)c;
    if (tt_at_end()) {
        /* Continuing from the end of history runs the machine live. */
        monitor_run_live();
        g_edge = 0;
        g_hit.hit = false;
        return 0;
    }
    tt_hit_t hit;
    int rc = tt_continue(&hit);
    moved(rc);
    g_hit = hit;
    return 0;
}

static process_t* thread_proc(uint32_t tid) {
    if (tid) return proc_by_pid(tid);
    return machine_current();
}

static void put_le(uint8_t* buf, uint32_t off, uint64_t v, uint32_t size) {
    for (uint32_t i = 0; i < size; i++) buf[off + i] = (uint8_t)(v >> (8 * i));
}

static int op_read_regs(void* c, uint32_t tid, uint8_t* buf) {
    (void)c;
    process_t* p = thread_proc(tid);
    if (!p) return -1;
    memset(buf, 0, GDBSTUB_REGS_BYTES);
    const process_context_t* x = &p->context;
    const uint64_t gpr[17] = {
        x->rax, x->rbx, x->rcx, x->rdx, x->rsi, x->rdi, x->rbp, x->rsp,
        x->r8, x->r9, x->r10, x->r11, x->r12, x->r13, x->r14, x->r15, x->rip,
    };
    for (int i = 0; i < 17; i++) put_le(buf, (uint32_t)i * 8, gpr[i], 8);
    put_le(buf, 136, x->rflags & ~(RFLAGS_TF | RFLAGS_RF), 4);
    put_le(buf, 140, SEL_UCODE, 4);   /* cs */
    put_le(buf, 144, SEL_UDATA, 4);   /* ss */
    put_le(buf, 148, SEL_UDATA, 4);   /* ds */
    put_le(buf, 152, SEL_UDATA, 4);   /* es */
    put_le(buf, 244, 0x37F, 4);       /* fctrl: the x87 reset value (unused by programs) */
    put_le(buf, 532, 0x1F80, 4);      /* mxcsr reset value */
    return 0;
}

static int op_read_mem(void* c, uint32_t tid, uint64_t addr, uint8_t* buf, uint32_t len) {
    (void)c;
    process_t* p = thread_proc(tid);
    if (!p) return -1;
    return uaccess_read(p->address_space, addr, buf, len) ? 0 : -1;
}

static uint32_t op_threads(void* c, uint32_t* tids, uint32_t max) {
    (void)c;
    uint32_t n = 0;
    for (uint32_t i = 0; i < proc_count() && n < max; i++) tids[n++] = (uint32_t)proc_at(i)->pid;
    return n;
}

static uint32_t op_current(void* c) {
    (void)c;
    process_t* p = machine_current();
    if (p) return (uint32_t)p->pid;
    return proc_count() ? (uint32_t)proc_at(0)->pid : 1;
}

static int op_thread_name(void* c, uint32_t tid, char* out, uint32_t cap) {
    (void)c;
    process_t* p = proc_by_pid(tid);
    if (!p) return 0;
    return ksnprintf(out, cap, "%s (pid %u)", p->name, tid) < (int)cap ? (int)strlen(out) : (int)cap - 1;
}

static int op_breakpoint(void* c, int type, uint64_t addr, uint32_t kind, bool insert) {
    (void)c;
    uint8_t dr_kind = type <= 1 ? 0 : type == 2 ? 1 : 3;   /* x86 has no read-only watch */
    uint8_t len = type <= 1 ? 1 : (uint8_t)kind;
    if (type > 1 && len != 1 && len != 2 && len != 4 && len != 8) return -1;
    uint32_t pid = op_current(0);
    for (int i = 0; i < MACHINE_MAX_HWBP; i++) {
        machine_hwbp_t* b = &g_bps[i];
        if (insert && !b->active) {
            machine_hwbp_t nb = { true, pid, addr, dr_kind, len };
            *b = nb;
            machine_set_hwbp(i, b);
            return 0;
        }
        if (!insert && b->active && b->addr == addr && b->kind == dr_kind) {
            b->active = false;
            machine_set_hwbp(i, 0);
            return 0;
        }
    }
    return insert ? -1 : 0;   /* out of debug registers / already gone */
}

static const char* sig_hex(void) {
    switch (monitor_live_signal()) {
    case 6: return "06";
    case 11: return "0b";
    case 4: return "04";
    case 8: return "08";
    case 2: return "02";
    default: return "05";
    }
}

static int op_stop_reply(void* c, char* out, uint32_t cap) {
    (void)c;
    uint32_t tid = op_current(0);
    int n = ksnprintf(out, cap, "T%sthread:%x;", monitor_live_signal() ? sig_hex() : "05", tid);
    if (g_edge == 1) n += ksnprintf(out + n, cap - (uint32_t)n, "replaylog:begin;");
    if (g_edge == 2) n += ksnprintf(out + n, cap - (uint32_t)n, "replaylog:end;");
    if (g_hit.hit) {
        const machine_hwbp_t* b = &g_bps[g_hit.slot];
        if (b->kind == 0) {
            n += ksnprintf(out + n, cap - (uint32_t)n, "hwbreak:;");
        } else {
            n += ksnprintf(out + n, cap - (uint32_t)n, "%s:%lx;", b->kind == 1 ? "watch" : "awatch", b->addr);
        }
    }
    return n;
}

static int op_monitor(void* c, const char* cmd, char* out, uint32_t cap) {
    (void)c;
    return monitor_command(cmd, out, cap);
}

static gdbstub_ops_t g_target = {
    .reverse_step = op_reverse_step,
    .reverse_continue = op_reverse_continue,
    .ctx = 0,
    .step = op_step,
    .cont = op_cont,
    .read_regs = op_read_regs,
    .read_mem = op_read_mem,
    .threads = op_threads,
    .current_thread = op_current,
    .thread_name = op_thread_name,
    .breakpoint = op_breakpoint,
    .stop_reply = op_stop_reply,
    .monitor = op_monitor,
};

void gdb_target_init(void) {
    gdbstub_set_ops(&g_target);
}

void gdb_target_live_stop(void) {
    g_edge = 0;
    g_hit.hit = false;
}
