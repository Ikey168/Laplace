/* Laplace core: user pages in keyframes, incrementally (#244).
 *
 * The keyframe store's slots hold kernel state, registers, and a page
 * manifest; the pages themselves live in a reference-counted pool on the same
 * disk (include/page_store.h). The kernel tracks which pages are unchanged
 * since a reference keyframe with a software PTE bit, PAGE_KF_KNOWN:
 *
 *   - Taking keyframe E sets it on every present page; a page keeps it until
 *     it is written (the snapshot-COW fault clears it) or moved (mremap).
 *     Restoring E sets it on every page mapped, so replay tracks writes too.
 *   - A KNOWN page has the slot the reference keyframe gave it. At the next
 *     keyframe it keeps that slot; only the others are written to the pool.
 *   - A KNOWN page's CRC is the slot's, so checksumming memory at an epoch
 *     boundary hashes only the pages written since.
 *   - Restore keeps the frames of KNOWN pages whose slot the target keyframe
 *     names, and replay saves a page's pristine copy at its first write, so
 *     going back again within the window rereads little or nothing.
 *
 * A KNOWN page is never writable without a fault: writable pages carry the
 * snapshot-COW tag instead of the write bit.
 */

#ifndef CORE_KFPAGES_H
#define CORE_KFPAGES_H

#include <stdint.h>
#include <stdbool.h>
#include "vmm.h"
#include "fat.h"

typedef struct {
    uint64_t pages_written;     /* to the pool, all keyframes */
    uint64_t pages_reused;      /* kept their slot */
    uint64_t last_written;      /* the newest keyframe */
    uint64_t last_pages;
    uint64_t restore_kept;      /* frames a restore kept */
    uint64_t restore_cached;    /* copied from the cache */
    uint64_t restore_read;      /* read from the pool */
    uint32_t slots_used;
    uint32_t slots_total;
} kfp_stats_t;

/* Bind the pool: `nslots` page slots from `base` on `dev`. Forgets every
 * manifest. */
int  kfp_init(fat_block_device_t* dev, uint32_t base, uint32_t nslots);

/* Boot: adopt a retained keyframe's manifest (serialized form). */
int  kfp_adopt(uint64_t epoch, const uint8_t* blob, uint32_t len);
bool kfp_has(uint64_t epoch);

/* Taking keyframe E: build its manifest from the live pages (before
 * checkpoint_take marks them), then, after checkpoint_take, capture the
 * manifest into the keyframe. */
void kfp_begin_take(void);
void kfp_end_take(uint64_t epoch, uint32_t tag);

/* Write the open keyframe: its kernel records and manifest to a slot, its new
 * pages to the pool; commit; release keyframes the ring evicted. */
int  kfp_writeback(void);

/* Restoring keyframe E: harvest reusable frames from the live machine (before
 * it is torn down), then map E's pages into the rebuilt one. */
void kfp_harvest(void);
int  kfp_map(uint64_t epoch);

/* Hooks: a snapshot-COW write fault on `va` is about to be resolved; a page
 * was mapped on first touch. */
void kfp_before_write(vm_space_t* s, uint64_t va);
void kfp_page_added(void);
/* Pages dirtied or added since the last keyframe (recording). */
uint64_t kfp_dirty(void);

/* The CRC of a present page (cached for KNOWN pages). */
uint32_t kfp_page_crc(uint32_t pid, uint64_t va, uint64_t phys, pte_t pte);

/* Mark function for checkpoint_take. */
int kfp_mark_space(vm_space_t* space, uint64_t epoch);

const kfp_stats_t* kfp_stats(void);

#endif /* CORE_KFPAGES_H */
