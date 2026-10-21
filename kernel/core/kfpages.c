/* Laplace core: user pages in keyframes, incrementally (#244). See include/core/kfpages.h. */

#include "core/kfpages.h"
#include "page_store.h"
#include "core/mm.h"
#include "core/vm.h"
#include "core/proc.h"
#include "core/machine.h"
#include "core/console.h"
#include "checkpoint.h"
#include "keyframe_store.h"
#include "divergence.h"

void* memset(void* dst, int c, size_t n);
void* memcpy(void* dst, const void* src, size_t n);

#define MAX_MF 72                       /* retained keyframes (<= 64) + the open one */

typedef struct {
    bool          used;
    bool          committed;
    ps_manifest_t m;
} mf_slot_t;

static mf_slot_t           g_mf[MAX_MF];
static int                 g_ref = -1;   /* the keyframe KNOWN pages are relative to */
static int                 g_open = -1;  /* taken, not yet written */
static ps_pool_t           g_pool;
static void*               g_pool_mem;
static fat_block_device_t* g_dev;
static uint32_t            g_base;
static ps_cache_t          g_extra;      /* pristine frames, kept across restores */
static ps_cache_t          g_harv;       /* frames a restore kept, for one restore */
static void*               g_harv_mem;
static kfp_stats_t         g_st;
static uint64_t            g_dirty;

/* The open keyframe's pages that need writing to the pool. */
typedef struct {
    uint32_t pid;
    uint32_t id;
    uint64_t va;
    bool     done;
} newpage_t;
static newpage_t* g_new;
static uint32_t   g_nnew, g_newcap;

const kfp_stats_t* kfp_stats(void) {
    g_st.slots_used = g_pool.used;
    g_st.slots_total = g_pool.nslots;
    return &g_st;
}

uint64_t kfp_dirty(void) { return g_dirty; }

static ps_run_t* grow(ps_run_t* old, uint32_t old_cap, uint32_t need) {
    ps_run_t* n = (ps_run_t*)kmalloc((size_t)need * sizeof(ps_run_t));
    if (!n) return 0;
    if (old) {
        memcpy(n, old, (size_t)old_cap * sizeof(ps_run_t));
        kfree(old);
    }
    return n;
}

static uint32_t pow2_at_least(uint64_t n) {
    uint32_t c = 64;
    while (c < n && c < (1u << 30)) c <<= 1;
    return c;
}

/* ---- Setup ---- */

int kfp_init(fat_block_device_t* dev, uint32_t base, uint32_t nslots) {
    for (int i = 0; i < MAX_MF; i++) {
        if (g_mf[i].m.runs) kfree(g_mf[i].m.runs);
        memset(&g_mf[i], 0, sizeof(g_mf[i]));
    }
    g_ref = g_open = -1;
    if (g_pool_mem) kfree(g_pool_mem);
    g_pool_mem = kmalloc(ps_pool_bytes(nslots));
    if (!g_pool_mem) return -1;
    ps_pool_init(&g_pool, nslots, g_pool_mem);
    g_dev = dev;
    g_base = base;
    if (!g_extra.cap) {
        /* Pristine copies: up to 1/16 of free memory, at most 16384 frames. */
        uint64_t frames = pmm_free_frames() / 16;
        uint32_t cap = pow2_at_least(frames < 16384 ? frames : 16384);
        if (cap > frames * 2 && cap > 64) cap >>= 1;
        void* mem = kmalloc(ps_cache_bytes(cap));
        if (!mem) return -1;
        ps_cache_init(&g_extra, cap, mem);
    }
    memset(&g_st, 0, sizeof(g_st));
    g_dirty = 0;
    return 0;
}

static int find_mf(uint64_t epoch) {
    for (int i = 0; i < MAX_MF; i++) {
        if (g_mf[i].used && g_mf[i].m.epoch == epoch) return i;
    }
    return -1;
}

static int free_mf(void) {
    for (int i = 0; i < MAX_MF; i++) {
        if (!g_mf[i].used) return i;
    }
    return -1;
}

bool kfp_has(uint64_t epoch) { return find_mf(epoch) >= 0; }

int kfp_adopt(uint64_t epoch, const uint8_t* blob, uint32_t len) {
    int i = free_mf();
    if (i < 0 || find_mf(epoch) >= 0) return -1;
    if (ps_mf_decode(&g_mf[i].m, blob, len, g_pool.nslots, grow) != PS_OK || g_mf[i].m.epoch != epoch) {
        return -1;
    }
    ps_mf_retain(&g_mf[i].m, &g_pool);
    g_mf[i].used = true;
    g_mf[i].committed = true;
    return 0;
}

static void drop_mf(int i) {
    ps_mf_release(&g_mf[i].m, &g_pool);
    g_mf[i].used = false;
    g_mf[i].committed = false;
    if (g_ref == i) g_ref = -1;
}

/* ---- Pool I/O ---- */

static void write_slot(uint32_t id, const void* data) {
    if (g_dev->write_sectors(g_dev->private_data, g_base + id * 8, 8, data) != 0) {
        panic("timetravel: cannot write page slot %u", id);
    }
    ps_set_crc(&g_pool, id, divergence_checksum(0, data, PAGE_SIZE));
}

static bool read_slot(uint32_t id, void* data) {
    return g_dev->read_sectors(g_dev->private_data, g_base + id * 8, 8, data) == 0;
}

/* ---- Taking a keyframe ---- */

static uint32_t ref_slot(uint32_t pid, uint64_t va) {
    return g_ref >= 0 ? ps_mf_find(&g_mf[g_ref].m, pid, va) : PS_NONE;
}

static void push_new(uint32_t pid, uint64_t va, uint32_t id) {
    if (g_nnew == g_newcap) {
        uint32_t cap = g_newcap ? g_newcap * 2 : 1024;
        newpage_t* n = (newpage_t*)kmalloc((size_t)cap * sizeof(newpage_t));
        if (!n) panic("timetravel: out of memory listing a keyframe's pages");
        if (g_new) {
            memcpy(n, g_new, (size_t)g_nnew * sizeof(newpage_t));
            kfree(g_new);
        }
        g_new = n;
        g_newcap = cap;
    }
    newpage_t e = { pid, id, va, false };
    g_new[g_nnew++] = e;
}

typedef struct {
    ps_manifest_t* m;
    uint32_t       pid;
} build_ctx_t;

static void build_one(void* c, uint64_t va, pte_t* pte) {
    build_ctx_t* b = (build_ctx_t*)c;
    uint32_t id = (*pte & PAGE_KF_KNOWN) ? ref_slot(b->pid, va) : PS_NONE;
    if (id != PS_NONE) {
        ps_ref(&g_pool, id);
        g_st.pages_reused++;
    } else {
        id = ps_alloc(&g_pool);
        if (id == PS_NONE) {
            panic("timetravel: the page store is full (%u slots); give the recording a larger disk",
                  g_pool.nslots);
        }
        push_new(b->pid, va, id);
    }
    if (ps_mf_add(b->m, b->pid, va, id, grow) != PS_OK) panic("timetravel: cannot list keyframe pages");
}

/* Processes in ascending pid order (manifests are sorted by pid). */
static uint32_t sorted_procs(process_t** out, uint32_t cap) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < proc_count() && n < cap; i++) {
        process_t* p = proc_at(i);
        uint32_t k = n++;
        while (k > 0 && out[k - 1]->pid > p->pid) {
            out[k] = out[k - 1];
            k--;
        }
        out[k] = p;
    }
    return n;
}

void kfp_begin_take(void) {
    if (g_open >= 0) panic("timetravel: a keyframe is still unwritten");
    int i = free_mf();
    if (i < 0) panic("timetravel: too many keyframes held");
    ps_mf_init(&g_mf[i].m, 0);
    g_nnew = 0;
    static process_t* procs[LP_MAX_PROCS];
    uint32_t n = sorted_procs(procs, LP_MAX_PROCS);
    for (uint32_t k = 0; k < n; k++) {
        build_ctx_t b = { &g_mf[i].m, (uint32_t)procs[k]->pid };
        vmm_for_each_pte(procs[k]->address_space, build_one, &b);
    }
    g_mf[i].used = true;
    g_mf[i].committed = false;
    g_open = i;
}

typedef struct {
    vm_space_t*  space;
    vm_region_t* r;
    int          marked;
    bool         current;
} mark_ctx_t;

static void mark_one(void* c, uint64_t va, pte_t* pte) {
    mark_ctx_t* m = (mark_ctx_t*)c;
    while (m->r && m->r->end_addr <= va) m->r = m->r->next;
    bool writable = m->r && va >= m->r->start_addr && (m->r->flags & VMM_FLAG_WRITE);
    pte_t e = *pte;
    if (writable && (e & PAGE_WRITABLE)) {
        e = (e & ~(pte_t)PAGE_WRITABLE) | PAGE_SNAPSHOT_COW;
        m->marked++;
    }
    if (!(e & PAGE_WRITABLE)) e |= PAGE_KF_KNOWN;
    if (e != *pte) {
        *pte = e;
        if (m->current) vmm_flush_tlb_page(va);
    }
}

int kfp_mark_space(vm_space_t* space, uint64_t epoch) {
    if (!space) return -1;
    mark_ctx_t m = { space, space->regions, 0, space == vmm_get_current_space() };
    vmm_for_each_pte(space, mark_one, &m);
    space->checkpoint_epoch = epoch;
    return m.marked;
}

void kfp_end_take(uint64_t epoch, uint32_t tag) {
    if (g_open < 0) panic("timetravel: no keyframe is being taken");
    ps_manifest_t* m = &g_mf[g_open].m;
    m->epoch = epoch;
    g_ref = g_open;
    uint32_t n = ps_mf_bytes(m);
    uint8_t* blob = (uint8_t*)kmalloc(n);
    if (!blob || ps_mf_encode(m, blob, n) != n ||
        checkpoint_capture_kernel_blob(tag, blob, n) != CHECKPOINT_OK) {
        panic("timetravel: cannot capture the page manifest");
    }
    kfree(blob);
    g_st.last_written = g_nnew;
    g_st.last_pages = m->npages;
    g_st.pages_written += g_nnew;
    g_dirty = 0;
}

/* ---- Writeback ---- */

static newpage_t* find_new(uint32_t pid, uint64_t va) {
    uint32_t lo = 0, hi = g_nnew;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        newpage_t* e = &g_new[mid];
        if (e->pid == pid && e->va == va) return e;
        if (e->pid < pid || (e->pid == pid && e->va < va)) lo = mid + 1;
        else hi = mid;
    }
    return 0;
}

static bool in_ring(keyframe_store_t* ks, uint64_t epoch) {
    uint32_t slot;
    uint64_t found;
    return keyframe_ring_find(keyframe_store_ring(ks), epoch, &slot, &found) && found == epoch;
}

int kfp_writeback(void) {
    keyframe_store_t* ks = keyframe_store_get();
    if (!ks || g_open < 0) return CHECKPOINT_ERR_PARAM;
    uint64_t epoch = checkpoint_current_epoch();
    if (g_mf[g_open].m.epoch != epoch) return CHECKPOINT_ERR_PARAM;

    snapshot_writer_t w;
    if (keyframe_store_begin(ks, epoch, &w) != KEYFRAME_STORE_OK) return CHECKPOINT_ERR_IO;
    for (checkpoint_capture_t* c = checkpoint_captures(); c; c = c->next) {
        if (c->flags & (CHECKPOINT_REC_KERNEL | CHECKPOINT_REC_CONTEXT)) {
            if (snapshot_writer_add_page(&w, c->pid, c->virt_addr, c->flags, c->data) != SNAPSHOT_OK) {
                return CHECKPOINT_ERR_IO;
            }
            continue;
        }
        /* A page written since the take: its capture is the keyframe's image. */
        newpage_t* e = find_new(c->pid, c->virt_addr);
        if (e && !e->done) {
            write_slot(e->id, c->data);
            e->done = true;
        }
    }
    for (uint32_t i = 0; i < g_nnew; i++) {
        newpage_t* e = &g_new[i];
        if (e->done) continue;
        process_t* p = proc_by_pid(e->pid);
        uint64_t phys = p ? vmm_get_physical_addr(p->address_space, e->va) : 0;
        if (!phys) panic("timetravel: keyframe page %lx of pid %u vanished", e->va, e->pid);
        write_slot(e->id, P2V(phys & ~0xFFFULL));
        e->done = true;
    }
    if (keyframe_store_commit(ks, &w, epoch) != KEYFRAME_STORE_OK) return CHECKPOINT_ERR_IO;
    checkpoint_after_commit(epoch);
    g_mf[g_open].committed = true;
    g_open = -1;
    g_nnew = 0;
    /* Keyframes the ring no longer holds release their slots. */
    for (int i = 0; i < MAX_MF; i++) {
        if (g_mf[i].used && g_mf[i].committed && !in_ring(ks, g_mf[i].m.epoch)) drop_mf(i);
    }
    return CHECKPOINT_OK;
}

/* ---- Restore ---- */

typedef struct {
    uint32_t pid;
    uint64_t count;
    bool     take;
    vm_space_t* space;
} harv_ctx_t;

static void harvest_one(void* c, uint64_t va, pte_t* pte) {
    harv_ctx_t* h = (harv_ctx_t*)c;
    if (!(*pte & PAGE_KF_KNOWN)) return;
    uint32_t id = ref_slot(h->pid, va);
    if (id == PS_NONE) return;
    if (!h->take) {
        h->count++;
        return;
    }
    uint64_t ev = 0;
    if (ps_cache_put(&g_harv, id, *pte & LP_PTE_ADDR_MASK, &ev)) {
        *pte = 0;                 /* the frame is the cache's now */
        h->space->page_count--;
    }
    if (ev) pmm_free(ev);
}

void kfp_harvest(void) {
    if (g_harv_mem) {
        kfree(g_harv_mem);
        g_harv_mem = 0;
        g_harv.cap = 0;
    }
    if (g_ref < 0) return;
    uint64_t count = 0;
    for (uint32_t i = 0; i < proc_count(); i++) {
        harv_ctx_t h = { (uint32_t)proc_at(i)->pid, 0, false, proc_at(i)->address_space };
        vmm_for_each_pte(h.space, harvest_one, &h);
        count += h.count;
    }
    if (!count) return;
    uint32_t cap = pow2_at_least(count * 2);
    g_harv_mem = kmalloc(ps_cache_bytes(cap));
    if (!g_harv_mem) return;   /* no reuse this time; the pages are reread */
    ps_cache_init(&g_harv, cap, g_harv_mem);
    for (uint32_t i = 0; i < proc_count(); i++) {
        harv_ctx_t h = { (uint32_t)proc_at(i)->pid, 0, true, proc_at(i)->address_space };
        vmm_for_each_pte(h.space, harvest_one, &h);
    }
}

static void stash_pristine(uint32_t id, uint64_t frame) {
    uint64_t ev = 0;
    if (!ps_cache_put(&g_extra, id, frame, &ev)) pmm_free(frame);
    if (ev) pmm_free(ev);
}

int kfp_map(uint64_t epoch) {
    int idx = find_mf(epoch);
    int rc = 0;
    if (idx < 0) rc = -1;
    const ps_manifest_t* m = idx >= 0 ? &g_mf[idx].m : 0;
    process_t* p = 0;
    vm_region_t* r = 0;
    for (uint32_t i = 0; m && rc == 0 && i < m->nruns; i++) {
        const ps_run_t* run = &m->runs[i];
        if (!p || (uint32_t)p->pid != run->pid) {
            p = proc_by_pid(run->pid);
            if (!p) { rc = -1; break; }
            r = p->address_space->regions;
        }
        for (uint32_t k = 0; k < run->npages; k++) {
            uint64_t va = run->va + (uint64_t)k * PAGE_SIZE;
            uint32_t id = run->id + k;
            while (r && r->end_addr <= va) r = r->next;
            if (!r || va < r->start_addr) { rc = -1; break; }
            uint64_t phys = g_harv.cap ? ps_cache_take(&g_harv, id) : 0;
            if (phys) {
                g_st.restore_kept++;
            } else {
                phys = pmm_alloc();
                if (!phys) { rc = -1; break; }
                uint64_t f = ps_cache_peek(&g_extra, id);
                if (f) {
                    memcpy(P2V(phys), P2V(f), PAGE_SIZE);
                    g_st.restore_cached++;
                } else if (read_slot(id, P2V(phys))) {
                    g_st.restore_read++;
                } else {
                    pmm_free(phys);
                    rc = -1;
                    break;
                }
            }
            uint32_t flags = vm_pte_flags(r);
            if (r->flags & VMM_FLAG_WRITE) flags = (flags & ~PAGE_WRITABLE) | PAGE_SNAPSHOT_COW;
            if (vmm_map_page(p->address_space, va, phys, flags | PAGE_KF_KNOWN) != 0) {
                pmm_free(phys);
                rc = -1;
                break;
            }
        }
    }
    /* Frames the target did not need are still pristine copies of their slots. */
    for (uint32_t s = 0; s < g_harv.cap; s++) {
        if (g_harv.ids[s] != PS_NONE) stash_pristine(g_harv.ids[s], g_harv.frames[s]);
    }
    if (g_harv_mem) {
        kfree(g_harv_mem);
        g_harv_mem = 0;
        g_harv.cap = 0;
    }
    g_ref = rc == 0 ? idx : -1;
    return rc;
}

/* ---- Hooks ---- */

void kfp_before_write(vm_space_t* s, uint64_t va) {
    pte_t* pte = vmm_get_page_table(s, va & ~0xFFFULL, PT_LEVEL, false);
    if (!pte || !(*pte & PAGE_PRESENT) || !(*pte & PAGE_KF_KNOWN) || !(*pte & PAGE_SNAPSHOT_COW)) return;
    if (machine_mode() == MACHINE_LIVE) {
        g_dirty++;
        return;
    }
    /* Replay: keep the page as the keyframe has it, for the next restore. */
    uint32_t id = ref_slot(s->owner_pid, va & ~0xFFFULL);
    if (id == PS_NONE || ps_cache_peek(&g_extra, id)) return;
    uint64_t copy = pmm_alloc();
    if (!copy) return;
    memcpy(P2V(copy), P2V(*pte & LP_PTE_ADDR_MASK), PAGE_SIZE);
    stash_pristine(id, copy);
}

void kfp_page_added(void) {
    if (machine_mode() == MACHINE_LIVE) g_dirty++;
}

uint32_t kfp_page_crc(uint32_t pid, uint64_t va, uint64_t phys, pte_t pte) {
    uint32_t id = (pte & PAGE_KF_KNOWN) ? ref_slot(pid, va) : PS_NONE;
    uint32_t crc;
    if (id != PS_NONE && ps_get_crc(&g_pool, id, &crc)) return crc;
    crc = divergence_checksum(0, P2V(phys & ~0xFFFULL), PAGE_SIZE);
    if (id != PS_NONE) ps_set_crc(&g_pool, id, crc);
    return crc;
}
