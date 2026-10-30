/* Laplace: the page store behind incremental keyframes (#244).
 *
 * User pages live in a pool of page-sized slots on the recording disk, one
 * slot per distinct page content a retained keyframe needs. A keyframe is a
 * manifest: runs of (pid, first virtual address, page count, first slot id),
 * sorted by pid and address. A page that did not change since the previous
 * keyframe keeps its slot id, so a keyframe writes only the pages dirtied in
 * the epoch before it, and its manifest grows with the number of runs, not
 * with resident memory.
 *
 * Slots are reference-counted by the manifests that name them. Evicting a
 * keyframe releases its manifest; a slot is free when no retained keyframe
 * names it. Counts live in memory and are rebuilt at boot from the retained
 * manifests, so a power cut between writing a keyframe's pages and committing
 * it leaves those slots free again.
 *
 * Also here: a CRC per slot (so checksumming memory costs only the dirty
 * pages), and a small id -> frame cache for restore. Pure and host-tested
 * (tests/test_page_store.c); the caller provides all memory.
 */

#ifndef PAGE_STORE_H
#define PAGE_STORE_H

#include <stdint.h>
#include <stdbool.h>

#define PS_NONE 0xFFFFFFFFu

#define PS_OK          0
#define PS_ERR_PARAM  -1
#define PS_ERR_FULL   -2
#define PS_ERR_CORRUPT -3

/* ---- Slot pool ---- */

typedef struct {
    uint32_t  nslots;
    uint16_t* refs;       /* per slot: retaining manifests (0 = free) */
    uint32_t* crc;        /* per slot: CRC of its content, valid if crc_ok */
    uint8_t*  crc_ok;     /* per slot */
    uint32_t  hint;       /* next slot to try */
    uint32_t  used;
} ps_pool_t;

/* Bytes of caller memory a pool of `nslots` needs (refs, crc, crc_ok). */
uint64_t ps_pool_bytes(uint32_t nslots);
void     ps_pool_init(ps_pool_t* p, uint32_t nslots, void* mem);
/* A free slot, with one reference; PS_NONE when the pool is full. Successive
 * allocations are consecutive while the pool is unfragmented. */
uint32_t ps_alloc(ps_pool_t* p);
void     ps_ref(ps_pool_t* p, uint32_t id);
void     ps_unref(ps_pool_t* p, uint32_t id);
void     ps_set_crc(ps_pool_t* p, uint32_t id, uint32_t crc);
bool     ps_get_crc(const ps_pool_t* p, uint32_t id, uint32_t* crc);

/* ---- Manifests ---- */

typedef struct {
    uint32_t pid;
    uint32_t npages;
    uint64_t va;        /* first page */
    uint32_t id;        /* slot of the first page; page k is in slot id + k */
    uint32_t reserved;
} ps_run_t;

typedef struct {
    uint64_t  epoch;
    uint32_t  nruns;
    uint32_t  cap;
    ps_run_t* runs;
    uint64_t  npages;
} ps_manifest_t;

/* Allocation is the caller's: these grow `runs` through `grow`, which returns
 * a buffer for at least `need` runs holding the old contents (or NULL). */
typedef ps_run_t* (*ps_grow_fn)(ps_run_t* old, uint32_t old_cap, uint32_t need);

void ps_mf_init(ps_manifest_t* m, uint64_t epoch);
/* Append a page; pages must come in ascending (pid, va) order. Coalesces with
 * the last run when both the address and the slot continue it. */
int  ps_mf_add(ps_manifest_t* m, uint32_t pid, uint64_t va, uint32_t id, ps_grow_fn grow);
/* The slot holding (pid, va), or PS_NONE. */
uint32_t ps_mf_find(const ps_manifest_t* m, uint32_t pid, uint64_t va);

/* Take or drop one reference on every slot the manifest names. */
void ps_mf_retain(const ps_manifest_t* m, ps_pool_t* p);
void ps_mf_release(const ps_manifest_t* m, ps_pool_t* p);

/* Serialized form: epoch, run count, runs; CRC-checked. */
uint32_t ps_mf_bytes(const ps_manifest_t* m);
uint32_t ps_mf_encode(const ps_manifest_t* m, uint8_t* out, uint32_t cap);
/* Decode into m (runs from grow). PS_ERR_CORRUPT on a bad blob, or on runs
 * out of order or naming slots past `nslots`. */
int      ps_mf_decode(ps_manifest_t* m, const uint8_t* in, uint32_t len, uint32_t nslots,
                      ps_grow_fn grow);

/* ---- Frame cache: slot id -> a frame holding that slot's content ---- */

typedef struct {
    uint32_t  cap;        /* entries; a power of two */
    uint32_t* ids;        /* PS_NONE = empty */
    uint64_t* frames;
    uint32_t* order;      /* FIFO ring of inserted ids, for eviction */
    uint32_t  head, flen; /* ring start and length (entries may be stale) */
    uint32_t  count;      /* live entries */
} ps_cache_t;

uint64_t ps_cache_bytes(uint32_t cap);
void     ps_cache_init(ps_cache_t* c, uint32_t cap, void* mem);
/* Insert; when full, the oldest entry is evicted and its frame returned in
 * *evicted (else 0). False if `id` is already present (frame not taken). */
bool     ps_cache_put(ps_cache_t* c, uint32_t id, uint64_t frame, uint64_t* evicted);
/* The frame for `id` without removing it, or 0. */
uint64_t ps_cache_peek(const ps_cache_t* c, uint32_t id);
/* Remove and return the frame for `id`, or 0. */
uint64_t ps_cache_take(ps_cache_t* c, uint32_t id);

#endif /* PAGE_STORE_H */
