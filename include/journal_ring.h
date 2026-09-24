/* Laplace - Journal Ring (#223)
 *
 * The input journal for every retained keyframe. The journal store
 * (checkpoint_journal.h) is double-buffered and holds one epoch, so on its own
 * only the latest epoch could be replayed. The journal ring gives each slot of
 * the keyframe ring (keyframe_ring.h) its own journal store: the journal of
 * keyframe epoch E lives in the journal region paired with E's ring slot. When
 * the keyframe ring evicts an epoch and reuses its slot, the paired journal
 * region is reused too, and a stale journal is never served because every read
 * checks the journal's own epoch stamp.
 *
 * Convention (docs/architecture/time-travel.md): journal E holds the inputs
 * recorded from keyframe E up to keyframe E+1 (or up to the stop point for the
 * newest, still-open epoch): preemption steps, time reads, entropy, the
 * divergence checksums of the state at its end, and its length in steps.
 *
 * Pure orchestration over a block device; host-testable.
 */

#ifndef JOURNAL_RING_H
#define JOURNAL_RING_H

#include <stdint.h>
#include <stdbool.h>
#include "fat.h"
#include "checkpoint_journal.h"
#include "keyframe_ring.h"

#define JOURNAL_RING_OK             0
#define JOURNAL_RING_ERR_PARAM     -1
#define JOURNAL_RING_ERR_IO        -2
#define JOURNAL_RING_ERR_NO_EPOCH  -3   /* epoch not retained, or its journal not written */

typedef struct {
    fat_block_device_t* dev;
    uint32_t base_sector;      /* first sector of region 0 */
    uint32_t capacity;         /* regions; equals the keyframe ring's capacity */
    uint32_t slot_sectors;     /* journal store slot size inside each region */
    uint32_t region_sectors;   /* derived: 1 + 2 * slot_sectors */
    journal_store_t store;     /* scratch store, re-pointed per region */
    bool     initialized;
} journal_ring_t;

/* Sectors the ring occupies on the device. */
uint32_t journal_ring_total_sectors(uint32_t capacity, uint32_t slot_sectors);

/* Bind the ring to `dev` at base_sector. Does no I/O. */
int journal_ring_init(journal_ring_t* jr, fat_block_device_t* dev,
                      uint32_t base_sector, uint32_t capacity, uint32_t slot_sectors);

/* Write an empty journal superblock into every region. */
int journal_ring_format(journal_ring_t* jr);

/* The journal store of the region paired with keyframe `epoch`'s ring slot.
 * NULL if `epoch` is not retained in `kr` (exactly; not a nearest match). */
journal_store_t* journal_ring_store_for(journal_ring_t* jr, const keyframe_ring_t* kr,
                                        uint64_t epoch);

/* Open a reader on epoch's journal. Fails with JOURNAL_RING_ERR_NO_EPOCH if the
 * epoch is not retained or its region holds another epoch's journal. */
int journal_ring_load(journal_ring_t* jr, const keyframe_ring_t* kr, uint64_t epoch,
                      journal_reader_t* reader);

#endif /* JOURNAL_RING_H */
