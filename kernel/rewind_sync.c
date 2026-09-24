/* IKOS Orthogonal Persistence - Rewind Command, kernel adapter (#169)
 *
 * Binds the pure rewind verb (rewind.c) to the kernel's global keyframe ring
 * (#168) and replay engine (#165), and exposes krewind_to() as the
 * "rewind-to <epoch>" command entry point. A CLI wrapper parses the epoch (and
 * an optional offset) and calls krewind_to().
 */

#include "rewind.h"

static rewind_ctx_t g_rewind;
static bool         g_rewind_bound;

void krewind_bind(const keyframe_ring_t* ring, replay_engine_t* engine) {
    g_rewind_bound = (rewind_init(&g_rewind, ring, engine) == REWIND_OK);
}

int krewind_to(uint64_t target_epoch, uint64_t target_offset) {
    if (!g_rewind_bound) return REWIND_ERR_PARAM;
    return rewind_to(&g_rewind, target_epoch, target_offset);
}

bool krewind_runnable(void) {
    return g_rewind_bound && g_rewind.runnable;
}

rewind_ctx_t* krewind_ctx(void) {
    return g_rewind_bound ? &g_rewind : 0;
}

uint64_t krewind_landed_keyframe(void) {
    return g_rewind_bound ? g_rewind.landed_keyframe : 0;
}
