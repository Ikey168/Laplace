/* IKOS Orthogonal Persistence - Reverse Breakpoints and Watchpoints (#171)
 *
 * Answers "where did this last happen?" by scanning backward through the
 * retained history. Both plug a predicate into the reverse-continue seam (#170):
 *
 *   - Reverse breakpoint: step backward to the most recent position before the
 *     current point where a condition holds (for example a PC or state check).
 *   - Reverse watchpoint: step backward to the most recent position at or before
 *     the current point where a watched value changed, i.e. the last write to
 *     it, answering "where did this value last change?".
 *
 * The search is bounded by the retained keyframe ring (#168): reverse-step stops
 * at the oldest retained moment, so a miss returns REVBREAK_NOT_FOUND rather than
 * running off the end.
 *
 * Pure and host-testable: the condition and the watched-value probe are
 * injected, reading the system state restored at each candidate position. The
 * kernel adapter (revbreak_sync.c) binds a reverse context for a debugger front
 * end.
 *
 * See docs/architecture/time-travel.md.
 */

#ifndef REVBREAK_H
#define REVBREAK_H

#include <stdint.h>
#include <stdbool.h>

#include "reverse.h"

#define REVBREAK_OK          0
#define REVBREAK_NOT_FOUND  -1   /* no hit within the retained window */
#define REVBREAK_ERR_PARAM  -2
#define REVBREAK_ERR        -3   /* an underlying rewind/replay failed */

/* Breakpoint condition, evaluated on the system state restored at the current
 * position. Return true to stop. */
typedef bool (*revbreak_cond_fn)(void* ctx);

/* Reads the watched value from the system state restored at the current
 * position (for a watchpoint). */
typedef uint64_t (*revbreak_probe_fn)(void* ctx);

/* Step backward to the most recent position strictly before the current point
 * where cond() holds. Leaves the system there and, if hit != NULL, writes the
 * position. Returns REVBREAK_NOT_FOUND if none within the retained ring. */
int reverse_breakpoint(reverse_ctx_t* rv, revbreak_cond_fn cond, void* ctx,
                       reverse_pos_t* hit);

/* Step backward to the most recent position at or before the current point where
 * the probed value changed (the last write to it). Leaves the system at that
 * write and, if hit != NULL, writes the position. Returns REVBREAK_NOT_FOUND if
 * the value never changed within the retained ring. */
int reverse_watchpoint(reverse_ctx_t* rv, revbreak_probe_fn probe, void* ctx,
                       reverse_pos_t* hit);

/* ---- Scan fast path (#226) ----
 *
 * The functions above walk backward one position at a time, and every step is a
 * full rewind (restore a keyframe, replay to the position): O(n^2) replay work
 * for an n-step epoch. A scanner instead re-executes an epoch once, forward,
 * visiting every step position in order with the system state there; the
 * search then walks epochs backward, newest first, and each epoch is replayed
 * once. Same results as the step-by-step functions.
 *
 * scan(sctx, epoch, limit, visit, vctx) replays `epoch` from its keyframe and
 * calls visit(vctx, pos) at (epoch, 0), (epoch, 1), ..., (epoch, limit - 1),
 * with the system restored at each. Returns 0 on success, negative on failure. */
typedef void (*revbreak_visit_fn)(void* vctx, reverse_pos_t pos);
typedef int  (*revbreak_scan_fn)(void* sctx, uint64_t epoch, uint64_t limit,
                                 revbreak_visit_fn visit, void* vctx);

int reverse_breakpoint_scan(reverse_ctx_t* rv, revbreak_scan_fn scan, void* sctx,
                            revbreak_cond_fn cond, void* ctx, reverse_pos_t* hit);
int reverse_watchpoint_scan(reverse_ctx_t* rv, revbreak_scan_fn scan, void* sctx,
                            revbreak_probe_fn probe, void* ctx, reverse_pos_t* hit);

/* ---- Kernel adapter (revbreak_sync.c) ----
 * Reverse breakpoint / watchpoint bound to a reverse context, for the debugger
 * front end (the GDB bridge in #172). */
void krevbreak_bind(reverse_ctx_t* rv);
/* Install the kernel's epoch scanner; krevbreak_* then use the scan fast path. */
void krevbreak_set_scanner(revbreak_scan_fn scan, void* sctx);
int  krevbreak_breakpoint(revbreak_cond_fn cond, void* ctx, reverse_pos_t* hit);
int  krevbreak_watchpoint(revbreak_probe_fn probe, void* ctx, reverse_pos_t* hit);

#endif /* REVBREAK_H */
