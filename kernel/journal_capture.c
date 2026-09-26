/* IKOS Orthogonal Persistence - Per-epoch journal capture core (#194)
 *
 * See include/journal_capture.h. Pure and host-testable: the epoch's deltas
 * arrive through injected source function pointers and are written to a
 * journal_store_t, so this file needs no allocator, no hardware, and no
 * dependency on the scheduler / time / entropy subsystems.
 */

#include "journal_capture.h"
#include <stddef.h>

int journal_capture_epoch(journal_store_t* store, uint64_t epoch,
                          uint64_t base_lclock,
                          const journal_capture_sources_t* src) {
    if (!store || !src) {
        return JOURNAL_ERR_PARAM;
    }

    journal_writer_t writer;
    int rc = journal_store_begin(store, epoch, base_lclock, &writer);
    if (rc != JOURNAL_OK) {
        return rc;
    }

    /* 1. Scheduler preemption points. Each point is the logical clock at which
     *    a context switch was forced; store it as both the event lclock and the
     *    value so replay can reinstate the exact switch schedule. */
    if (src->preempt_points) {
        const uint64_t* pts = NULL;
        uint32_t n = src->preempt_points(&pts);
        for (uint32_t i = 0; i < n && pts; i++) {
            rc = journal_writer_append(&writer, JOURNAL_EV_SCHED, pts[i], pts[i]);
            if (rc != JOURNAL_OK) {
                return rc; /* uncommitted: previous epoch's journal stays live */
            }
        }
    }

    /* 2. Time / cycle reads, in the order they were captured. lclock is the
     *    read index within the epoch; value is the RDTSC value that was
     *    returned to the caller and must be reproduced on replay. */
    if (src->time_values) {
        const uint64_t* vals = NULL;
        uint32_t n = src->time_values(&vals);
        for (uint32_t i = 0; i < n && vals; i++) {
            rc = journal_writer_append(&writer, JOURNAL_EV_TIMER, i, vals[i]);
            if (rc != JOURNAL_OK) {
                return rc;
            }
        }
    }

    /* 3. Entropy bytes, packed up to 8 per event (little-endian into value).
     *    len records how many of those 8 bytes are valid, so the exact byte
     *    stream reads back even when the run is not a multiple of 8. lclock is
     *    the byte offset of the chunk within the epoch's entropy run. */
    if (src->entropy_bytes) {
        const uint8_t* bytes = NULL;
        uint32_t n = src->entropy_bytes(&bytes);
        for (uint32_t off = 0; off < n && bytes; off += 8) {
            uint32_t chunk = (n - off) < 8u ? (n - off) : 8u;
            uint64_t packed = 0;
            for (uint32_t b = 0; b < chunk; b++) {
                packed |= (uint64_t)bytes[off + b] << (8u * b);
            }
            rc = journal_writer_append_len(&writer, JOURNAL_EV_ENTROPY,
                                           off, packed, chunk);
            if (rc != JOURNAL_OK) {
                return rc;
            }
        }
    }

    /* 4. Divergence checksums for the epoch boundary (#197): one event per
     *    component, carrying the component id in lclock and the checksum in
     *    value, so replay can compare recomputed sums against these. */
    if (src->divergence_sums) {
        const uint32_t* ids = NULL;
        const uint32_t* sums = NULL;
        uint32_t n = src->divergence_sums(&ids, &sums);
        for (uint32_t i = 0; i < n && ids && sums; i++) {
            rc = journal_writer_append(&writer, JOURNAL_EV_DIVERGE,
                                       ids[i], sums[i]);
            if (rc != JOURNAL_OK) {
                return rc;
            }
        }
    }

    /* 5. The epoch's length in steps (#223), written last so a reader that
     *    sees it knows the journal is complete. */
    if (src->epoch_length) {
        uint64_t len = src->epoch_length();
        rc = journal_writer_append(&writer, JOURNAL_EV_EPOCH_LEN, len, len);
        if (rc != JOURNAL_OK) {
            return rc;
        }
    }

    /* Commit: the journal store flips its superblock, atomically publishing
     * this epoch's journal next to the checkpoint that closed the epoch. */
    return journal_store_commit(&writer);
}
