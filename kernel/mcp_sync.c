/* IKOS Orthogonal Persistence - MCP Time-Travel Interface, kernel adapter
 * (#159 Milestone E, #229)
 *
 * Wires the MCP tools (mcp.c) to the booted machine: positions and navigation
 * through the time-travel core (core/timetravel.h), memory and registers of
 * the reconstructed processes, the byte-exact verification, and live runs
 * through the debug monitor. The transport is the monitor's COM3 line loop.
 */

#include "mcp.h"
#include "keyframe_ring.h"
#include "keyframe_store.h"
#include "rewind.h"
#include "reverse.h"
#include "revbreak.h"
#include "core/timetravel.h"
#include "core/machine.h"
#include "core/monitor.h"
#include "core/proc.h"
#include "core/mm.h"

static mcp_probe_fn g_probe;
static void*        g_probe_ctx;

static int op_list(void* c, uint64_t* oldest, uint64_t* newest, uint32_t* count) {
    (void)c;
    tt_window_t w;
    if (!tt_window(&w)) return -1;
    *oldest = w.oldest;
    *newest = w.newest;
    *count = w.count;
    return 0;
}

static int op_rewind(void* c, uint64_t epoch, uint64_t offset) {
    (void)c;
    monitor_note_navigation();
    return tt_goto(epoch, offset, 0) == TT_OK ? 0 : -1;
}

static int op_goto_insn(void* c, uint64_t epoch, uint64_t step, uint64_t insn) {
    (void)c;
    monitor_note_navigation();
    return tt_goto(epoch, step, insn) == TT_OK ? 0 : -1;
}

/* One kernel entry back: to the entry this position follows, or through the
 * reverse verb to the previous entry. */
static int op_reverse_step(void* c, uint64_t* out_epoch, uint64_t* out_offset) {
    (void)c;
    monitor_note_navigation();
    int rc;
    if (!machine_at_entry()) {
        rc = tt_goto(machine_epoch(), machine_step(), 0) == TT_OK ? REVERSE_OK : REVERSE_ERR_REWIND;
    } else {
        kreverse_set_position(machine_epoch(), machine_step());
        rc = kreverse_step();
    }
    *out_epoch = machine_epoch();
    *out_offset = machine_step();
    return rc == REVERSE_OK ? 0 : -1;
}

/* One kernel entry forward (to the next entry of the recording). */
static int step_entry_forward(void) {
    if (tt_at_end()) return 1;
    uint64_t epoch = machine_epoch(), step = machine_step() + 1, len;
    if (!tt_epoch_len(epoch, &len)) return -1;
    if (step > len) {
        /* At an epoch's last entry: the next entry is step 1 of the next
         * keyframe's epoch. */
        tt_window_t w;
        if (!tt_window(&w) || epoch >= w.newest) return 1;
        uint64_t next = w.newest;
        const keyframe_ring_t* r = keyframe_store_ring(keyframe_store_get());
        for (uint32_t i = 0; i < KEYFRAME_RING_MAX; i++) {
            if (r->slots[i].valid && r->slots[i].epoch > epoch && r->slots[i].epoch < next) {
                next = r->slots[i].epoch;
            }
        }
        epoch = next;
        step = 1;
        if (!tt_epoch_len(epoch, &len) || step > len) return tt_goto(epoch, 0, 0) == TT_OK ? 0 : -1;
    }
    return tt_goto(epoch, step, 0) == TT_OK ? 0 : -1;
}

static int op_move(void* c, bool backward, bool stepi) {
    (void)c;
    monitor_note_navigation();
    if (stepi) {
        int rc = backward ? tt_reverse_stepi() : tt_stepi();
        if (rc == TT_BEGIN || rc == TT_END) return 1;
        return rc == TT_OK ? 0 : -1;
    }
    if (backward) {
        uint64_t e, o;
        return op_reverse_step(c, &e, &o) == 0 ? 0 : 1;
    }
    return step_entry_forward();
}

static int op_watch(void* c, uint64_t* out_epoch, uint64_t* out_offset) {
    (void)c;
    if (!g_probe) return -1;
    reverse_pos_t hit;
    if (krevbreak_watchpoint(g_probe, g_probe_ctx, &hit) != REVBREAK_OK) return -1;
    *out_epoch = hit.epoch;
    *out_offset = hit.offset;
    return 0;
}

static int op_watch_memory(void* c, uint32_t pid, uint64_t addr, uint32_t len) {
    (void)c;
    monitor_note_navigation();
    tt_pos_t where;
    int rc = tt_last_change(pid, addr, len, &where);
    if (rc == TT_OK) return 0;
    if (rc == TT_BEGIN) return 1;
    return -1;
}

static int op_where(void* c, mcp_pos_t* out) {
    (void)c;
    tt_pos_t p;
    tt_position(&p);
    out->epoch = p.epoch;
    out->step = p.step;
    out->insn = p.insn;
    process_t* cur = machine_current();
    out->pid = cur ? (uint32_t)cur->pid : 0;
    out->rip = cur ? cur->context.rip : 0;
    tt_window_t w;
    out->edge = 0;
    if (tt_at_end()) out->edge = 2;
    else if (tt_window(&w) && p.epoch == w.oldest && p.step == 0 && p.insn == 0) out->edge = 1;
    out->stop = out->edge == 2 ? monitor_stop_text() : 0;
    return 0;
}

static int op_read_memory(void* c, uint32_t pid, uint64_t addr, uint8_t* buf, uint32_t len) {
    (void)c;
    process_t* p = proc_by_pid(pid);
    return p && uaccess_read(p->address_space, addr, buf, len) ? 0 : -1;
}

static int op_get_registers(void* c, uint32_t pid, mcp_regs_t* r) {
    (void)c;
    process_t* p = proc_by_pid(pid);
    if (!p) return -1;
    const process_context_t* x = &p->context;
    r->rax = x->rax; r->rbx = x->rbx; r->rcx = x->rcx; r->rdx = x->rdx;
    r->rsi = x->rsi; r->rdi = x->rdi; r->rbp = x->rbp; r->rsp = x->rsp;
    r->r8 = x->r8;   r->r9 = x->r9;   r->r10 = x->r10; r->r11 = x->r11;
    r->r12 = x->r12; r->r13 = x->r13; r->r14 = x->r14; r->r15 = x->r15;
    r->rip = x->rip;
    r->rflags = x->rflags & 0xED5ULL;
    return 0;
}

static uint32_t op_list_processes(void* c, mcp_proc_t* out, uint32_t max) {
    (void)c;
    uint32_t n = 0;
    process_t* cur = machine_current();
    for (uint32_t i = 0; i < proc_count() && n < max; i++) {
        process_t* p = proc_at(i);
        out[n].pid = (uint32_t)p->pid;
        uint32_t k = 0;
        for (; k + 1 < sizeof(out[n].name) && p->name[k]; k++) out[n].name[k] = p->name[k];
        out[n].name[k] = 0;
        out[n].current = p == cur;
        n++;
    }
    return n;
}

static int op_verify(void* c, mcp_verify_t* out) {
    (void)c;
    monitor_note_navigation();
    tt_verify_t v;
    if (tt_verify(&v) != TT_OK) return -1;
    out->epochs_checked = v.epochs_checked;
    out->epochs_diverged = v.epochs_diverged;
    out->first_epoch = v.first_epoch;
    out->first_component = v.first_component;
    return 0;
}

static int op_resume(void* c) {
    (void)c;
    return monitor_run_live() == 0 ? 0 : -1;
}

static mcp_ops_t g_ops;

void mcp_bind(void) {
    g_ops.list_checkpoints = op_list;
    g_ops.rewind_to = op_rewind;
    g_ops.reverse_step = op_reverse_step;
    g_ops.watch_last_write = op_watch;
    g_ops.ctx = 0;
    g_ops.where = op_where;
    g_ops.goto_insn = op_goto_insn;
    g_ops.move = op_move;
    g_ops.read_memory = op_read_memory;
    g_ops.get_registers = op_get_registers;
    g_ops.list_processes = op_list_processes;
    g_ops.watch_memory = op_watch_memory;
    g_ops.verify = op_verify;
    g_ops.resume = op_resume;
}

void mcp_set_ring(const void* keyframe_ring) {
    (void)keyframe_ring;   /* the window comes from the keyframe store */
}

void mcp_set_watch_probe(mcp_probe_fn probe, void* ctx) {
    g_probe = probe;
    g_probe_ctx = ctx;
}

const mcp_ops_t* mcp_kernel_ops(void) {
    return &g_ops;
}
