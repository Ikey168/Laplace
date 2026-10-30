/* Host-side unit test for the page store (#244).
 *
 * Verifies:
 *   1. The pool hands out consecutive slots, counts references, and frees a
 *      slot only when its last reference goes.
 *   2. Manifests coalesce runs, find any page by (pid, va), and reject pages
 *      out of order.
 *   3. Incremental keyframes: a sequence of keyframes over a large resident
 *      set with a small working set allocates slots only for dirtied pages;
 *      evicting old keyframes (including the full base) keeps every slot the
 *      retained ones name and frees the rest.
 *   4. Manifests round-trip through their serialized form; corruption, bad
 *      slot ids, and disorder are refused.
 *   5. Rebuilding the counts from the retained manifests (a boot) gives the
 *      same pool state.
 *   6. The frame cache finds, takes, and evicts oldest-first, and never
 *      loses a frame (every frame put is returned by take or eviction).
 *
 * Build: gcc -I../include -o test_page_store \
 *            test_page_store.c ../kernel/page_store.c ../kernel/snapshot_store.c
 */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
extern int printf(const char*, ...);

#include "page_store.h"

void* kmalloc(size_t s) { return malloc(s); }
void  kfree(void* p) { free(p); }

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("  FAIL: %s\n", msg); failures++; } \
    else { printf("  ok:   %s\n", msg); } \
} while (0)

static ps_run_t* grow(ps_run_t* old, uint32_t old_cap, uint32_t need) {
    (void)old_cap;
    return (ps_run_t*)realloc(old, (size_t)need * sizeof(ps_run_t));
}

#define RESIDENT 4096        /* pages of one process */
#define WORKSET  8           /* pages it dirties per epoch */
#define RETAIN   4           /* keyframes retained */

/* The current id of each page, as the kernel's reference manifest knows it. */
static uint32_t g_cur[RESIDENT];

int main(void) {
    printf("=== Page store (#244) unit test ===\n");

    /* --- 1. pool --- */
    {
        ps_pool_t p;
        void* mem = malloc(ps_pool_bytes(8));
        ps_pool_init(&p, 8, mem);
        uint32_t a = ps_alloc(&p), b = ps_alloc(&p), c = ps_alloc(&p);
        CHECK(a == 0 && b == 1 && c == 2, "fresh slots are consecutive");
        ps_ref(&p, b);
        ps_unref(&p, b);
        CHECK(p.refs[b] == 1 && p.used == 3, "a slot stays while referenced");
        ps_unref(&p, b);
        CHECK(p.refs[b] == 0 && p.used == 2, "the last reference frees it");
        ps_set_crc(&p, a, 1234);
        uint32_t crc = 0;
        CHECK(ps_get_crc(&p, a, &crc) && crc == 1234 && !ps_get_crc(&p, c, &crc),
              "a slot's CRC is kept once set");
        for (int i = 0; i < 6; i++) ps_alloc(&p);
        CHECK(ps_alloc(&p) == PS_NONE, "a full pool says so");
        free(mem);
    }

    /* --- 2. manifests --- */
    {
        ps_manifest_t m = { 0 };
        ps_mf_init(&m, 7);
        ps_mf_add(&m, 1, 0x1000, 10, grow);
        ps_mf_add(&m, 1, 0x2000, 11, grow);
        ps_mf_add(&m, 1, 0x3000, 12, grow);
        ps_mf_add(&m, 1, 0x5000, 13, grow);   /* gap in va: new run */
        ps_mf_add(&m, 1, 0x6000, 40, grow);   /* gap in id: new run */
        ps_mf_add(&m, 2, 0x1000, 41, grow);   /* next pid */
        CHECK(m.nruns == 4 && m.npages == 6, "contiguous pages coalesce into runs");
        CHECK(ps_mf_find(&m, 1, 0x2000) == 11 && ps_mf_find(&m, 1, 0x6000) == 40 &&
              ps_mf_find(&m, 2, 0x1000) == 41, "pages are found by (pid, va)");
        CHECK(ps_mf_find(&m, 1, 0x4000) == PS_NONE && ps_mf_find(&m, 3, 0x1000) == PS_NONE &&
              ps_mf_find(&m, 1, 0x0) == PS_NONE, "absent pages are not");
        CHECK(ps_mf_add(&m, 1, 0x9000, 50, grow) == PS_ERR_PARAM, "out-of-order pages are refused");
        free(m.runs);
    }

    /* --- 3. incremental keyframes with eviction --- */
    ps_pool_t pool;
    uint32_t nslots = RESIDENT * 2;
    void* pmem = malloc(ps_pool_bytes(nslots));
    ps_pool_init(&pool, nslots, pmem);
    ps_manifest_t ring[RETAIN];
    memset(ring, 0, sizeof(ring));
    uint32_t written_first = 0, written_max_later = 0;
    uint32_t seed = 1;
    for (uint32_t k = 0; k < 20; k++) {
        ps_manifest_t* m = &ring[k % RETAIN];
        if (k >= RETAIN) ps_mf_release(m, &pool);      /* evict the oldest */
        ps_mf_init(m, k + 1);
        uint32_t written = 0;
        for (uint32_t pg = 0; pg < RESIDENT; pg++) {
            bool dirty = k == 0;
            if (k > 0) {
                for (uint32_t w = 0; w < WORKSET; w++) {
                    if (pg == (seed * (w + 1) * 2654435761u + k * 97) % RESIDENT) dirty = true;
                }
            }
            uint32_t id;
            if (dirty) {
                id = ps_alloc(&pool);
                written++;
            } else {
                id = g_cur[pg];
                ps_ref(&pool, id);
            }
            g_cur[pg] = id;
            ps_mf_add(m, 1, 0x400000 + (uint64_t)pg * 4096, id, grow);
        }
        if (k == 0) written_first = written;
        else if (written > written_max_later) written_max_later = written;
    }
    CHECK(written_first == RESIDENT, "the first keyframe writes every page");
    CHECK(written_max_later <= WORKSET, "later keyframes write only the dirtied pages");
    /* Every page of every retained keyframe has a live slot. */
    bool all_live = true;
    uint32_t referenced = 0;
    for (uint32_t i = 0; i < RETAIN; i++) {
        for (uint32_t pg = 0; pg < RESIDENT; pg++) {
            uint32_t id = ps_mf_find(&ring[i], 1, 0x400000 + (uint64_t)pg * 4096);
            if (id == PS_NONE || pool.refs[id] == 0) all_live = false;
        }
    }
    for (uint32_t id = 0; id < nslots; id++) referenced += pool.refs[id] != 0;
    CHECK(all_live, "retained keyframes keep all their slots after the base is evicted");
    CHECK(pool.used == referenced && pool.used < RESIDENT + RETAIN * WORKSET + 1,
          "evicted keyframes' slots are freed");

    /* --- 4. serialized form --- */
    ps_manifest_t* newest = &ring[19 % RETAIN];
    uint32_t nb = ps_mf_bytes(newest);
    uint8_t* blob = malloc(nb);
    CHECK(ps_mf_encode(newest, blob, nb) == nb, "a manifest encodes");
    CHECK(nb < 64 * 1024, "a manifest's size follows its runs, not resident memory");
    ps_manifest_t back = { 0 };
    CHECK(ps_mf_decode(&back, blob, nb, nslots, grow) == PS_OK && back.nruns == newest->nruns &&
          back.npages == RESIDENT && back.epoch == 20 &&
          memcmp(back.runs, newest->runs, newest->nruns * sizeof(ps_run_t)) == 0,
          "and decodes to the same runs");
    blob[nb - 3] ^= 1;
    CHECK(ps_mf_decode(&back, blob, nb, nslots, grow) == PS_ERR_CORRUPT, "a flipped bit is refused");
    blob[nb - 3] ^= 1;
    CHECK(ps_mf_decode(&back, blob, nb, 10, grow) == PS_ERR_CORRUPT, "slot ids past the pool are refused");
    CHECK(ps_mf_decode(&back, blob, nb - 1, nslots, grow) == PS_ERR_CORRUPT, "a short blob is refused");

    /* --- 5. rebuild at boot --- */
    {
        ps_pool_t again;
        void* amem = malloc(ps_pool_bytes(nslots));
        ps_pool_init(&again, nslots, amem);
        for (uint32_t i = 0; i < RETAIN; i++) ps_mf_retain(&ring[i], &again);
        CHECK(memcmp(again.refs, pool.refs, nslots * sizeof(uint16_t)) == 0 && again.used == pool.used,
              "counts rebuilt from the retained manifests match");
        free(amem);
    }

    /* --- 6. frame cache --- */
    {
        ps_cache_t c;
        void* cmem = malloc(ps_cache_bytes(16));
        ps_cache_init(&c, 16, cmem);
        uint64_t ev, returned = 0, put = 0;
        bool order_ok = true;
        for (uint32_t i = 0; i < 40; i++) {
            if (ps_cache_put(&c, 100 + i, 0x1000ULL * (i + 1), &ev)) put += 0x1000ULL * (i + 1);
            if (ev) {
                returned += ev;
                /* oldest first: the evicted frame is the oldest still cached */
                if (ev != 0x1000ULL * (i - 11)) order_ok = false;
            }
        }
        CHECK(c.count == 12, "the cache stays at most 3/4 full");
        CHECK(order_ok, "eviction is oldest-first");
        CHECK(ps_cache_peek(&c, 139) == 0x1000ULL * 40 && ps_cache_peek(&c, 100) == 0,
              "the newest entries are there, the oldest gone");
        uint64_t t = ps_cache_take(&c, 135);
        CHECK(t == 0x1000ULL * 36 && ps_cache_peek(&c, 135) == 0, "take removes an entry");
        returned += t;
        CHECK(!ps_cache_put(&c, 139, 0xDEAD000, &ev), "a present id is not put twice");
        /* Drain: every frame put comes back exactly once. */
        for (uint32_t i = 100; i < 140; i++) returned += ps_cache_take(&c, i);
        CHECK(returned == put && c.count == 0, "no frame is lost or duplicated");
        /* Takes leave stale FIFO entries; the cache keeps working past them. */
        for (uint32_t r = 0; r < 200; r++) {
            ps_cache_put(&c, 1000 + r, 0x5000, &ev);
            if (r % 3 == 0) ps_cache_take(&c, 1000 + r);
        }
        CHECK(c.count <= 12 && ps_cache_peek(&c, 1199) == 0x5000, "stale entries do not jam it");
        free(cmem);
    }

    for (uint32_t i = 0; i < RETAIN; i++) free(ring[i].runs);
    free(back.runs);
    free(blob);
    free(pmem);
    if (failures == 0) {
        printf("PASSED: keyframes share unchanged pages, and slots live exactly as long as needed\n");
        return 0;
    }
    printf("FAILED: %d check(s)\n", failures);
    return 1;
}
