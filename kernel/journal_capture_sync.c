/* IKOS Orthogonal Persistence - Journal Capture, kernel adapter (#194, #223)
 *
 * Journals each epoch into the journal ring (journal_ring.h): one journal per
 * retained keyframe. The time-travel core calls journal_capture_close_epoch()
 * when an epoch ends (the next keyframe is taken) and when the machine stops
 * (the open epoch is sealed up to the stop point). The journal for keyframe
 * epoch E carries everything recorded since keyframe E: the scheduler's
 * preemption steps, time reads, entropy, the divergence checksums of the state
 * at the epoch's end, and the epoch's length in steps.
 */

#include "journal_capture.h"
#include "journal_ring.h"
#include "keyframe_store.h"   /* keyframe_store_get, keyframe_store_ring */
#include "time_record.h"      /* ktime_values */
#include "entropy_record.h"   /* kentropy_bytes */
#include "divergence_scan.h"  /* kdiverge_record_epoch, kdiverge_journal_sums */
#include <stddef.h>

/* The scheduler's preemption record (core/machine.c). */
extern uint32_t scheduler_preempt_points(const uint64_t** out);

static journal_ring_t g_ring;
static bool           g_ready;
static uint64_t       g_epoch_len;

static uint64_t epoch_length(void) { return g_epoch_len; }

static const journal_capture_sources_t g_live_sources = {
    .preempt_points  = scheduler_preempt_points,
    .time_values     = ktime_values,
    .entropy_bytes   = kentropy_bytes,
    .divergence_sums = kdiverge_journal_sums,
    .epoch_length    = epoch_length,
};

int journal_capture_init(fat_block_device_t* dev, uint32_t base_sector,
                         uint32_t slot_sectors, uint32_t capacity, bool format) {
    g_ready = false;
    if (journal_ring_init(&g_ring, dev, base_sector, capacity, slot_sectors) != JOURNAL_RING_OK) {
        return JOURNAL_ERR_PARAM;
    }
    if (format && journal_ring_format(&g_ring) != JOURNAL_RING_OK) {
        return JOURNAL_ERR_IO;
    }
    g_ready = true;
    return JOURNAL_OK;
}

int journal_capture_close_epoch(uint64_t epoch, uint64_t steps) {
    if (!g_ready) return JOURNAL_ERR_STATE;
    keyframe_store_t* ks = keyframe_store_get();
    if (!ks) return JOURNAL_ERR_STATE;
    journal_store_t* js = journal_ring_store_for(&g_ring, keyframe_store_ring(ks), epoch);
    if (!js) return JOURNAL_ERR_NO_JOURNAL;   /* the keyframe is not retained */

    /* Checksum the machine as it stands at the epoch's end (#197, #225). */
    kdiverge_record_epoch(epoch);
    g_epoch_len = steps;
    return journal_capture_epoch(js, epoch, 0, &g_live_sources);
}

journal_ring_t* journal_capture_ring(void) {
    return g_ready ? &g_ring : NULL;
}

int journal_capture_load(uint64_t epoch, journal_reader_t* reader) {
    if (!g_ready) return JOURNAL_ERR_STATE;
    keyframe_store_t* ks = keyframe_store_get();
    if (!ks) return JOURNAL_ERR_STATE;
    return journal_ring_load(&g_ring, keyframe_store_ring(ks), epoch, reader) == JOURNAL_RING_OK
               ? JOURNAL_OK : JOURNAL_ERR_NO_JOURNAL;
}
