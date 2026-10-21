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
#include "core/kjournal.h"
#include "core/linux.h"
#include "core/vm.h"
#include "core/kfpages.h"
#include "core/kio.h"
#include "core/pctl.h"
#include "core/files.h"
#include "core/clock.h"
#include "iorec.h"

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

/* #241: per-instruction hook while single-stepping (leak diagnosis). */
static machine_insn_hook_fn g_insn_hook;
static void*                g_insn_hook_ctx;

/* #241: rolling hash of the registers at every entry of this epoch, journaled
 * every KJ_TRACE_EVERY steps; on replay, the first step whose hash differs. */
static uint64_t g_trace;
static bool     g_trace_bad;
static uint64_t g_trace_bad_step;

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
    if (step == 0) {
        g_trace = 0;
        g_trace_bad = false;
    }
}

bool machine_trace_mismatch(uint64_t* step) {
    if (g_trace_bad && step) *step = g_trace_bad_step;
    return g_trace_bad;
}

void machine_set_insn_hook(machine_insn_hook_fn fn, void* ctx) {
    g_insn_hook = fn;
    g_insn_hook_ctx = ctx;
}
const pending_entry_t* machine_pending(void) { return &g_pending; }
void machine_set_pending(const pending_entry_t* e) { g_pending = *e; }
const machine_stop_t* machine_last_stop(void) { return &g_stop; }
bool machine_started(void) { return g_started; }
sched_record_t* machine_sched_record(void) { return &g_sr; }
bool machine_quiet(void) { return g_mode == MACHINE_REPLAY; }

uint64_t machine_device_write(uint32_t device, const void* buf, uint64_t len) {
    if (machine_quiet()) return len;          /* the recording already did it */
    switch (device) {
    case MACHINE_DEV_CONSOLE: console_write((const char*)buf, (uint32_t)len); break;
    default: break;
    }
    return len;
}
void machine_request_stop(void) { g_stop_request = true; }

void machine_set_target(uint64_t step, uint64_t insn, uint64_t max_step) {
    g_has_target = true;
    g_target_step = step;
    g_target_insn = insn;
    g_max_step = max_step;
}
void machine_clear_target(void) { g_has_target = false; }

void machine_set_count_insns(bool on) { g_count_insns = on; g_counted = 0; }

static bool want_stepping(void);
void machine_stop_counting(void) {
    g_count_insns = false;
    if (!want_stepping()) g_stepping = false;   /* the next #DB does not re-arm TF */
}
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

/* ---- x87/SSE/AVX ownership (#237) ---- */

/* The process whose state the FPU registers hold (see core/fpu.h). */
static process_t* g_fpu_owner;
/* FS/GS base (#238): written to the MSRs when a different process runs, or
 * when arch_prctl changes them. */
static process_t* g_seg_owner;

void machine_fpu_flush(void) {
    if (g_fpu_owner) fpu_save(proc_arch(g_fpu_owner)->fpu);
}

void machine_fpu_forget(process_t* p) {
    if (g_fpu_owner == p) g_fpu_owner = 0;
    if (g_seg_owner == p) g_seg_owner = 0;
}

void machine_load_segment_bases(process_t* p) {
    wrmsr(MSR_FS_BASE, proc_arch(p)->fs_base);
    wrmsr(MSR_GS_BASE, proc_arch(p)->gs_base);
    g_seg_owner = p;
}

static void fpu_switch_to(process_t* p) {
    if (g_fpu_owner == p) return;
    if (g_fpu_owner) fpu_save(proc_arch(g_fpu_owner)->fpu);
    fpu_restore(proc_arch(p)->fpu);
    g_fpu_owner = p;
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
    machine_fpu_flush();   /* debuggers read the saved areas */
}

/* ---- Entering user mode ---- */

static trap_frame_t* frame_slot(void) {
    return (trap_frame_t*)(cpu_trap_stack_top() - sizeof(trap_frame_t));
}

/* Load the current process's registers into `f` and its address space. */
static void load_current(trap_frame_t* f) {
    ctx_to_frame(&g_cur->context, f);
    fpu_switch_to(g_cur);
    if (g_seg_owner != g_cur) machine_load_segment_bases(g_cur);
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

void machine_stop_diverged(void) {
    stop_here(STOP_DIVERGED, 0);
    machine_exit();
    for (;;) {}
}

/* p is gone: with a living parent it waits as a zombie for wait4 (#248);
 * its own children go to pid 1, or are destroyed if no one can reap them. */
static void end_process(process_t* p, uint32_t status) {
    tt_before_process_exit(p);
    process_t* init = proc_by_pid(1);
    bool init_reaps = init && init != p && init->state != PROCESS_STATE_ZOMBIE;
    for (uint32_t i = 0; i < proc_count();) {
        process_t* c = proc_at(i);
        if (c->ppid != p->pid || c == p) { i++; continue; }
        c->ppid = init_reaps ? 1 : 0;
        if (!init_reaps && c->state == PROCESS_STATE_ZOMBIE) {
            proc_destroy(c);                 /* the table shifted: same index */
            continue;
        }
        if (init_reaps && c->state == PROCESS_STATE_ZOMBIE) pctl_child_exited(c);
        i++;
    }
    process_t* next = proc_next_ready(p);
    if (next == p) next = 0;
    if (g_cur == p) g_cur = next;
    process_t* parent = p->ppid ? proc_by_pid((uint32_t)p->ppid) : 0;
    if (!parent || parent->state == PROCESS_STATE_ZOMBIE) {
        proc_destroy(p);
        return;
    }
    /* A zombie keeps its pid and status, and nothing else. */
    files_proc_gone(p);
    machine_fpu_forget(p);
    vm_space_t* empty = vmm_create_address_space((uint32_t)p->pid);
    vmm_destroy_address_space(p->address_space);
    p->address_space = empty;
    p->state = PROCESS_STATE_ZOMBIE;
    p->wait_status = status;
    p->wait_reason = 0;
    p->wait_obj = 0;
    p->wait_deadline = 0;
    p->wait_aux = 0;
    pctl_child_exited(p);
}

void machine_terminate(process_t* p, int code) {
    end_process(p, ((uint32_t)code & 0xFF) << 8);
}

void machine_kill(process_t* p, uint32_t sig) {
    end_process(p, sig & 0x7F);
}

/* ---- Blocking and idle (#246) ---- */

#define WAKE_TIMER 1                  /* kio_event nr: a deadline passed */

void machine_block(process_t* p, uint32_t reason, uint64_t obj, uint64_t deadline_ns) {
    p->state = PROCESS_STATE_BLOCKED;
    p->wait_reason = reason;
    p->wait_obj = obj;
    p->wait_deadline = deadline_ns;
    if (g_cur == p) {
        process_t* next = proc_next_ready(p);
        g_cur = next != p ? next : 0;
    }
}

void machine_wake(process_t* p, int64_t ret) {
    if (p->state != PROCESS_STATE_BLOCKED) return;
    p->context.rax = (uint64_t)ret;
    p->state = PROCESS_STATE_READY;
    p->wait_reason = 0;
    p->wait_obj = 0;
    p->wait_deadline = 0;
}

int64_t machine_timeout_result(uint32_t reason) {
    return reason == WAIT_SLEEP ? 0 : -110;   /* ETIMEDOUT */
}

static bool any_deadline(void) {
    for (uint32_t i = 0; i < proc_count(); i++) {
        process_t* p = proc_at(i);
        if (p->state == PROCESS_STATE_BLOCKED && p->wait_deadline) return true;
    }
    return false;
}

static bool any_blocked(void) {
    for (uint32_t i = 0; i < proc_count(); i++) {
        if (proc_at(i)->state == PROCESS_STATE_BLOCKED) return true;
    }
    return false;
}

/* Wakes due at `step`. Recording: deadlines that passed, by the clock, each
 * journaled. Replaying: the journaled ones for this step. */
static void deliver_wakes(uint64_t step) {
    if (g_mode == MACHINE_LIVE) {
        if (!any_deadline()) return;
        uint64_t now = clock_now_unrecorded();
        for (uint32_t i = 0; i < proc_count(); i++) {
            process_t* p = proc_at(i);
            if (p->state != PROCESS_STATE_BLOCKED || !p->wait_deadline || p->wait_deadline > now) continue;
            kio_event(IOREC_DEVICE, WAKE_TIMER, (uint64_t)p->pid, step);
            machine_wake(p, machine_timeout_result(p->wait_reason));
        }
        return;
    }
    uint32_t nr;
    uint64_t pid;
    while (kio_event_next(IOREC_DEVICE, step, &nr, &pid)) {
        process_t* p = proc_by_pid((uint32_t)pid);
        if (nr != WAKE_TIMER || !p || p->state != PROCESS_STATE_BLOCKED) machine_stop_diverged();
        machine_wake(p, machine_timeout_result(p->wait_reason));
    }
}

static stop_reason_t g_pick_stop = STOP_IDLE;

/* Recording, nothing runnable: halt until a wake (or a debugger's request).
 * No steps pass. */
static bool idle_until_wake(void) {
    for (;;) {
        if (g_stop_request) {
            g_stop_request = false;
            g_pick_stop = STOP_REQUEST;
            return false;
        }
        if (!any_deadline()) {
            g_pick_stop = STOP_IDLE;          /* nothing can ever wake them */
            return false;
        }
        deliver_wakes(g_step);
        if (proc_next_ready(0)) return true;
        __asm__ volatile("sti; hlt; cli" ::: "memory");
    }
}

/* Choose the process to run next, idling if they all wait. False (with
 * g_pick_stop) when the machine must stop instead. */
static bool pick_next(void) {
    if (!g_cur) g_cur = proc_next_ready(0);
    while (!g_cur) {
        if (!any_blocked()) {
            g_pick_stop = STOP_IDLE;
            return false;
        }
        if (g_mode == MACHINE_LIVE) {
            if (!idle_until_wake()) return false;
        } else {
            deliver_wakes(g_step);
            if (!proc_next_ready(0)) {
                /* The recording stopped here, everything waiting. */
                g_pick_stop = g_has_target && g_step == g_target_step && g_target_insn == 0
                                  ? STOP_TARGET : STOP_END;
                return false;
            }
        }
        g_cur = proc_next_ready(0);
    }
    return true;
}

void machine_yield(process_t* p) {
    process_t* next = proc_next_ready(p);
    if (next && next != p) {
        p->state = PROCESS_STATE_READY;
        g_cur = next;
    }
}

/* The pending entry ends its process: exit in either ABI. */
static bool entry_is_exit(void) {
    if (g_pending.vector == VEC_SYSCALL) return g_cur->context.rax == SYS_EXIT;
    if (g_pending.vector == VEC_LINUX_SYSCALL) return linux_is_exit(g_cur->context.rax);
    return false;
}

/* SYS_WRITE: copy out of user memory in chunks and print (unless replaying). */
static int64_t sys_write(process_t* p, uint64_t buf, uint64_t len) {
    char tmp[256];
    uint64_t done = 0;
    while (done < len) {
        uint64_t n = len - done < sizeof(tmp) ? len - done : sizeof(tmp);
        if (!uaccess_read(p->address_space, buf + done, tmp, n)) return done ? (int64_t)done : -1;
        machine_device_write(MACHINE_DEV_CONSOLE, tmp, n);
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

/* ---- Emulated instructions (#241) ----
 *
 * CR4.TSD makes user RDTSC/RDTSCP raise #GP, and CPUID faulting does the same
 * for CPUID. The kernel answers them from the journaled clock and the recorded
 * CPUID table, so a program that reads the time stamp counter or the CPU's
 * identity directly replays exactly. Each one is a kernel entry, so a step. */

typedef enum { EMUL_NONE = 0, EMUL_RDTSC, EMUL_RDTSCP, EMUL_CPUID } emul_t;

static emul_t emulated_insn(process_t* p, uint64_t* len) {
    if (g_pending.vector != VEC_GPF || g_pending.error != 0) return EMUL_NONE;
    uint8_t b[3] = { 0, 0, 0 };
    if (!uaccess_read(p->address_space, p->context.rip, b, 2)) return EMUL_NONE;
    if (b[0] != 0x0F) return EMUL_NONE;
    if (b[1] == 0x31) { *len = 2; return EMUL_RDTSC; }
    if (b[1] == 0xA2) { *len = 2; return cpu_cpuid_faulting() ? EMUL_CPUID : EMUL_NONE; }
    if (b[1] == 0x01 && uaccess_read(p->address_space, p->context.rip + 2, &b[2], 1) &&
        b[2] == 0xF9) {
        *len = 3;
        return EMUL_RDTSCP;
    }
    return EMUL_NONE;
}

static void emulate(process_t* p, emul_t kind, uint64_t len) {
    process_context_t* c = &p->context;
    if (kind == EMUL_CPUID) {
        uint32_t r[4];
        cpu_cpuid_lookup((uint32_t)c->rax, (uint32_t)c->rcx, r);
        c->rax = r[0]; c->rbx = r[1]; c->rcx = r[2]; c->rdx = r[3];
    } else {
        uint64_t t = ktime_read();
        c->rax = (uint32_t)t;
        c->rdx = t >> 32;
        if (kind == EMUL_RDTSCP) c->rcx = 0;   /* IA32_TSC_AUX: CPU 0 */
    }
    c->rip += len;
}

bool machine_entry_is_fault(void) {
    uint64_t len;
    return g_cur && !vec_is_syscall(g_pending.vector) && emulated_insn(g_cur, &len) == EMUL_NONE;
}

/* ---- Register trace (#241) ---- */

static uint64_t mix64(uint64_t h, uint64_t v) {
    h ^= v + 0x9E3779B97F4A7C15ULL + (h << 6) + (h >> 2);
    h ^= h >> 29;
    h *= 0xBF58476D1CE4E5B9ULL;
    return h ^ (h >> 32);
}

static void trace_entry(uint64_t step) {
    const process_context_t* c = &g_cur->context;
    const uint64_t regs[] = {
        step, (uint64_t)g_cur->pid, g_pending.vector,
        c->rax, c->rbx, c->rcx, c->rdx, c->rsi, c->rdi, c->rbp, c->rsp,
        c->r8, c->r9, c->r10, c->r11, c->r12, c->r13, c->r14, c->r15,
        c->rip, c->rflags & 0xED5ULL,
    };
    for (uint32_t i = 0; i < sizeof(regs) / sizeof(regs[0]); i++) g_trace = mix64(g_trace, regs[i]);
    if ((step + 1) % KJ_TRACE_EVERY != 0) return;
    if (g_mode == MACHINE_LIVE) {
        kj_append(KJ_EV_TRACE, step, g_trace, 0);
        return;
    }
    uint64_t want;
    if (!g_trace_bad && kj_find(KJ_EV_TRACE, step, &want) && want != g_trace) {
        g_trace_bad = true;
        g_trace_bad_step = step;
    }
}

typedef enum { AFTER_CONTINUE, AFTER_SWITCHED } after_t;

/* Handle the pending entry of g_cur. Returns AFTER_SWITCHED when the entry
 * itself changed the running process (yield, exit, termination), so no
 * preemption decision follows. */
static after_t handle_entry(void) {
    process_t* p = g_cur;
    process_context_t* c = &p->context;

    if (g_pending.vector == VEC_LINUX_SYSCALL) {
        return linux_syscall(p) == LX_SWITCHED ? AFTER_SWITCHED : AFTER_CONTINUE;
    }
    if (g_pending.vector != VEC_SYSCALL) {
        uint64_t len;
        emul_t kind = emulated_insn(p, &len);
        if (kind != EMUL_NONE) {
            emulate(p, kind, len);
            return AFTER_CONTINUE;
        }
        /* A fault that was reported (or replayed): the process dies of it. */
        machine_kill(p, g_pending.vector == VEC_PAGE_FAULT ? 11 /* SIGSEGV */ : 4 /* SIGILL */);
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
    case SYS_YIELD:
        c->rax = 0;
        machine_yield(p);
        return AFTER_SWITCHED;
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
static void start_segment(void) {
    g_cur->state = PROCESS_STATE_RUNNING;
    g_at_entry = false;
    g_stepping = want_stepping();
    if (g_stepping && g_insn_hook) g_insn_hook(g_insn_hook_ctx, g_cur, g_cur->context.rip);
}

static bool complete_entry(void) {
    uint64_t step = g_step;
    deliver_wakes(step);                  /* before the entry: same order in a replay */
    trace_entry(step);
    if (handle_entry() == AFTER_CONTINUE && g_cur) preempt_decision(step);
    g_step = step + 1;
    g_insn = 1;
    g_seg_hits = 0;
    if (!pick_next()) return false;
    start_segment();
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
    if (machine_entry_is_fault()) {
        stop_here(STOP_FAULT, 0);
        return true;
    }
    if (g_pending.vector == VEC_SYSCALL && g_cur->context.rax == SYS_BREAK) {
        stop_here(STOP_BREAK, g_cur->context.rdi);
        return true;
    }
    if (entry_is_exit() && proc_live_count() == 1) {
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
        /* Stopped while everything waited: wait again, then run. */
        if (!pick_next()) {
            stop_here(g_pick_stop, 0);
            return;
        }
        start_segment();
        if (target_at_segment_start()) {
            stop_here(STOP_TARGET, 0);
            return;
        }
        enter();
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
        stop_here(g_pick_stop, 0);
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
        if (g_insn_hook) g_insn_hook(g_insn_hook_ctx, g_cur, f->rip);
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
        if (cpu_msr_probe_recover(f)) return;
        kernel_trap(f);
    }

    if (f->vector == VEC_PAGE_FAULT && (f->error & 0x3) == 0x3) {
        /* A user write to a present page: a snapshot-COW capture is invisible
         * to the program and is not a step. */
        uint64_t cr2 = read_cr2();
        if (vmm_user_write_fault(g_cur->address_space, cr2)) return;
    }
    if (f->vector == VEC_PAGE_FAULT && !(f->error & 0x1)) {
        /* First touch of a page in a region that allows the access (#239):
         * a function of the program and of which pages are present, so like
         * a capture it is not a step. */
        if (vm_fault_in(g_cur->address_space, read_cr2(), (f->error & 0x2) != 0)) return;
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
        stop_here(g_pick_stop, 0);
        machine_exit();
    }
    if (target_at_segment_start()) {
        stop_here(STOP_TARGET, 0);
        machine_exit();
    }
    load_current(f);
}
