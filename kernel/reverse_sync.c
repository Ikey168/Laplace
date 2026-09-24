/* IKOS Orthogonal Persistence - Reverse Execution, kernel adapter (#170)
 *
 * Binds the pure reverse-execution core (reverse.c) to the kernel's rewind verb
 * (#169) and exposes reverse-step / reverse-continue for a debugger front end
 * (the GDB bridge in #172). The per-epoch step count comes from the input
 * journal (#161); the kernel registers an epoch_len reader when it wires this up.
 */

#include "reverse.h"

static reverse_ctx_t g_reverse;
static bool          g_reverse_bound;

void kreverse_bind(rewind_ctx_t* rw, reverse_epoch_len_fn epoch_len, void* ctx) {
    g_reverse_bound = (reverse_init(&g_reverse, rw, epoch_len, ctx) == REVERSE_OK);
}

void kreverse_set_position(uint64_t epoch, uint64_t offset) {
    reverse_set_position(&g_reverse, epoch, offset);
}

reverse_pos_t kreverse_position(void) {
    if (g_reverse_bound) return reverse_position(&g_reverse);
    reverse_pos_t z = { 0, 0 };
    return z;
}

reverse_ctx_t* kreverse_ctx(void) {
    return g_reverse_bound ? &g_reverse : 0;
}

int kreverse_step(void) {
    if (!g_reverse_bound) return REVERSE_ERR_PARAM;
    return reverse_step(&g_reverse);
}

int kreverse_continue(reverse_stop_fn should_stop, void* pctx) {
    if (!g_reverse_bound) return REVERSE_ERR_PARAM;
    return reverse_continue(&g_reverse, should_stop, pctx);
}
