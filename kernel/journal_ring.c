/* Laplace - Journal Ring (#223). See include/journal_ring.h. */

#include "journal_ring.h"

uint32_t journal_ring_total_sectors(uint32_t capacity, uint32_t slot_sectors) {
    return capacity * (1 + 2 * slot_sectors);
}

int journal_ring_init(journal_ring_t* jr, fat_block_device_t* dev,
                      uint32_t base_sector, uint32_t capacity, uint32_t slot_sectors) {
    if (!jr || !dev || capacity == 0 || capacity > KEYFRAME_RING_MAX || slot_sectors < 2) {
        return JOURNAL_RING_ERR_PARAM;
    }
    jr->dev = dev;
    jr->base_sector = base_sector;
    jr->capacity = capacity;
    jr->slot_sectors = slot_sectors;
    jr->region_sectors = 1 + 2 * slot_sectors;
    jr->initialized = true;
    return JOURNAL_RING_OK;
}

static journal_store_t* region(journal_ring_t* jr, uint32_t slot) {
    if (journal_store_init(&jr->store, jr->dev,
                           jr->base_sector + slot * jr->region_sectors,
                           jr->slot_sectors) != JOURNAL_OK) {
        return 0;
    }
    return &jr->store;
}

int journal_ring_format(journal_ring_t* jr) {
    if (!jr || !jr->initialized) return JOURNAL_RING_ERR_PARAM;
    for (uint32_t i = 0; i < jr->capacity; i++) {
        journal_store_t* js = region(jr, i);
        if (!js || journal_store_format(js) != JOURNAL_OK) return JOURNAL_RING_ERR_IO;
    }
    return JOURNAL_RING_OK;
}

journal_store_t* journal_ring_store_for(journal_ring_t* jr, const keyframe_ring_t* kr,
                                        uint64_t epoch) {
    if (!jr || !jr->initialized || !kr) return 0;
    uint32_t slot;
    uint64_t found;
    if (!keyframe_ring_find(kr, epoch, &slot, &found) || found != epoch) return 0;
    if (slot >= jr->capacity) return 0;
    return region(jr, slot);
}

int journal_ring_load(journal_ring_t* jr, const keyframe_ring_t* kr, uint64_t epoch,
                      journal_reader_t* reader) {
    if (!reader) return JOURNAL_RING_ERR_PARAM;
    journal_store_t* js = journal_ring_store_for(jr, kr, epoch);
    if (!js) return JOURNAL_RING_ERR_NO_EPOCH;
    if (journal_store_load(js, reader) != JOURNAL_OK) return JOURNAL_RING_ERR_NO_EPOCH;
    if (reader->epoch != epoch) return JOURNAL_RING_ERR_NO_EPOCH;
    return JOURNAL_RING_OK;
}
