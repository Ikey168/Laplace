/* Laplace core: the debug monitor (#226). See include/core/monitor.h. */

#include "core/monitor.h"
#include "core/gdb_target.h"
#include "core/timetravel.h"
#include "core/machine.h"
#include "core/proc.h"
#include "core/cpu.h"
#include "core/console.h"
#include "gdb_serial.h"
#include "gdbstub.h"
#include "mcp.h"
#include "mcp_server.h"
#include "reverse.h"
#include "divergence.h"

int strncmp(const char* a, const char* b, size_t n);

static char g_stop_text[160] = "not started";
static int  g_live_signal;

/* ---- Live stops ---- */

void monitor_live_stopped(void) {
    const machine_stop_t* st = machine_last_stop();
    int sig = 5;
    switch (st->reason) {
    case STOP_BREAK:
        ksnprintf(g_stop_text, sizeof(g_stop_text), "break: pid %u asserted (code %lx)",
                  st->pid, st->code);
        sig = 6;
        break;
    case STOP_FAULT:
        ksnprintf(g_stop_text, sizeof(g_stop_text), "fault: pid %u vector %lu error %lx address %lx",
                  st->pid, st->vector, st->error, st->cr2);
        sig = st->vector == 6 ? 4 : st->vector == 0 ? 8 : 11;
        break;
    case STOP_EXITED:
        ksnprintf(g_stop_text, sizeof(g_stop_text), "exit: pid %u is the last process and is exiting",
                  st->pid);
        break;
    case STOP_REQUEST:
        ksnprintf(g_stop_text, sizeof(g_stop_text), "interrupt: a debugger asked to stop");
        sig = 2;
        break;
    default:
        ksnprintf(g_stop_text, sizeof(g_stop_text), "idle: nothing left to run");
        break;
    }
    g_live_signal = sig;
    tt_seal();
    kreverse_set_position(machine_epoch(), machine_step());
    gdb_target_live_stop();
    kprintf("monitor: stopped (%s) at epoch %lu step %lu; gdb on COM2, MCP on COM3\n",
            g_stop_text, machine_epoch(), machine_step());
}

int monitor_run_live(void) {
    if (machine_mode() == MACHINE_REPLAY) {
        int rc = tt_resume_live();
        if (rc != TT_OK) {
            kprintf("monitor: cannot return to the end of the recording (%d)\n", rc);
            return rc;
        }
    }
    kprintf("monitor: running live from epoch %lu step %lu\n", machine_epoch(), machine_step());
    machine_resume();
    monitor_live_stopped();
    return 0;
}

void monitor_note_navigation(void) { g_live_signal = 0; }
int monitor_live_signal(void) { return g_live_signal; }
const char* monitor_stop_text(void) { return g_stop_text; }

/* ---- "monitor <cmd>" ---- */

static uint64_t parse_u64(const char** p) {
    while (**p == ' ') (*p)++;
    uint64_t v = 0;
    while (**p >= '0' && **p <= '9') v = v * 10 + (uint64_t)(*(*p)++ - '0');
    return v;
}

int monitor_command(const char* cmd, char* out, uint32_t cap) {
    tt_pos_t p;
    tt_window_t w;
    tt_position(&p);
    if (strncmp(cmd, "where", 5) == 0 || cmd[0] == 0) {
        process_t* cur = machine_current();
        bool have = tt_window(&w);
        return ksnprintf(out, cap,
                         "position epoch %lu step %lu insn %lu, pid %d, rip %lx\n"
                         "window epochs %lu..%lu (end at step %lu)\nlast live stop: %s\n",
                         p.epoch, p.step, p.insn, cur ? cur->pid : 0, cur ? cur->context.rip : 0,
                         have ? w.oldest : 0, have ? w.newest : 0, have ? w.end_step : 0,
                         g_stop_text);
    }
    if (strncmp(cmd, "window", 6) == 0) {
        if (!tt_window(&w)) return ksnprintf(out, cap, "no recording\n");
        return ksnprintf(out, cap, "%u keyframes, epochs %lu..%lu, recording ends at step %lu\n",
                         w.count, w.oldest, w.newest, w.end_step);
    }
    if (strncmp(cmd, "verify", 6) == 0) {
        tt_verify_t v;
        int rc = tt_verify(&v);
        monitor_note_navigation();
        if (rc != TT_OK) return ksnprintf(out, cap, "verification could not run (%d)\n", rc);
        if (v.diverged) {
            return ksnprintf(out, cap, "DIVERGED: %u of %u epochs; first epoch %lu component %u\n",
                             v.epochs_diverged, v.epochs_checked, v.first_epoch, v.first_component);
        }
        return ksnprintf(out, cap, "byte-exact: %u epochs re-executed from their keyframes\n",
                         v.epochs_checked);
    }
    if (strncmp(cmd, "stats", 5) == 0) {
        const tt_stats_t* s = tt_stats();
        return ksnprintf(out, cap,
                         "keyframes %lu, writebacks %lu, COW captures %lu, journals %lu, "
                         "restores %lu, replayed steps %lu\n",
                         s->keyframes_taken, s->writebacks, s->cow_captures, s->journals_written,
                         s->restores, s->replayed_steps);
    }
    if (strncmp(cmd, "goto", 4) == 0) {
        const char* a = cmd + 4;
        uint64_t e = parse_u64(&a), s = parse_u64(&a), i = parse_u64(&a);
        int rc = tt_goto(e, s, i);
        monitor_note_navigation();
        return ksnprintf(out, cap, rc == TT_OK ? "at epoch %lu step %lu insn %lu\n"
                                               : "cannot reach epoch %lu step %lu insn %lu\n", e, s, i);
    }
    return ksnprintf(out, cap,
                     "Laplace monitor commands:\n"
                     "  where            position, window, and why the machine stopped\n"
                     "  window           the retained recording\n"
                     "  verify           re-execute every retained epoch and compare checksums\n"
                     "  stats            recording and replay counters\n"
                     "  goto E S [I]     reconstruct epoch E, step S, instruction I\n");
}

/* ---- Transports ---- */

static int g_gdb_pushback = -1;

static int gdb_get(void* ctx) {
    (void)ctx;
    if (g_gdb_pushback >= 0) {
        int c = g_gdb_pushback;
        g_gdb_pushback = -1;
        return c;
    }
    return serial_getc(PORT_GDB);
}

static int gdb_put(void* ctx, uint8_t b) {
    (void)ctx;
    serial_putc(PORT_GDB, (char)b);
    return 0;
}

static int mcp_get(void* ctx) {
    (void)ctx;
    return serial_getc(PORT_MCP);
}

static int mcp_put(void* ctx, uint8_t b) {
    (void)ctx;
    serial_putc(PORT_MCP, (char)b);
    return 0;
}

static int mcp_handle_kernel(const char* req, uint32_t len, char* out, uint32_t cap) {
    return mcp_handle(mcp_kernel_ops(), req, len, out, cap);
}

static const gdb_serial_ops_t g_gdb_io = { gdb_get, gdb_put, 0 };
static const mcp_transport_t  g_mcp_io = { mcp_get, mcp_put, 0 };

void monitor_init(void) {
    gdb_target_init();
    mcp_bind();
}

void monitor_run(void) {
    for (;;) {
        if (serial_rx_ready(PORT_GDB)) {
            int c = serial_getc(PORT_GDB);
            /* A Ctrl-C that stopped a live run was already answered by the
             * stop reply of the run it interrupted; stray acks need nothing. */
            if (c != 0x03 && c != '+' && c != '-') {
                g_gdb_pushback = c;
                gdb_serial_serve_once(&g_gdb_io, gdbstub_serve);
            }
        }
        if (serial_rx_ready(PORT_MCP)) {
            mcp_server_serve_once(&g_mcp_io, mcp_handle_kernel);
        }
        __asm__ volatile("pause");
    }
}
