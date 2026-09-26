/* IKOS Orthogonal Persistence - Rewind Command (#169, epic #159)
 *
 * The core time-travel verb: "rewind to epoch E". It composes the keyframe
 * retention ring (#168) and the replay engine (#165): find the nearest retained
 * keyframe at or before the target, restore it, then replay forward to the exact
 * target (an epoch plus an offset into that epoch's journal), leaving the system
 * runnable at that past moment.
 *
 * A target outside the retained window is rejected with a clear error: before
 * the oldest keyframe (fell off the ring, #168) or after the newest (there is
 * no such future to rewind to).
 *
 * The core is pure and host-testable: it drives a real keyframe_ring and a real
 * replay_engine, whose restore/load/run steps are the engine's injected hooks.
 * The kernel adapter (rewind_sync.c) binds the global ring and engine and
 * exposes krewind_to() as the command entry point.
 *
 * See docs/architecture/time-travel.md.
 */

#ifndef REWIND_H
#define REWIND_H

#include <stdint.h>
#include <stdbool.h>

#include "keyframe_ring.h"
#include "replay_engine.h"

#define REWIND_OK             0
#define REWIND_ERR_PARAM     -1
#define REWIND_ERR_OUT_OF_RANGE -2   /* target before the oldest / after the newest keyframe */
#define REWIND_ERR_REPLAY    -3      /* the replay engine failed en route to the target */

typedef struct {
    const keyframe_ring_t* ring;    /* keyframe selection */
    replay_engine_t*       engine;  /* drives restore + re-drive */

    /* Result of the last rewind_to. */
    uint64_t landed_keyframe;  /* keyframe epoch restored from */
    uint64_t landed_epoch;     /* target epoch reached */
    uint64_t landed_offset;    /* offset into the target epoch */
    bool     runnable;         /* true once the system sits at the target */
} rewind_ctx_t;

/* Bind the rewind verb to a keyframe ring and a (hook-initialized) engine. */
int rewind_init(rewind_ctx_t* rw, const keyframe_ring_t* ring, replay_engine_t* engine);

/* Rewind to (target_epoch, target_offset): restore the nearest retained
 * keyframe at or before target_epoch, then replay forward to the target. An
 * offset of 0 lands exactly at the target epoch's keyframe boundary. Returns
 * REWIND_ERR_OUT_OF_RANGE if the target is outside the retained window. */
int rewind_to(rewind_ctx_t* rw, uint64_t target_epoch, uint64_t target_offset);

/* ---- Kernel adapter (rewind_sync.c) ----
 * A global rewind verb bound to the kernel's keyframe ring and replay engine.
 * krewind_to() is the "rewind-to <epoch>" command entry point; a CLI wrapper
 * parses the argument and calls it. */
void     krewind_bind(const keyframe_ring_t* ring, replay_engine_t* engine);
int      krewind_to(uint64_t target_epoch, uint64_t target_offset);
bool     krewind_runnable(void);
uint64_t krewind_landed_keyframe(void);
/* The bound rewind context, for binding reverse execution to it (#226). NULL
 * until krewind_bind() succeeds. */
rewind_ctx_t* krewind_ctx(void);

#endif /* REWIND_H */
