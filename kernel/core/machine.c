/* Laplace core: the recorded machine's execution engine (#220, #222, #225).
 * See include/core/machine.h for the step, determinism, and coroutine model. */

#include "core/machine.h"
#include "core/timetravel.h"
#include "core/proc.h"
#include "core/mm.h"
#include "core/cpu.h"
#include "core/console.h"
#include "checkpoint.h"
#include "time_record.h"
#include "entropy_record.h"
#include "laplace/syscalls.h"

void* memset(void* dst, int c, size_t n);

#define SCHED_POINTS_MAX 4096

static machine_mode_t  g_mode = MACHINE_LIVE;
static process_t*      g_cur;
static uint64_t        g_epoch;          /* keyframe epoch the current segment belongs to */
static uint64_t        g_step;           /* index of the next (or pending) entry in this epoch */
static uint64_t        g_insn;           /* position inside the segment after entry g_step-1:
                                           * 1 right after the entry was handled, +1 per
                                           * executed instruction (#228) */
static bool            g_at_entry;       /* stopped at an entry (vs. inside a segment) */
static pending_entry_t g_pending;
static machine_stop_t  g_stop;
static bool            g_started;

static uint64_t        g_ticks;
static uint32_t        g_slice;
static bool            g_need_resched;
static volatile bool   g_stop_request;

static sched_record_t  g_sr;
static uint64_t        g_sr_points[SCHED_POINTS_MAX];

/* Replay target. */
static bool     g_has_target;
static uint64_t g_target_step;
static uint64_t g_target_insn;
static uint64_t g_max_step;

/* #228: single-stepping and hardware breakpoints. */
static bool           g_stepping;        /* TF armed on the current segment */
static bool           g_count_insns;
static uint64_t       g_counted;
static uint32_t       g_seg_hits;        /* breakpoint hits in the current segment */
static machine_hit_hook_fn g_hit_hook;
static void*               g_hit_hook_ctx;
static machine_hwbp_t g_hwbp[MACHINE_MAX_HWBP];
static bool           g_hwbp_armed;      /* some slot is set */
static bool           g_hwbp_active;     /* a search or continue wants them on */
static uint64_t       g_dr_enabled;      /* DR6 B0-B3 bits of the slots DR7 enables now */

/* Scans (#226): called at every replayed entry, before the stop checks. */
static machine_entry_hook_fn g_entry_hook;
static void*                 g_entry_hook_ctx;

/* ---- Accessors ---- */

void machine_init(void) {
    sched_record_init(&g_sr, SCHED_REC_RECORD, g_sr_points, SCHED_POINTS_MAX);
}

machine_mode_t machine_mode(void) { return g_mode; }
void machine_set_mode(machine_mode_t m) {
    g_mode = m;
    sched_record_set_mode(&g_sr, m == MACHINE_LIVE ? SCHED_REC_RECORD : SCHED_REC_REPLAY);
    ktime_set_mode(m == MACHINE_LIVE ? TIME_REC_RECORD : TIME_REC_REPLAY);
    kentropy_set_mode(m == MACHINE_LIVE ? ENTROPY_REC_RECORD : ENTROPY_REC_REPLAY);
}
process_t* machine_current(void) { return g_cur; }
void machine_set_current(process_t* p) { g_cur = p; }
uint64_t machine_epoch(void) { return g_epoch; }
/* Position semantics (#228): at an entry, (step, 0); inside the segment that
 * follows entry s after i instructions, (s, i). */
uint64_t machine_step(void) { return g_at_entry ? g_step : g_step - 1; }
uint64_t machine_insn(void) { return g_at_entry ? 0 : g_insn; }
bool machine_at_entry(void) { return g_at_entry; }
void machine_set_position(uint64_t epoch, uint64_t step) {
    g_epoch = epoch;
    g_step = step;
    g_insn = 0;
    g_at_entry = true;
}
const pending_entry_t* machine_pending(void) { return &g_pending; }
void machine_set_pending(const pending_entry_t* e) { g_pending = *e; }
const machine_stop_t* machine_last_stop(void) { return &g_stop; }
bool machine_started(void) { return g_started; }
sched_record_t* machine_sched_record(void) { return &g_sr; }
bool machine_quiet(void) { return g_mode == MACHINE_REPLAY; }
void machine_request_stop(void) { g_stop_request = true; }

void machine_set_target(uint64_t step, uint64_t insn, uint64_t max_step) {
    g_has_target = true;
    g_target_step = step;
    g_target_insn = insn;
    g_max_step = max_step;
}
void machine_clear_target(void) { g_has_target = false; }

void machine_set_count_insns(bool on) { g_count_insns = on; g_counted = 0; }
void machine_set_hit_hook(machine_hit_hook_fn fn, void* ctx) {
    g_hit_hook = fn;
    g_hit_hook_ctx = ctx;
}
void machine_set_entry_hook(machine_entry_hook_fn fn, void* ctx) {
    g_entry_hook = fn;
    g_entry_hook_ctx = ctx;
}
uint64_t machine_counted_insns(void) { return g_counted; }

/* Scheduler hooks the replay-engine adapter calls (replay_engine_sync.c). */
void scheduler_preempt_set_mode(sched_rec_mode_t mode) { sched_record_set_mode(&g_sr, mode); }
int scheduler_preempt_load(uint64_t epoch, const uint64_t* pts, uint32_t n) {
    return sched_record_load(&g_sr, epoch, pts, n);
}
uint32_t scheduler_preempt_points(const uint64_t** out) { return sched_record_points(&g_sr, out); }

/* ---- Hardware breakpoints (#228) ---- */

void machine_set_hwbp(int slot, const machine_hwbp_t* bp) {
    if (slot < 0 || slot >= MACHINE_MAX_HWBP) return;
    if (bp) g_hwbp[slot] = *bp; else g_hwbp[slot].active = false;
    g_hwbp_armed = false;
    for (int i = 0; i < MACHINE_MAX_HWBP; i++) g_hwbp_armed |= g_hwbp[i].active;
}

void machine_set_hwbp_active(bool on) { g_hwbp_active = on; }
bool machine_hwbps_set(void) { return g_hwbp_armed; }

void machine_clear_hwbps(void) {
    for (int i = 0; i < MACHINE_MAX_HWBP; i++) g_hwbp[i].active = false;
    g_hwbp_armed = false;
}

/* Program DR0-3/DR7 for the process about to run (breakpoints are per pid). */
static void load_hwbps(const process_t* p) {
    uint64_t dr7 = 0;
    if (g_mode != MACHINE_REPLAY || !g_hwbp_active) {
        /* Breakpoints and watchpoints apply to recorded history, and only
         * while a continue or a search runs; plain navigation ignores them
         * (#228). The addresses are cleared too: a CPU may flag a matching
         * breakpoint in DR6 even when DR7 does not enable it. */
        write_dr(7, 0);
        for (int i = 0; i < MACHINE_MAX_HWBP; i++) write_dr(i, 0);
        g_dr_enabled = 0;
        return;
    }
    g_dr_enabled = 0;
    for (int i = 0; i < MACHINE_MAX_HWBP; i++) {
        const machine_hwbp_t* b = &g_hwbp[i];
        if (!g_hwbp_armed || !b->active || (b->pid && p && b->pid != (uint32_t)p->pid)) continue;
        write_dr(i, b->addr);
        uint64_t rw = b->kind & 3;
        uint64_t ln = b->len == 8 ? 2 : b->len == 4 ? 3 : b->len == 2 ? 1 : 0;
        if (rw == 0) ln = 0;
        dr7 |= (1ULL << (i * 2));                  /* local enable */
        dr7 |= (rw | (ln << 2)) << (16 + i * 4);
        g_dr_enabled |= 1ULL << i;
    }
    write_dr(6, 0);
    write_dr(7, dr7);
}

/* ---- Stops ---- */

static void stop_here(stop_reason_t reason, uint64_t code) {
    memset(&g_stop, 0, sizeof(g_stop));
    g_stop.reason = reason;
    g_stop.pid = g_cur ? (uint32_t)g_cur->pid : 0;
    g_stop.vector = g_pending.vector;
    g_stop.error = g_pending.error;
    g_stop.cr2 = g_pending.cr2;
    g_stop.code = code;
    g_stepping = false;
    write_dr(7, 0);
}

/* ---- Entering user mode ---- */

static trap_frame_t* frame_slot(void) {
    return (trap_frame_t*)(cpu_trap_stack_top() - sizeof(trap_frame_t));
}

/* Load the current process's registers into `f` and its address space. */
static void load_current(trap_frame_t* f) {
    ctx_to_frame(&g_cur->context, f);
    if (g_stepping) f->rflags |= RFLAGS_TF;
    vmm_switch_address_space(g_cur->address_space);
    load_hwbps(g_cur);
}

static void enter(void) {
    trap_frame_t* f = frame_slot();
    load_current(f);
    g_cur->state = PROCESS_STATE_RUNNING;
    machine_enter(f);
    /* Back on the monitor stack: the machine stopped. */
    vmm_switch_address_space(0);
}

/* ---- Handling an entry ---- */

void machine_terminate(process_t* p, int code) {
    (void)code;
    tt_before_process_exit(p);
    process_t* next = proc_next_ready(p);
    if (next == p) next = 0;
    if (g_cur == p) g_cur = next;
    proc_destroy(p);
}

/* SYS_WRITE: copy out of user memory in chunks and print (unless replaying). */
static int64_t sys_write(process_t* p, uint64_t buf, uint64_t len) {
    char tmp[256];
    uint64_t done = 0;
    while (done < len) {
        uint64_t n = len - done < sizeof(tmp) ? len - done : sizeof(tmp);
        if (!uaccess_read(p->address_space, buf + done, tmp, n)) return done ? (int64_t)done : -1;
        if (!machine_quiet()) console_write(tmp, (uint32_t)n);
        done += n;
    }
    return (int64_t)done;
}

static int64_t sys_random(process_t* p, uint64_t buf, uint64_t len) {
    uint8_t tmp[256];
    if (len > sizeof(tmp)) return -1;
    if (kentropy_fill(tmp, (uint32_t)len) != ENTROPY_REC_OK) return -1;
    return uaccess_write(p->address_space, buf, tmp, len) ? 0 : -1;
}

typedef enum { AFTER_CONTINUE, AFTER_SWITCHED } after_t;

/* Handle the pending entry of g_cur. Returns AFTER_SWITCHED when the entry
 * itself changed the running process (yield, exit, termination), so no
 * preemption decision follows. */
static after_t handle_entry(void) {
    process_t* p = g_cur;
    process_context_t* c = &p->context;

    if (g_pending.vector != VEC_SYSCALL) {
        /* A fault that was reported (or replayed): the process dies. */
        machine_terminate(p, -1);
        return AFTER_SWITCHED;
    }

    switch (c->rax) {
    case SYS_EXIT:
        machine_terminate(p, (int)c->rdi);
        return AFTER_SWITCHED;
    case SYS_BREAK:
        machine_terminate(p, -2);
        return AFTER_SWITCHED;
    case SYS_WRITE:
        c->rax = (uint64_t)sys_write(p, c->rdi, c->rsi);
        break;
    case SYS_YIELD: {
        c->rax = 0;
        process_t* next = proc_next_ready(p);
        if (next && next != p) {
            p->state = PROCESS_STATE_READY;
            g_cur = next;
        }
        return AFTER_SWITCHED;
    }
    case SYS_GETPID:
        c->rax = (uint64_t)p->pid;
        break;
    case SYS_TIME:
        c->rax = ktime_read();
        break;
    case SYS_RANDOM:
        c->rax = (uint64_t)sys_random(p, c->rdi, c->rsi);
        break;
    default:
        c->rax = (uint64_t)-1;
        break;
    }
    return AFTER_CONTINUE;
}

/* The preemption decision after an entry: the only place the scheduler
 * switches involuntarily. Live: a switch is wanted when the slice expired, and
 * the step is recorded. Replay: switches are forced at the recorded steps. */
static void preempt_decision(uint64_t step) {
    bool want = g_mode == MACHINE_LIVE && g_need_resched;
    if (sched_record_decide(&g_sr, want, step)) {
        process_t* next = proc_next_ready(g_cur);
        if (next && next != g_cur) {
            g_cur->state = PROCESS_STATE_READY;
            g_cur = next;
        }
        g_need_resched = false;
        g_slice = 0;
    }
}

/* Single-step the current segment when the replay target lies inside it, or
 * while counting a segment's instructions (#228). */
static bool want_stepping(void) {
    if (g_mode != MACHINE_REPLAY) return false;
    if (g_count_insns) return true;
    return g_has_target && g_target_insn > 1 && g_step == g_target_step + 1 &&
           g_insn < g_target_insn;
}

/* Complete the pending entry: handle it, decide preemption, advance the clock.
 * Returns false when no process is left to run. */
static bool complete_entry(void) {
    uint64_t step = g_step;
    if (handle_entry() == AFTER_CONTINUE && g_cur) preempt_decision(step);
    g_step = step + 1;
    g_insn = 1;
    g_seg_hits = 0;
    if (!g_cur) {
        g_cur = proc_next_ready(0);
    }
    if (!g_cur) return false;
    g_cur->state = PROCESS_STATE_RUNNING;
    g_at_entry = false;
    g_stepping = want_stepping();
    return true;
}

/* A replay target at the very start of the segment (entry handled, nothing
 * executed yet) is reached without running any instruction. */
static bool target_at_segment_start(void) {
    return g_mode == MACHINE_REPLAY && g_has_target && g_target_insn == 1 &&
           g_step == g_target_step + 1 && g_insn == 1;
}

/* Checks made when an entry arrives, before it is handled. Returns true (and
 * fills g_stop) when the machine must stop here. */
static bool stop_at_entry(void) {
    if (g_mode == MACHINE_REPLAY) {
        if (g_has_target && g_step == g_target_step && g_target_insn == 0) {
            stop_here(STOP_TARGET, 0);
            return true;
        }
        if (g_has_target && g_target_insn > 0 && g_step > g_target_step) {
            /* The segment ended before the requested instruction. */
            stop_here(STOP_END, 0);
            return true;
        }
        if (g_step >= g_max_step) {
            stop_here(STOP_END, 0);
            return true;
        }
        return false;
    }
    /* Live. */
    if (g_stop_request) {
        g_stop_request = false;
        stop_here(STOP_REQUEST, 0);
        return true;
    }
    if (g_pending.vector != VEC_SYSCALL) {
        stop_here(STOP_FAULT, 0);
        return true;
    }
    if (g_cur->context.rax == SYS_BREAK) {
        stop_here(STOP_BREAK, g_cur->context.rdi);
        return true;
    }
    if (g_cur->context.rax == SYS_EXIT && proc_count() == 1) {
        stop_here(STOP_EXITED, g_cur->context.rdi);
        return true;
    }
    return false;
}

/* ---- Public run entry points (monitor stack) ---- */

void machine_start(void) {
    g_cur = proc_next_ready(0);
    if (!g_cur) {
        stop_here(STOP_IDLE, 0);
        return;
    }
    g_started = false;
    g_mode = MACHINE_LIVE;
    enter();
}

void machine_resume(void) {
    if (!g_cur) {
        stop_here(STOP_IDLE, 0);
        return;
    }
    if (!g_at_entry) {
        /* Stopped inside a segment (single-step target or breakpoint): keep
         * executing it. Step over an execution breakpoint with RF. */
        g_stepping = want_stepping();
        g_cur->context.rflags |= RFLAGS_RF;
        enter();
        return;
    }
    if (g_mode == MACHINE_REPLAY && g_has_target &&
        g_step == g_target_step && g_target_insn == 0) {
        stop_here(STOP_TARGET, 0);
        return;
    }
    if (!complete_entry()) {
        stop_here(STOP_IDLE, 0);
        return;
    }
    if (target_at_segment_start()) {
        stop_here(STOP_TARGET, 0);
        return;
    }
    enter();
}

/* ---- Trap dispatch ---- */

static void on_timer(void) {
    g_ticks++;
    if (g_mode == MACHINE_LIVE) {
        if (++g_slice >= MACHINE_SLICE_TICKS) g_need_resched = true;
        tt_on_tick();
        if (serial_rx_ready(PORT_GDB) || serial_rx_ready(PORT_MCP)) g_stop_request = true;
    }
    pic_eoi(0);
}

static void kernel_trap(trap_frame_t* f) {
    panic("trap %lu in kernel mode: err %lx rip %lx cr2 %lx",
          f->vector, f->error, f->rip, read_cr2());
}

/* Save the interrupted registers without the debugger's trap and resume flags. */
static void save_segment_state(const trap_frame_t* f) {
    ctx_from_frame(&g_cur->context, f);
    g_cur->context.rflags &= ~(RFLAGS_TF | RFLAGS_RF);
}

/* #DB from user mode during replay: single-step counting and/or a hardware
 * breakpoint or watchpoint (#228). */
static void on_debug(trap_frame_t* f) {
    uint64_t dr6 = read_dr6();
    write_dr(6, 0);
    bool stepped = (dr6 & (1ULL << 14)) != 0;
    /* Only enabled breakpoints count: B0-B3 may report matches that DR7
     * does not enable (Intel SDM 17.2.3). */
    dr6 = (dr6 & ~0xFULL) | (dr6 & g_dr_enabled);
    if (stepped) {
        g_insn++;
        if (g_count_insns) g_counted++;
    }
    if (dr6 & 0xF) {
        g_seg_hits++;
        if (g_hit_hook) {
            /* Searching: note the hit and keep going (RF steps over an
             * execution breakpoint). The hit may also be the replay target,
             * which is checked below. */
            g_hit_hook(g_hit_hook_ctx, g_step - 1, g_seg_hits, g_stepping ? g_insn : 0, dr6);
            f->rflags |= RFLAGS_RF;
            goto target_check;
        }
        save_segment_state(f);
        stop_here(STOP_BREAKPOINT, 0);
        g_stop.dr6 = dr6;
        g_stop.vector = VEC_DEBUG;
        g_stop.code = g_seg_hits;
        machine_exit();
    }
target_check:
    if (stepped && g_has_target && g_step == g_target_step + 1 && g_insn == g_target_insn) {
        save_segment_state(f);
        stop_here(STOP_TARGET, 0);
        machine_exit();
    }
    if (g_stepping) f->rflags |= RFLAGS_TF;
}

void trap_dispatch(trap_frame_t* f) {
    bool from_user = (f->cs & 3) == 3;
    if (f->vector == VEC_TIMER) {
        on_timer();
        return;
    }
    if (f->vector >= VEC_IRQ_BASE && f->vector < VEC_IRQ_BASE + 16) {
        pic_eoi((uint8_t)(f->vector - VEC_IRQ_BASE));
        return;
    }
    if (!from_user) {
        /* The kernel sets no breakpoints of its own. A #DB here is the second
         * half of a user instruction that both completed a single step and hit
         * a data watchpoint: real CPUs report the two in one #DB, QEMU's TCG
         * delivers a second one at the first instruction of the handler for
         * the first. DR6 already holds both causes for that handler, so return
         * without touching it. */
        if (f->vector == VEC_DEBUG) return;
        kernel_trap(f);
    }

    if (f->vector == VEC_PAGE_FAULT && (f->error & 0x3) == 0x3) {
        /* A user write to a present page: a snapshot-COW capture is invisible
         * to the program and is not a step. */
        uint64_t cr2 = read_cr2();
        if (checkpoint_handle_write_fault(g_cur->address_space, cr2)) return;
    }
    if (f->vector == VEC_DEBUG && g_mode == MACHINE_REPLAY) {
        on_debug(f);
        return;
    }

    /* A kernel entry: one step of the machine. */
    ctx_from_frame(&g_cur->context, f);
    g_cur->context.rflags &= ~(RFLAGS_TF | RFLAGS_RF);
    g_stepping = false;
    g_pending.vector = f->vector;
    g_pending.error = f->error;
    g_pending.cr2 = f->vector == VEC_PAGE_FAULT ? read_cr2() : 0;
    g_insn = 0;
    g_at_entry = true;

    if (g_mode == MACHINE_LIVE) {
        g_started = true;
        tt_on_live_entry();        /* keyframes and writeback happen here */
    } else if (g_entry_hook) {
        g_entry_hook(g_entry_hook_ctx, g_step);
    }
    if (stop_at_entry()) machine_exit();
    if (!complete_entry()) {
        stop_here(STOP_IDLE, 0);
        machine_exit();
    }
    if (target_at_segment_start()) {
        stop_here(STOP_TARGET, 0);
        machine_exit();
    }
    load_current(f);
}
