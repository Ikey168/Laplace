/* Laplace core: the recorded machine's execution engine (#220, #222, #225).
 *
 * Steps. Every kernel entry a user process makes (a system call, or a fault)
 * is one step, and so is a preemption point (#245, include/core/preempt.h). A position (epoch, step) is the machine state at the moment the
 * step-th entry of that epoch has trapped into the kernel and has not been
 * handled yet. Keyframes are taken at entries, so a keyframe is exactly
 * (epoch, 0).
 *
 * Determinism. Context switches happen only at entries, through
 * sched_record_decide(), which records the step of every preemption on a live
 * run and forces the same switches on replay. A timer interrupt in user mode
 * either only marks the time slice expired, or becomes an entry whose state
 * the journal records, so a replay turns the same point into the same entry. The clock and entropy system calls
 * go through ktime_read() and kentropy_fill(). Everything else a process can
 * observe is a pure function of its memory and registers, so replaying the
 * journal from a keyframe re-executes the run exactly.
 *
 * Coroutine. The debug monitor (and the boot path) own the boot stack. They
 * call machine_resume()/machine_run_to(), which enter user mode through
 * machine_enter(); the machine runs until it stops, and trap_dispatch() then
 * returns control with machine_exit().
 */

#ifndef CORE_MACHINE_H
#define CORE_MACHINE_H

#include <stdint.h>
#include <stdbool.h>
#include "process.h"
#include "sched_record.h"

typedef enum {
    MACHINE_LIVE = 0,   /* running for real and recording */
    MACHINE_REPLAY      /* re-executing recorded history */
} machine_mode_t;

typedef enum {
    STOP_NONE = 0,
    STOP_BREAK,         /* SYS_BREAK (a failed assertion) */
    STOP_FAULT,         /* a user-mode exception */
    STOP_EXITED,        /* the last process is exiting */
    STOP_REQUEST,       /* a debugger connected or asked to stop */
    STOP_TARGET,        /* replay reached the requested position */
    STOP_BREAKPOINT,    /* a hardware breakpoint or watchpoint hit (#228) */
    STOP_END,           /* replay reached the end of the recording */
    STOP_IDLE,          /* nothing left to run */
    STOP_ERROR,         /* replay could not continue */
    STOP_DIVERGED,      /* replay: an input call is not the recorded one (#242) */
    STOP_SIGNAL         /* a signal whose default action dumps core (#250); code = signal */
} stop_reason_t;

typedef struct {
    stop_reason_t reason;
    uint32_t pid;
    uint64_t vector;    /* entry vector: 128 for a system call */
    uint64_t error;
    uint64_t cr2;
    uint64_t code;      /* SYS_BREAK argument */
    uint64_t dr6;       /* debug status on a breakpoint stop */
} machine_stop_t;

/* The kernel entry the current process is stopped at. */
typedef struct {
    uint64_t vector;
    uint64_t error;
    uint64_t cr2;
} pending_entry_t;

/* Scheduling quantum, in timer ticks. */
#define MACHINE_SLICE_TICKS 1
#define MACHINE_TIMER_HZ    1000

void machine_init(void);

machine_mode_t machine_mode(void);
void           machine_set_mode(machine_mode_t mode);

/* Current process (the one at the pending entry), and position. */
process_t* machine_current(void);
void       machine_set_current(process_t* p);
uint64_t   machine_epoch(void);
uint64_t   machine_step(void);
uint64_t   machine_insn(void);         /* instructions past the entry (#228) */
bool       machine_at_entry(void);     /* stopped at an entry, not inside a segment */
void       machine_set_position(uint64_t epoch, uint64_t step);
const pending_entry_t* machine_pending(void);
/* Stop a replay whose program made an input call the recording does not
 * have at this step (#242). Does not return. */
void machine_stop_diverged(void);
void       machine_set_pending(const pending_entry_t* e);
const machine_stop_t*  machine_last_stop(void);
bool       machine_started(void);     /* the first entry has happened */

/* The scheduler's preemption record (RECORD on live runs, REPLAY on replays). */
sched_record_t* machine_sched_record(void);

/* Live: start the machine from the freshly spawned processes; returns when it
 * stops. */
void machine_start(void);

/* Handle the pending entry, then run until the next stop. Live: records.
 * Replay: stops at the target set by machine_set_target(). */
void machine_resume(void);

/* Replay targets: stop at (current epoch, step). With insn > 0 the target lies
 * in the segment after entry `step`: insn 1 is right after the entry was
 * handled, insn k after k-1 further user instructions (#228). max_step bounds
 * the replay: reaching it without hitting the target stops with STOP_END. */
void machine_set_target(uint64_t step, uint64_t insn, uint64_t max_step);
void machine_clear_target(void);

/* Verify (#245): a preemption point a replay cannot find is noted (as an
 * input mismatch) and taken just before the entry that arrived instead, so
 * the epoch still runs to its end. Off: the replay stops there. */
void machine_set_lenient(bool on);

/* Ask a live machine to stop at its next kernel entry. */
void machine_request_stop(void);

/* Hardware breakpoints / watchpoints armed during replay (#228). */
#define MACHINE_MAX_HWBP 4
typedef struct {
    bool     active;
    uint32_t pid;       /* 0: any process */
    uint64_t addr;
    uint8_t  kind;      /* 0 exec, 1 write, 3 read/write */
    uint8_t  len;       /* 1, 2, 4, 8 */
} machine_hwbp_t;
void machine_set_hwbp(int slot, const machine_hwbp_t* bp);
void machine_clear_hwbps(void);
bool machine_hwbps_set(void);
/* Arm the set breakpoints for the next runs (continue / search only). */
void machine_set_hwbp_active(bool on);

/* A callback at every replayed entry, before the stop checks (#226 scans). */
typedef void (*machine_entry_hook_fn)(void* ctx, uint64_t step);
void machine_set_entry_hook(machine_entry_hook_fn fn, void* ctx);

/* Breakpoint searches (#228): while set, a hardware breakpoint or watchpoint
 * hit during replay calls fn(ctx, segment, ordinal, dr6) and execution
 * continues. `segment` is the entry index the segment follows; `ordinal`
 * counts hits within that segment from 1; `insn` is the exact position when
 * the segment is being single-stepped (instruction counting), else 0. */
typedef void (*machine_hit_hook_fn)(void* ctx, uint64_t segment, uint32_t ordinal, uint64_t insn,
                                    uint64_t dr6);
void machine_set_hit_hook(machine_hit_hook_fn fn, void* ctx);

/* Instruction counting for the current segment during replay (#228). */
void     machine_set_count_insns(bool on);
/* From the instruction hook: stop counting, and single-stepping unless a
 * replay target needs it. */
void     machine_stop_counting(void);
uint64_t machine_counted_insns(void);

/* #241: the entry pending now is a real fault (not an emulated RDTSC, RDTSCP,
 * or CPUID). */
bool machine_entry_is_fault(void);

/* #241: while replaying, the first step at which the register trace differed
 * from the recording's (since the current epoch began). */
bool machine_trace_mismatch(uint64_t* step);

/* #241: called before every user instruction executes while single-stepping
 * (the segment's first instruction, then after every step). */
typedef void (*machine_insn_hook_fn)(void* ctx, process_t* p, uint64_t rip);
void machine_set_insn_hook(machine_insn_hook_fn fn, void* ctx);

/* #237: write the FPU owner's registers into its saved area (before reading
 * or capturing areas), and drop a process that is going away as the owner. */
void machine_fpu_flush(void);
void machine_fpu_forget(process_t* p);

/* Console output from user processes is suppressed while replaying. */
bool machine_quiet(void);

/* Programs reach devices outside the machine (the console now, the network
 * later) only through here. While replaying, nothing goes out: a replay never
 * repeats an external side effect (#242). Returns the bytes accepted. */
uint64_t machine_device_write(uint32_t device, const void* buf, uint64_t len);
#define MACHINE_DEV_CONSOLE 0

/* Blocking (#246). A process that waits leaves the run queue with a reason,
 * the object it waits on, and an optional deadline (monotonic ns, 0: none).
 * Another process's call wakes it deterministically; a deadline passing is an
 * input: the kernel notices it at an entry or while idle, journals the wake
 * with the step it is delivered at (kio_event), and a replay delivers it at
 * that step without waiting. When nothing can run, the machine idles with
 * hlt: idle time adds no steps. */
#define WAIT_SLEEP  1         /* nanosleep: a timeout returns 0 */
#define WAIT_CHILD  2         /* wait4 (#248) */
#define WAIT_FUTEX  3         /* futex (#251) */
#define WAIT_INPUT  4         /* console read (#249) */
#define WAIT_POLL   5         /* poll, select (#249) */
#define WAIT_SIGNAL 6         /* pause, sigsuspend (#250) */
void machine_block(process_t* p, uint32_t reason, uint64_t obj, uint64_t deadline_ns);
/* Make a blocked process runnable; its system call returns `ret`. */
void machine_wake(process_t* p, int64_t ret);
/* What a wait returns when its deadline passes. */
int64_t machine_timeout_result(uint32_t reason);

/* Give the CPU to the next ready process (a yield). */
void machine_yield(process_t* p);

/* Load p's FS/GS base into the MSRs (#238). */
void machine_load_segment_bases(process_t* p);

/* Terminate a process (fault or break handled, or exit). */
void machine_terminate(process_t* p, int code);
/* End p by a signal (wait status: the signal number). */
/* The signal the pending fault raises (#250). */
uint32_t   machine_fault_signal(void);
void       machine_kill(process_t* p, uint32_t sig);

#endif /* CORE_MACHINE_H */
