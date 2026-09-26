/* IKOS Orthogonal Persistence - Replay Driver, kernel adapter (#196, #225)
 *
 * Binds the pure replay driver (replay_driver.c) to the booted kernel:
 *   restore_keyframe  rebuild the machine from a retained keyframe
 *                     (tt_restore_keyframe: processes, address spaces, pages,
 *                     registers, run order, pending entry);
 *   source            that epoch's journal, from the journal ring (#223);
 *   load_subsystems   install the journal into the scheduler / time / entropy
 *                     wrappers in REPLAY mode (replay_engine_sync.c);
 *   drive_steps       re-execute the user processes until the step clock
 *                     reaches the target (tt_drive).
 *
 * The engine it initializes is the one the rewind verb (rewind_sync.c) drives,
 * so krewind_to(), kreverse_step(), and the front ends all reconstruct the past
 * by actually re-running it.
 */

#include "replay_driver.h"
#include "replay_engine.h"       /* replay_load_subsystems */
#include "journal_capture.h"     /* journal_capture_load, JOURNAL_EV_DIVERGE */
#include "checkpoint_journal.h"  /* journal_reader_t, journal_reader_next */
#include "core/timetravel.h"     /* tt_restore_keyframe, tt_drive */
#include <stddef.h>

int tt_drive(uint64_t epoch, uint64_t step, uint64_t insn, uint64_t epoch_len);

/* The driver carries large per-epoch scratch buffers; keep it in BSS. */
static replay_driver_t  g_driver;
static journal_reader_t g_reader;
static bool             g_reader_ok;
static uint64_t         g_insn_target;
static bool             g_perturb;

static int src_begin_epoch(void* ctx, uint64_t epoch) {
    (void)ctx;
    g_reader_ok = journal_capture_load(epoch, &g_reader) == JOURNAL_OK;
    return g_reader_ok ? 0 : -1;
}

static int src_next(void* ctx, replay_event_t* out) {
    (void)ctx;
    if (!g_reader_ok) return -1;
    journal_event_t ev;
    int rc = journal_reader_next(&g_reader, &ev);
    if (rc == JOURNAL_ERR_NO_JOURNAL) return 0;   /* epoch exhausted */
    if (rc != JOURNAL_OK) return -1;
    out->type = ev.type;
    out->len = ev.len;
    out->lclock = ev.lclock;
    out->value = ev.value;
    return 1;
}

static int drv_restore_keyframe(void* ctx, uint64_t epoch) {
    (void)ctx;
    return tt_restore_keyframe(epoch) == TT_OK ? 0 : -1;
}

static int drv_load_subsystems(uint64_t epoch,
                               const uint64_t* pts, uint32_t n_pts,
                               const uint64_t* times, uint32_t n_times,
                               const uint8_t* entropy, uint32_t n_entropy) {
    if (g_perturb) {
        /* Test hook: corrupt the recorded inputs so the replay must diverge.
         * Every entropy byte is flipped (a single byte can be masked off or
         * overwritten before the epoch ends, leaving the end state intact). */
        g_perturb = false;
        for (uint32_t i = 0; i < n_entropy; i++) g_driver.entropy[i] ^= 0x5A;
        if (n_entropy == 0) {
            for (uint32_t i = 0; i < n_times; i++) g_driver.times[i] += 1000000;
        }
    }
    return replay_load_subsystems(epoch, pts, n_pts, times, n_times, entropy, n_entropy);
}

static int drv_drive_steps(void* ctx, uint64_t epoch, uint64_t steps) {
    (void)ctx;
    if (!g_driver.has_epoch_len) return REPLAY_ERR_RUN;
    return tt_drive(epoch, steps, g_insn_target, g_driver.epoch_len) == TT_OK ? 0 : REPLAY_ERR_RUN;
}

void replay_kernel_bind(void) {
    g_driver.restore_keyframe = drv_restore_keyframe;
    g_driver.source.begin_epoch = src_begin_epoch;
    g_driver.source.next = src_next;
    g_driver.source.ctx = NULL;
    g_driver.load_subsystems = drv_load_subsystems;
    g_driver.drive_steps = drv_drive_steps;
    g_driver.ctx = NULL;
    replay_driver_bind(&g_driver);
}

replay_engine_t* replay_kernel_engine(void) {
    return &g_driver.engine;
}

void replay_set_insn_target(uint64_t insn) {
    g_insn_target = insn;
}

void replay_perturb_next(bool on) {
    g_perturb = on;
}

/* Load `epoch`'s journal into the wrappers without restoring or running. */
int replay_load_epoch(uint64_t epoch) {
    return g_driver.engine.hooks.load_epoch(g_driver.engine.hooks.ctx, epoch) == REPLAY_OK ? 0 : -1;
}

bool replay_loaded_epoch_len(uint64_t* len) {
    if (len) *len = g_driver.epoch_len;
    return g_driver.has_epoch_len;
}

int replay_to(uint64_t target_epoch, uint64_t target_offset) {
    return replay_driver_run(&g_driver, target_epoch, target_epoch, target_offset);
}

int replay_recorded_sums(uint64_t epoch, uint32_t* ids, uint32_t* sums, uint32_t max,
                         uint32_t* n_out) {
    journal_reader_t rd;
    *n_out = 0;
    if (journal_capture_load(epoch, &rd) != JOURNAL_OK) return -1;
    journal_event_t ev;
    while (journal_reader_next(&rd, &ev) == JOURNAL_OK) {
        if (ev.type != JOURNAL_EV_DIVERGE || *n_out >= max) continue;
        ids[*n_out] = (uint32_t)ev.lclock;    /* component id rides in lclock */
        sums[*n_out] = (uint32_t)ev.value;    /* checksum rides in value */
        (*n_out)++;
    }
    return 0;
}

int replay_journal_epoch_len(uint64_t epoch, uint64_t* len) {
    journal_reader_t rd;
    if (journal_capture_load(epoch, &rd) != JOURNAL_OK) return -1;
    journal_event_t ev;
    int found = -1;
    while (journal_reader_next(&rd, &ev) == JOURNAL_OK) {
        if (ev.type == JOURNAL_EV_EPOCH_LEN) {
            *len = ev.value;
            found = 0;
        }
    }
    return found;
}
