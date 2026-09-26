/* IKOS Orthogonal Persistence - Reverse Execution (#170, epic #159)
 *
 * Classic reverse debugging on top of the rewind verb (#169): to step backward,
 * restore the prior keyframe and replay forward to just before the current
 * point. Because every reverse step recomputes from a keyframe via deterministic
 * replay, repeated reverse-steps are stable and never drift.
 *
 * A position is (keyframe epoch, offset), where offset indexes the steps of that
 * keyframe's journal up to the next keyframe. reverse-step moves one step
 * earlier: within an epoch it decrements the offset; at a keyframe boundary
 * (offset 0) it crosses to the previous keyframe's last step. reverse-continue
 * steps backward until an injected stop condition holds (breakpoints arrive in
 * #171) or the oldest retained keyframe is reached.
 *
 * The core is pure and host-testable: it drives a real rewind_ctx, and the
 * per-epoch step count comes from an injected epoch_len (the journal's event
 * count for that epoch). The kernel adapter (reverse_sync.c) binds the globals.
 *
 * See docs/architecture/time-travel.md.
 */

#ifndef REVERSE_H
#define REVERSE_H

#include <stdint.h>
#include <stdbool.h>

#include "rewind.h"

#define REVERSE_OK          0
#define REVERSE_ERR_PARAM  -1
#define REVERSE_AT_START   -2   /* already at the oldest retained moment */
#define REVERSE_ERR_REWIND -3   /* the underlying rewind failed */

typedef struct {
    uint64_t epoch;    /* keyframe epoch this position is anchored at */
    uint64_t offset;   /* steps replayed into that keyframe's journal */
} reverse_pos_t;

/* Number of steps in keyframe `epoch`'s journal (up to the next keyframe). */
typedef uint64_t (*reverse_epoch_len_fn)(void* ctx, uint64_t epoch);

/* Stop predicate for reverse-continue: return true to stop at `pos`. */
typedef bool (*reverse_stop_fn)(void* ctx, reverse_pos_t pos);

typedef struct {
    rewind_ctx_t*        rw;         /* the rewind verb (drives restore+replay) */
    reverse_epoch_len_fn epoch_len;  /* per-epoch step count (from the journal) */
    void*                ctx;
    reverse_pos_t        cur;        /* current position */
} reverse_ctx_t;

/* Bind reverse execution to a rewind verb and an epoch-length source. */
int  reverse_init(reverse_ctx_t* rc, rewind_ctx_t* rw,
                  reverse_epoch_len_fn epoch_len, void* ctx);

/* Set the current position (where execution is "now"). */
void reverse_set_position(reverse_ctx_t* rc, uint64_t epoch, uint64_t offset);
reverse_pos_t reverse_position(const reverse_ctx_t* rc);

/* Step one unit backward. Lands at the previous step and updates the current
 * position. Returns REVERSE_AT_START if already at the oldest retained moment. */
int reverse_step(reverse_ctx_t* rc);

/* Step backward until `should_stop(pos)` holds, or the oldest retained moment is
 * reached. Returns REVERSE_OK if a stop was hit (position left there), or
 * REVERSE_AT_START if it ran back to the beginning without stopping. */
int reverse_continue(reverse_ctx_t* rc, reverse_stop_fn should_stop, void* pctx);

/* ---- Kernel adapter (reverse_sync.c) ----
 * A global reverse-execution context bound to the kernel's rewind verb, exposing
 * the reverse-step / reverse-continue commands for a debugger front end. */
void          kreverse_bind(rewind_ctx_t* rw, reverse_epoch_len_fn epoch_len, void* ctx);
void          kreverse_set_position(uint64_t epoch, uint64_t offset);
reverse_pos_t kreverse_position(void);
int           kreverse_step(void);
int           kreverse_continue(reverse_stop_fn should_stop, void* pctx);
/* The bound reverse context, for binding reverse breakpoints to it (#226). NULL
 * until kreverse_bind() succeeds. */
reverse_ctx_t* kreverse_ctx(void);

#endif /* REVERSE_H */
