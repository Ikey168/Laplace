/* Laplace core: the recorded machine's execution engine (#220, #222, #225).
 *
 * Steps. Every kernel entry a user process makes (a system call, or a fault)
 * is one step. A position (epoch, step) is the machine state at the moment the
 * step-th entry of that epoch has trapped into the kernel and has not been
 * handled yet. Keyframes are taken at entries, so a keyframe is exactly
 * (epoch, 0).
 *
 * Determinism. Timer interrupts never change user-visible state: they only
 * mark the time slice expired. Context switches happen only at entries, through
 * sched_record_decide(), which records the step of every preemption on a live
 * run and forces the same switches on replay. The clock and entropy system calls
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
    STOP_ERROR          /* replay could not continue */
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
uint64_t machine_counted_insns(void);

/* Console output from user processes is suppressed while replaying. */
bool machine_quiet(void);

/* Terminate a process (fault or break handled, or exit). */
void machine_terminate(process_t* p, int code);

#endif /* CORE_MACHINE_H */
