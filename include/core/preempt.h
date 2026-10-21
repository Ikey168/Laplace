/* Laplace core: preemption at any user instruction, without a counter (#245).
 *
 * While recording, a timer interrupt that lands in user mode becomes a kernel
 * entry of its own (vector VEC_TIMER, a step like any other) when the machine
 * wants one there: the slice expired and another process is ready, a timer
 * deadline passed, a keyframe is due, or a debugger asked to stop. So a
 * process that never makes a system call is preempted, signalled,
 * keyframed, and stoppable like any other.
 *
 * Each such entry journals a preemption record (include/pmatch.h): the
 * registers, the FS/GS bases, the extended-state checksum, and hashes of the
 * pages the process may have written since its last entry (the dirty bits,
 * cleared at every record). A replay arms a hardware breakpoint at the
 * recorded rip for the process that runs the segment, checks each hit, and
 * turns the first matching point into the entry. A recorded point inside a
 * repeated string instruction is reached by single-stepping its iterations.
 * A replay that reaches the segment's next real entry first has diverged:
 * it stops there and is reported in the "inputs" component.
 */

#ifndef CORE_PREEMPT_H
#define CORE_PREEMPT_H

#include <stdint.h>
#include <stdbool.h>
#include "process.h"
#include "pmatch.h"

void preempt_init(void);

/* Recording: journal p's state as the preemption point of entry `step`. */
void preempt_record(process_t* p, uint64_t step);

/* Replay, at the start of the segment p runs before entry `step`: arm the
 * recorded point of that entry, if it is a preemption. */
typedef enum { PREEMPT_NONE = 0, PREEMPT_ARMED, PREEMPT_BAD } preempt_arm_t;
preempt_arm_t preempt_arm(process_t* p, uint64_t step);
void          preempt_disarm(void);
/* The armed point for p: its rip. */
bool          preempt_armed_for(const process_t* p, uint64_t* rip);
bool          preempt_armed(uint64_t* step);

/* Replay: is p, with registers `c`, at the armed point? */
pm_verdict_t  preempt_check(process_t* p, const process_context_t* c);
/* Replay, PM_STEP at a `rep movs`/`rep stos` (forward, 64-bit addressing):
 * run its iterations up to the recorded count here, element by element as
 * the CPU would, advancing rcx/rsi/rdi in *c (also when it stops early).
 * True when the count was reached. */
bool          preempt_rep_forward(process_t* p, process_context_t* c);
/* The length of the repeated string instruction at rip; 0 if it is not one. */
uint32_t      preempt_rep_len(process_t* p, uint64_t rip);

typedef struct {
    uint64_t recorded;          /* preemption points journaled */
    uint64_t recorded_rep;      /*   inside a repeated string instruction */
    uint64_t record_pages;      /*   pages hashed for them */
    uint64_t matched;           /* found again by a replay */
    uint64_t matched_rep;       /*   inside a string instruction */
    uint64_t forwarded;         /*   whose iterations ran in the kernel */
    uint64_t candidates;        /* breakpoint hits checked */
} preempt_stats_t;
const preempt_stats_t* preempt_stats(void);

#endif /* CORE_PREEMPT_H */
