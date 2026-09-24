/* Host-side unit test for reverse breakpoints and watchpoints (#171).
 *
 * Drives the REAL reverse / rewind / keyframe-ring / replay-engine stack. The
 * "system state" is modeled as a watched value that is written at two known
 * positions; the injected condition and probe read that value at whatever
 * position the stack restored. Verifies:
 *   1. A reverse breakpoint stops at the most recent earlier position where the
 *      condition holds.
 *   2. A reverse watchpoint lands on the last write to the value.
 *   3. The search is bounded by the retained ring: a miss reports NOT_FOUND at
 *      the oldest retained moment.
 *
 * Build: gcc -I../include -o test_revbreak test_revbreak.c ../kernel/revbreak.c \
 *          ../kernel/reverse.c ../kernel/rewind.c ../kernel/keyframe_ring.c \
 *          ../kernel/replay_engine.c
 */

#include <stdint.h>
#include <stdbool.h>
extern int printf(const char*, ...);

#include "revbreak.h"

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("  FAIL: %s\n", msg); failures++; } \
    else { printf("  ok:   %s\n", msg); } \
} while (0)

/* Mock engine hooks (positions are driven by rewind; state is modeled below). */
typedef struct { uint64_t restored; } sim_t;
static int sim_restore(void* c, uint64_t e) { ((sim_t*)c)->restored = e; return 0; }
static int sim_load(void* c, uint64_t e) { (void)c; (void)e; return 0; }
static int sim_run(void* c, uint64_t e, uint64_t l) { (void)c; (void)e; (void)l; return 0; }

static uint64_t elen(void* c, uint64_t e) {
    (void)c;
    if (e == 20) return 3;
    if (e == 30) return 4;
    if (e == 40) return 5;
    return 0;
}

/* Total order on positions, and the watched value = number of writes at or
 * before a position. Writes at W1 = (30,2) and W2 = (40,1). */
static bool pos_le(uint64_t ae, uint64_t ao, uint64_t be, uint64_t bo) {
    return ae < be || (ae == be && ao <= bo);
}
static uint64_t value_at(reverse_pos_t p) {
    uint64_t v = 0;
    if (pos_le(30, 2, p.epoch, p.offset)) v++;  /* W1 */
    if (pos_le(40, 1, p.epoch, p.offset)) v++;  /* W2 */
    return v;
}

/* Condition / probe read the value at the reverse context's current position. */
static bool cond_value_is_1(void* c) {
    return value_at(reverse_position((reverse_ctx_t*)c)) == 1;
}
static bool cond_never(void* c) { (void)c; return false; }
static uint64_t probe_value(void* c) {
    return value_at(reverse_position((reverse_ctx_t*)c));
}

/* Scan hook: visit (epoch, 0..limit-1) with the model positioned at each. */
static int g_scan_calls;
static int sim_scan(void* sctx, uint64_t epoch, uint64_t limit,
                    revbreak_visit_fn visit, void* vctx) {
    reverse_ctx_t* rv = (reverse_ctx_t*)sctx;
    g_scan_calls++;
    for (uint64_t o = 0; o < limit; o++) {
        reverse_set_position(rv, epoch, o);
        reverse_pos_t p = { epoch, o };
        visit(vctx, p);
    }
    return 0;
}

/* A value written exactly when keyframe 40 is taken: it differs between the
 * last position of epoch 30, (30,3), and (40,0). */
static bool g_boundary_write;
static uint64_t probe_boundary(void* c) {
    reverse_pos_t p = reverse_position((reverse_ctx_t*)c);
    return (g_boundary_write && p.epoch >= 40) ? 7 : 0;
}

static bool pos_is(reverse_pos_t p, uint64_t e, uint64_t o) {
    return p.epoch == e && p.offset == o;
}

static void fixture(keyframe_ring_t* ring, sim_t* s, replay_engine_t* re,
                    rewind_ctx_t* rw, reverse_ctx_t* rv,
                    uint64_t start_epoch, uint64_t start_offset) {
    keyframe_ring_init(ring, 4);
    keyframe_ring_advance(ring, 20);
    keyframe_ring_advance(ring, 30);
    keyframe_ring_advance(ring, 40);
    s->restored = 0;
    replay_hooks_t h = { sim_restore, sim_load, sim_run, s };
    replay_init(re, &h);
    rewind_init(rw, ring, re);
    reverse_init(rv, rw, elen, 0);
    reverse_set_position(rv, start_epoch, start_offset);
}

int main(void) {
    printf("=== Reverse breakpoints and watchpoints (#171) unit test ===\n");

    /* --- 1. Reverse breakpoint: last earlier position where value == 1 --- */
    {
        keyframe_ring_t ring; sim_t s; replay_engine_t re; rewind_ctx_t rw; reverse_ctx_t rv;
        fixture(&ring, &s, &re, &rw, &rv, 40, 3);   /* value there is 2 */
        reverse_pos_t hit;
        CHECK(reverse_breakpoint(&rv, cond_value_is_1, &rv, &hit) == REVBREAK_OK,
              "reverse breakpoint finds an earlier hit");
        CHECK(pos_is(hit, 40, 0), "stopped at the most recent earlier position with value 1 (40,0)");
        CHECK(pos_is(reverse_position(&rv), 40, 0), "system left at the breakpoint hit");
    }

    /* --- 2. Reverse watchpoint: last write to the value --- */
    {
        keyframe_ring_t ring; sim_t s; replay_engine_t re; rewind_ctx_t rw; reverse_ctx_t rv;
        fixture(&ring, &s, &re, &rw, &rv, 40, 3);
        reverse_pos_t hit;
        CHECK(reverse_watchpoint(&rv, probe_value, &rv, &hit) == REVBREAK_OK,
              "reverse watchpoint finds a write");
        CHECK(pos_is(hit, 40, 1), "landed on the last write to the value (40,1)");
        CHECK(pos_is(reverse_position(&rv), 40, 1), "system left at the write");
    }

    /* --- 2b. Watchpoint from an earlier point finds the earlier write --- */
    {
        keyframe_ring_t ring; sim_t s; replay_engine_t re; rewind_ctx_t rw; reverse_ctx_t rv;
        fixture(&ring, &s, &re, &rw, &rv, 40, 0);   /* value there is 1 */
        reverse_pos_t hit;
        CHECK(reverse_watchpoint(&rv, probe_value, &rv, &hit) == REVBREAK_OK &&
              pos_is(hit, 30, 2),
              "from (40,0) the last write is the earlier one (30,2)");
    }

    /* --- 3. Bounded by the ring: misses report NOT_FOUND at the start --- */
    {
        keyframe_ring_t ring; sim_t s; replay_engine_t re; rewind_ctx_t rw; reverse_ctx_t rv;
        fixture(&ring, &s, &re, &rw, &rv, 40, 3);
        CHECK(reverse_breakpoint(&rv, cond_never, &rv, 0) == REVBREAK_NOT_FOUND,
              "a breakpoint that never holds reports NOT_FOUND");
        CHECK(pos_is(reverse_position(&rv), 20, 0), "and the scan stopped at the oldest retained moment");
    }
    {
        keyframe_ring_t ring; sim_t s; replay_engine_t re; rewind_ctx_t rw; reverse_ctx_t rv;
        fixture(&ring, &s, &re, &rw, &rv, 20, 2);   /* before any write: value 0 throughout */
        reverse_pos_t hit;
        CHECK(reverse_watchpoint(&rv, probe_value, &rv, &hit) == REVBREAK_NOT_FOUND,
              "a value that never changed in the window reports NOT_FOUND");
    }

    /* --- 5. Scan fast path (#226): same answers, each epoch replayed once --- */
    {
        keyframe_ring_t ring; sim_t s; replay_engine_t re; rewind_ctx_t rw; reverse_ctx_t rv;
        fixture(&ring, &s, &re, &rw, &rv, 40, 3);
        g_scan_calls = 0;
        reverse_pos_t hit;
        CHECK(reverse_watchpoint_scan(&rv, sim_scan, &rv, probe_value, &rv, &hit) == REVBREAK_OK &&
              pos_is(hit, 40, 1), "scan watchpoint lands on the last write (40,1)");
        CHECK(g_scan_calls == 1, "found within the current epoch: one scan");
        CHECK(pos_is(reverse_position(&rv), 40, 1), "system left at the write");
    }
    {
        keyframe_ring_t ring; sim_t s; replay_engine_t re; rewind_ctx_t rw; reverse_ctx_t rv;
        fixture(&ring, &s, &re, &rw, &rv, 40, 0);
        g_scan_calls = 0;
        reverse_pos_t hit;
        CHECK(reverse_watchpoint_scan(&rv, sim_scan, &rv, probe_value, &rv, &hit) == REVBREAK_OK &&
              pos_is(hit, 30, 2), "scan watchpoint from (40,0) finds (30,2)");
        CHECK(g_scan_calls == 2, "two epochs scanned, once each");
    }
    {
        /* A write right at an epoch boundary: value changes between (30,3) and (40,0). */
        keyframe_ring_t ring; sim_t s; replay_engine_t re; rewind_ctx_t rw; reverse_ctx_t rv;
        fixture(&ring, &s, &re, &rw, &rv, 40, 2);
        g_boundary_write = true;
        reverse_pos_t hit;
        CHECK(reverse_watchpoint_scan(&rv, sim_scan, &rv, probe_boundary, &rv, &hit) == REVBREAK_OK &&
              pos_is(hit, 40, 0), "a write across the epoch boundary lands at (40,0)");
        reverse_pos_t hit2;
        fixture(&ring, &s, &re, &rw, &rv, 40, 2);
        CHECK(reverse_watchpoint(&rv, probe_boundary, &rv, &hit2) == REVBREAK_OK &&
              pos_is(hit2, 40, 0), "the step-by-step search agrees");
        g_boundary_write = false;
    }
    {
        keyframe_ring_t ring; sim_t s; replay_engine_t re; rewind_ctx_t rw; reverse_ctx_t rv;
        fixture(&ring, &s, &re, &rw, &rv, 40, 3);
        reverse_pos_t hit;
        CHECK(reverse_breakpoint_scan(&rv, sim_scan, &rv, cond_value_is_1, &rv, &hit) == REVBREAK_OK &&
              pos_is(hit, 40, 0), "scan breakpoint stops at (40,0), like the step-by-step search");
        fixture(&ring, &s, &re, &rw, &rv, 40, 3);
        CHECK(reverse_breakpoint_scan(&rv, sim_scan, &rv, cond_never, &rv, 0) == REVBREAK_NOT_FOUND,
              "scan breakpoint that never holds: NOT_FOUND");
        CHECK(pos_is(reverse_position(&rv), 20, 0), "and leaves the system at the oldest moment");
        fixture(&ring, &s, &re, &rw, &rv, 20, 2);
        CHECK(reverse_watchpoint_scan(&rv, sim_scan, &rv, probe_value, &rv, 0) == REVBREAK_NOT_FOUND,
              "scan watchpoint on an unchanged value: NOT_FOUND");
    }

    /* --- 4. Bad params --- */
    {
        reverse_ctx_t rv;
        CHECK(reverse_breakpoint(0, cond_never, 0, 0) == REVBREAK_ERR_PARAM, "breakpoint rejects NULL ctx");
        CHECK(reverse_watchpoint(&rv, 0, 0, 0) == REVBREAK_ERR_PARAM, "watchpoint rejects NULL probe");
    }

    if (failures == 0) {
        printf("PASSED: reverse breakpoints and watchpoints find the last hit, bounded by the ring\n");
        return 0;
    }
    printf("FAILED: %d check(s)\n", failures);
    return 1;
}
