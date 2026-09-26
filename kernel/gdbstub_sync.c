/* IKOS Orthogonal Persistence - GDB Reverse Stub, kernel adapter (#172)
 *
 * Wires the pure RSP stub (gdbstub.c) to IKOS's reverse execution (#170): the
 * bs / bc packets drive kreverse_step / kreverse_continue. A kernel serial loop
 * reads a framed request from the gdb connection, calls gdbstub_serve(), and
 * writes the framed reply back; that transport is the only remaining wiring to
 * run reverse-step / reverse-continue live under `make debug`.
 */

#include "gdbstub.h"
#include "reverse.h"

static int op_reverse_step(void* ctx) {
    (void)ctx;
    return kreverse_step();
}

/* Plain reverse-continue runs back to the previous stop; with no gdb breakpoint
 * set that is the oldest retained moment. Breakpoint-aware reverse-continue
 * plugs a predicate in here (reverse breakpoints, #171). */
static int op_reverse_continue(void* ctx) {
    (void)ctx;
    return kreverse_continue(0, 0);
}

static gdbstub_ops_t       g_ops;
static const gdbstub_ops_t* g_active = &g_ops;
static gdbstub_session_t   g_session;

void gdbstub_bind_reverse(void) {
    g_ops.reverse_step = op_reverse_step;
    g_ops.reverse_continue = op_reverse_continue;
    g_ops.ctx = 0;
}

void gdbstub_set_ops(const gdbstub_ops_t* ops) {
    g_active = ops ? ops : &g_ops;
    g_session.gthread = 0;
}

int gdbstub_serve(const char* frame, uint32_t flen, char* out, uint32_t outcap) {
    static char payload[GDBSTUB_PACKET_MAX + 64];
    bool ok = false;
    int plen = gdbstub_unframe(frame, flen, payload, sizeof(payload), &ok);
    if (plen < 0 || !ok) return -1;

    static char resp[GDBSTUB_PACKET_MAX + 64];
    int rlen = gdbstub_handle_session(g_active, &g_session, payload, (uint32_t)plen,
                                      resp, sizeof(resp));
    if (rlen == GDBSTUB_NO_REPLY) return 0;
    if (rlen < 0) return -1;

    return gdbstub_frame(resp, (uint32_t)rlen, out, outcap);
}
