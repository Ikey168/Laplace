/* Laplace core: POSIX signals for Linux programs (#250).
 *
 * Per process: 64 actions (handler, flags, restorer, mask), the blocked and
 * pending sets with each pending signal's info, the alternate stack, and up
 * to eight timers (setitimer's ITIMER_REAL and timer_create's). All of it is
 * kernel state: the "signals" keyframe section, its own divergence
 * component.
 *
 * Delivery happens only at deterministic points: whenever a process is about
 * to run after a kernel entry. A handler gets a frame laid out per the Linux
 * x86-64 ABI (return address = sa_restorer, ucontext with sigcontext and the
 * signal mask, siginfo, and the x87/SSE/AVX state with the XSAVE markers),
 * and rt_sigreturn restores from it. A waiting process that a signal
 * interrupts returns -EINTR.
 *
 * Synchronous signals come from faults, at the faulting instruction: #PF ->
 * SIGSEGV with si_addr, #GP -> SIGSEGV, #DE/#MF/#XM -> SIGFPE, #UD ->
 * SIGILL, int3 -> SIGTRAP, #AC -> SIGBUS. With no handler, a fault stops the
 * machine for the debugger (STOP_FAULT, reporting the signal), and so does
 * any other signal whose default action dumps core (SIGABRT, ...): the
 * monitor reports it as STOP_SIGNAL. Other default-fatal signals end the
 * process with the signal as its wait status.
 *
 * Timer expiries are inputs: noticed against the clock while recording,
 * journaled with the step they are delivered at, and replayed there.
 */

#ifndef CORE_KSIG_H
#define CORE_KSIG_H

#include <stdint.h>
#include <stdbool.h>
#include "process.h"
#include "core/linux.h"

#define KS_ID_SIGNALS 8

#define SIG_SIGHUP   1
#define SIG_SIGINT   2
#define SIG_SIGQUIT  3
#define SIG_SIGILL   4
#define SIG_SIGTRAP  5
#define SIG_SIGABRT  6
#define SIG_SIGBUS   7
#define SIG_SIGFPE   8
#define SIG_SIGKILL  9
#define SIG_SIGUSR1  10
#define SIG_SIGSEGV  11
#define SIG_SIGUSR2  12
#define SIG_SIGPIPE  13
#define SIG_SIGALRM  14
#define SIG_SIGTERM  15
#define SIG_SIGCHLD  17
#define SIG_SIGCONT  18
#define SIG_SIGSTOP  19

/* si_code values. */
#define SI_USER      0
#define SI_KERNEL    0x80
#define SI_TIMER     (-2)
#define SI_TKILL     (-6)

void ksig_init(void);
void ksig_proc_new(process_t* p);
void ksig_proc_gone(process_t* p);
void ksig_fork(process_t* parent, process_t* child);
void ksig_exec(process_t* p);
/* A new thread (#251): the group's handlers and the creator's mask. */
void ksig_thread(process_t* parent, process_t* child);

bool ksig_syscall(process_t* p, uint64_t nr, lx_after_t* after);

/* Send `sig` to t from outside a fault (kill, a timer, a child's exit).
 * SIGKILL ends t at once; others become pending, and wake t if it waits and
 * the signal would do something. */
void ksig_send(process_t* t, uint32_t sig, int32_t code, uint64_t val);

/* A fault became `sig`. True when p handles it (delivered when p next runs);
 * false when the default action applies. */
bool ksig_fault(process_t* p, uint32_t sig, int32_t code, uint64_t addr);
/* Would a fault becoming `sig` be handled? (Recording stops at unhandled
 * faults before handling the entry.) */
bool ksig_fault_handled(const process_t* p, uint32_t sig);

typedef enum {
    KSIG_NONE = 0,      /* nothing to do */
    KSIG_DELIVERED,     /* a handler frame was built */
    KSIG_KILLED,        /* the default action ended the process */
    KSIG_STOP           /* a core-dumping default action: stop the machine */
} ksig_result_t;
/* Deliver p's next pending signal, if any. *sig gets the signal. */
ksig_result_t ksig_deliver(process_t* p, uint32_t* sig);

/* Timers (#246's deadlines): the earliest armed deadline, and firing one. */
bool ksig_next_deadline(const process_t* p, uint64_t* deadline);
/* Fire p's timers due at `now`; `fired(pid, index)` journals each. */
void ksig_fire_due(process_t* p, uint64_t now, void (*fired)(uint32_t pid, uint32_t index));
void ksig_fire(process_t* p, uint32_t index);

const char* ksig_name(uint32_t sig);

#endif /* CORE_KSIG_H */
