/* IKOS Orthogonal Persistence - Reverse Breakpoints/Watchpoints adapter (#171)
 *
 * Binds the reverse breakpoint / watchpoint operations (revbreak.c) to a reverse
 * context for the debugger front end (the GDB bridge in #172).
 */

#include "revbreak.h"

static reverse_ctx_t*   g_rv;
static revbreak_scan_fn g_scan;
static void*            g_scan_ctx;

void krevbreak_bind(reverse_ctx_t* rv) {
    g_rv = rv;
}

void krevbreak_set_scanner(revbreak_scan_fn scan, void* sctx) {
    g_scan = scan;
    g_scan_ctx = sctx;
}

int krevbreak_breakpoint(revbreak_cond_fn cond, void* ctx, reverse_pos_t* hit) {
    if (!g_rv) return REVBREAK_ERR_PARAM;
    if (g_scan) return reverse_breakpoint_scan(g_rv, g_scan, g_scan_ctx, cond, ctx, hit);
    return reverse_breakpoint(g_rv, cond, ctx, hit);
}

int krevbreak_watchpoint(revbreak_probe_fn probe, void* ctx, reverse_pos_t* hit) {
    if (!g_rv) return REVBREAK_ERR_PARAM;
    if (g_scan) return reverse_watchpoint_scan(g_rv, g_scan, g_scan_ctx, probe, ctx, hit);
    return reverse_watchpoint(g_rv, probe, ctx, hit);
}
