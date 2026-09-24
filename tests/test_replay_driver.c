/* Host-side unit test for the replay driver core (#196).
 *
 * Verifies, with mock hooks (no kernel):
 *   1. replay_split_epoch splits a mixed event stream (scheduler points, time
 *      reads, packed entropy runs) back into the three delta arrays in order,
 *      reconstructing a non-8-multiple entropy run byte-for-byte.
 *   2. replay_driver_run assembles the hooks and lands at an arbitrary
 *      (epoch, offset): restore is called once with the keyframe epoch, each
 *      full epoch loads its deltas and re-drives its whole length, and the
 *      target epoch re-drives exactly target_offset steps.
 *   3. A whole-epoch drive uses the epoch's last recorded switch point.
 *
 * Build: gcc -I../include -o test_replay_driver \
 *            test_replay_driver.c ../kernel/replay_driver.c \
 *            ../kernel/replay_engine.c
 */

#include <stdint.h>
#include <stdbool.h>

typedef __SIZE_TYPE__ size_t;
#include <stddef.h>  /* NULL */
extern int printf(const char*, ...);

#include "replay_driver.h"

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("  FAIL: %s\n", msg); failures++; } \
    else { printf("  ok:   %s\n", msg); } \
} while (0)

/* ---- A scripted event source: a flat table of (epoch, event) rows ---- */

typedef struct { uint64_t epoch; replay_event_t ev; } row_t;

static row_t g_rows[64];
static uint32_t g_nrows;
static uint64_t g_cur_epoch;
static uint32_t g_cur_index;

static void rows_reset(void) { g_nrows = 0; }
static void add_row(uint64_t epoch, uint32_t type, uint32_t len,
                    uint64_t lclock, uint64_t value) {
    g_rows[g_nrows].epoch = epoch;
    g_rows[g_nrows].ev.type = type;
    g_rows[g_nrows].ev.len = len;
    g_rows[g_nrows].ev.lclock = lclock;
    g_rows[g_nrows].ev.value = value;
    g_nrows++;
}

static int mock_begin(void* ctx, uint64_t epoch) {
    (void)ctx;
    g_cur_epoch = epoch;
    g_cur_index = 0;
    return 0; /* every epoch is available in this mock */
}
static int mock_next(void* ctx, replay_event_t* out) {
    (void)ctx;
    while (g_cur_index < g_nrows) {
        row_t* r = &g_rows[g_cur_index++];
        if (r->epoch == g_cur_epoch) { *out = r->ev; return 1; }
    }
    return 0;
}

/* ---- Recording mock hooks for the driver ---- */

static uint64_t rec_restore_epoch;
static int      rec_restore_calls;
static uint64_t rec_last_load_epoch;
static uint32_t rec_last_n_pts, rec_last_n_times, rec_last_n_entropy;
static uint8_t  rec_last_entropy[REPLAY_MAX_ENTROPY];
static uint64_t rec_total_steps;
static uint64_t rec_last_steps;

static int mock_restore(void* ctx, uint64_t epoch) {
    (void)ctx; rec_restore_epoch = epoch; rec_restore_calls++; return 0;
}
static int mock_load(uint64_t epoch,
                     const uint64_t* pts, uint32_t n_pts,
                     const uint64_t* times, uint32_t n_times,
                     const uint8_t* entropy, uint32_t n_entropy) {
    (void)pts; (void)times;
    rec_last_load_epoch = epoch;
    rec_last_n_pts = n_pts;
    rec_last_n_times = n_times;
    rec_last_n_entropy = n_entropy;
    for (uint32_t i = 0; i < n_entropy && i < REPLAY_MAX_ENTROPY; i++)
        rec_last_entropy[i] = entropy[i];
    return 0;
}
static int mock_drive(void* ctx, uint64_t epoch, uint64_t steps) {
    (void)ctx; (void)epoch;
    rec_total_steps += steps;
    rec_last_steps = steps;
    return 0;
}

static void wire(replay_driver_t* d) {
    d->restore_keyframe = mock_restore;
    d->source.begin_epoch = mock_begin;
    d->source.next = mock_next;
    d->source.ctx = NULL;
    d->load_subsystems = mock_load;
    d->drive_steps = mock_drive;
    d->ctx = NULL;
}

static replay_driver_t g_d;

int main(void) {
    printf("test_replay_driver\n");

    /* ---- 1: split a mixed stream for one epoch ---- */
    replay_event_source_t src = { mock_begin, mock_next, NULL };
    rows_reset();
    add_row(5, REPLAY_EV_SCHED,  0, 3,  3);
    add_row(5, REPLAY_EV_SCHED,  0, 9,  9);
    add_row(5, REPLAY_EV_TIMER,  0, 0,  0x1111);
    add_row(5, REPLAY_EV_TIMER,  0, 1,  0x2222);
    add_row(5, REPLAY_EV_TIMER,  0, 2,  0x3333);
    /* 11 entropy bytes: one full 8-byte chunk + a 3-byte remainder. */
    add_row(5, REPLAY_EV_ENTROPY, 8, 0, 0x0403020100BEADDEULL);
    add_row(5, REPLAY_EV_ENTROPY, 3, 8, 0x0000000000CCBBAAULL);

    static uint64_t pts[REPLAY_MAX_PTS];
    static uint64_t times[REPLAY_MAX_TIMES];
    static uint8_t  ent[REPLAY_MAX_ENTROPY];
    uint32_t n_pts = 0, n_times = 0, n_ent = 0;

    CHECK(replay_split_epoch(&src, 5, pts, &n_pts, times, &n_times, ent, &n_ent)
              == REPLAY_OK, "split epoch 5");
    CHECK(n_pts == 2 && pts[0] == 3 && pts[1] == 9, "2 scheduler points in order");
    CHECK(n_times == 3 && times[0] == 0x1111 && times[2] == 0x3333,
          "3 time reads in order");
    CHECK(n_ent == 11, "11 entropy bytes unpacked");
    const uint8_t expect[11] = {0x00,0x01,0x02,0x03,0x04,0xDE,0xAD,0xBE,
                                0xAA,0xBB,0xCC};
    /* value 0x0403020100BEADDE little-endian => DE AD BE 00 01 02 03 04 */
    const uint8_t expect0[8] = {0xDE,0xAD,0xBE,0x00,0x01,0x02,0x03,0x04};
    const uint8_t expect1[3] = {0xAA,0xBB,0xCC};
    (void)expect;
    bool ent_ok = true;
    for (int i = 0; i < 8; i++) if (ent[i] != expect0[i]) ent_ok = false;
    for (int i = 0; i < 3; i++) if (ent[8 + i] != expect1[i]) ent_ok = false;
    CHECK(ent_ok, "entropy run reconstructed byte-for-byte across chunks");

    /* Split of an epoch with no rows yields empty arrays. */
    CHECK(replay_split_epoch(&src, 99, pts, &n_pts, times, &n_times, ent, &n_ent)
              == REPLAY_OK && n_pts == 0 && n_times == 0 && n_ent == 0,
          "empty epoch splits to empty arrays");

    /* ---- 2/3: drive multi-epoch replay to an arbitrary (epoch, offset) ---- */
    rows_reset();
    /* epoch 5: last switch point 9  => whole-epoch drive is 9 steps
     * epoch 6: last switch point 4  => whole-epoch drive is 4 steps
     * epoch 7: target, driven target_offset steps (not whole) */
    add_row(5, REPLAY_EV_SCHED, 0, 3, 3);
    add_row(5, REPLAY_EV_SCHED, 0, 9, 9);
    add_row(6, REPLAY_EV_SCHED, 0, 4, 4);
    add_row(7, REPLAY_EV_SCHED, 0, 2, 2);
    add_row(7, REPLAY_EV_TIMER, 0, 0, 0xABCD);

    wire(&g_d);
    rec_restore_calls = 0; rec_total_steps = 0;
    int rc = replay_driver_run(&g_d, 5 /*keyframe*/, 7 /*target*/, 6 /*offset*/);
    CHECK(rc == REPLAY_OK, "replay_driver_run OK");
    CHECK(rec_restore_calls == 1 && rec_restore_epoch == 5,
          "restore called once at keyframe epoch 5");
    /* Whole epochs 5 (9 steps) + 6 (4 steps) + target 7 (6 offset steps) = 19. */
    CHECK(rec_total_steps == 9 + 4 + 6, "total driven steps = 9+4+6");
    CHECK(rec_last_steps == 6, "target epoch driven exactly offset (6) steps");
    CHECK(rec_last_load_epoch == 7 && rec_last_n_times == 1,
          "target epoch 7's deltas loaded (1 time read)");
    CHECK(g_d.engine.done && g_d.engine.current_epoch == 7,
          "engine landed done at epoch 7");

    /* offset 0 stops exactly at the target epoch boundary (no partial drive). */
    rec_total_steps = 0; rec_restore_calls = 0;
    rc = replay_driver_run(&g_d, 5, 6, 0);
    CHECK(rc == REPLAY_OK, "replay to (6, 0) OK");
    CHECK(rec_total_steps == 9, "only whole epoch 5 driven (9 steps), no partial");

    /* keyframe after target is rejected by the engine. */
    CHECK(replay_driver_run(&g_d, 8, 7, 0) == REPLAY_ERR_PARAM,
          "keyframe after target rejected");

    /* The epoch's recorded length (#223): a whole-epoch re-drive runs the
     * epoch to its end, not just to its last preemption point. */
    rows_reset();
    add_row(20, REPLAY_EV_SCHED, 0, 3, 3);
    add_row(20, REPLAY_EV_EPOCH_LEN, 0, 50, 50);
    add_row(21, REPLAY_EV_EPOCH_LEN, 0, 8, 8);
    {
        uint64_t len = 0;
        bool has = false;
        uint32_t n_pts2 = 0, n_times2 = 0, n_ent2 = 0;
        CHECK(replay_split_epoch_ex(&src, 20, pts, &n_pts2, times, &n_times2, ent, &n_ent2,
                                    &len, &has) == REPLAY_OK && has && len == 50,
              "split reports the epoch length");
        rec_total_steps = 0;
        CHECK(replay_driver_run(&g_d, 20, 21, 2) == REPLAY_OK, "replay across a sized epoch");
        CHECK(rec_total_steps == 50 + 2, "whole epoch 20 re-driven for its 50 recorded steps");
    }

    /* replay_driver_bind: the engine carries the driver's hooks for callers
     * that run it themselves (the rewind verb, #226). */
    {
        replay_driver_t* d = &g_d;
        CHECK(replay_driver_bind(d) == REPLAY_OK, "bind initializes the engine");
        rec_total_steps = 0;
        rec_restore_calls = 0;
        CHECK(replay_run(&d->engine, 21, 21, 5) == REPLAY_OK && rec_restore_calls == 1 &&
              rec_total_steps == 5, "replay_run on the bound engine restores and drives");
        replay_driver_t empty = { 0 };
        CHECK(replay_driver_bind(&empty) == REPLAY_ERR_PARAM, "bind rejects missing hooks");
    }

    printf("%s (%d failure%s)\n", failures ? "FAILED" : "PASSED",
           failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
