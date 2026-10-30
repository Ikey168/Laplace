/* Laplace: the page store behind incremental keyframes (#244). See include/page_store.h. */

#include "page_store.h"
#include "snapshot_store.h"   /* snapshot_crc32 */

static void zero(void* p, uint64_t n) {
    uint8_t* b = (uint8_t*)p;
    for (uint64_t i = 0; i < n; i++) b[i] = 0;
}

static void copy(void* dst, const void* src, uint64_t n) {
    uint8_t* d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;
    for (uint64_t i = 0; i < n; i++) d[i] = s[i];
}

/* ---- Pool ---- */

uint64_t ps_pool_bytes(uint32_t nslots) {
    return (uint64_t)nslots * (sizeof(uint16_t) + sizeof(uint32_t) + 1) + 8;
}

void ps_pool_init(ps_pool_t* p, uint32_t nslots, void* mem) {
    uint8_t* m = (uint8_t*)mem;
    zero(m, ps_pool_bytes(nslots));
    p->nslots = nslots;
    p->crc = (uint32_t*)m;                         /* 4-byte aligned first */
    p->refs = (uint16_t*)(m + (uint64_t)nslots * 4);
    p->crc_ok = m + (uint64_t)nslots * 6;
    p->hint = 0;
    p->used = 0;
}

uint32_t ps_alloc(ps_pool_t* p) {
    for (uint32_t k = 0; k < p->nslots; k++) {
        uint32_t id = (p->hint + k) % p->nslots;
        if (p->refs[id] == 0) {
            p->refs[id] = 1;
            p->crc_ok[id] = 0;
            p->hint = id + 1;
            p->used++;
            return id;
        }
    }
    return PS_NONE;
}

void ps_ref(ps_pool_t* p, uint32_t id) {
    if (id >= p->nslots) return;
    if (p->refs[id]++ == 0) p->used++;
}

void ps_unref(ps_pool_t* p, uint32_t id) {
    if (id >= p->nslots || p->refs[id] == 0) return;
    if (--p->refs[id] == 0) {
        p->used--;
        p->crc_ok[id] = 0;
    }
}

void ps_set_crc(ps_pool_t* p, uint32_t id, uint32_t crc) {
    if (id >= p->nslots) return;
    p->crc[id] = crc;
    p->crc_ok[id] = 1;
}

bool ps_get_crc(const ps_pool_t* p, uint32_t id, uint32_t* crc) {
    if (id >= p->nslots || !p->crc_ok[id]) return false;
    *crc = p->crc[id];
    return true;
}

/* ---- Manifests ---- */

void ps_mf_init(ps_manifest_t* m, uint64_t epoch) {
    m->epoch = epoch;
    m->nruns = 0;
    m->npages = 0;
    /* runs and cap are kept: a manifest's buffer is reused. */
}

static int reserve(ps_manifest_t* m, uint32_t need, ps_grow_fn grow) {
    if (need <= m->cap) return PS_OK;
    uint32_t want = m->cap ? m->cap : 16;
    while (want < need) want *= 2;
    ps_run_t* r = grow(m->runs, m->cap, want);
    if (!r) return PS_ERR_FULL;
    m->runs = r;
    m->cap = want;
    return PS_OK;
}

int ps_mf_add(ps_manifest_t* m, uint32_t pid, uint64_t va, uint32_t id, ps_grow_fn grow) {
    if (m->nruns) {
        ps_run_t* last = &m->runs[m->nruns - 1];
        uint64_t end = last->va + (uint64_t)last->npages * 4096;
        if (pid < last->pid || (pid == last->pid && va < end)) return PS_ERR_PARAM;
        if (pid == last->pid && va == end && id == last->id + last->npages) {
            last->npages++;
            m->npages++;
            return PS_OK;
        }
    }
    if (reserve(m, m->nruns + 1, grow) != PS_OK) return PS_ERR_FULL;
    ps_run_t r = { pid, 1, va, id, 0 };
    m->runs[m->nruns++] = r;
    m->npages++;
    return PS_OK;
}

uint32_t ps_mf_find(const ps_manifest_t* m, uint32_t pid, uint64_t va) {
    uint32_t lo = 0, hi = m->nruns;
    while (lo < hi) {                       /* first run past (pid, va) */
        uint32_t mid = lo + (hi - lo) / 2;
        const ps_run_t* r = &m->runs[mid];
        if (r->pid < pid || (r->pid == pid && r->va <= va)) lo = mid + 1;
        else hi = mid;
    }
    if (lo == 0) return PS_NONE;
    const ps_run_t* r = &m->runs[lo - 1];
    if (r->pid != pid || va >= r->va + (uint64_t)r->npages * 4096 || (va & 0xFFF)) return PS_NONE;
    return r->id + (uint32_t)((va - r->va) / 4096);
}

void ps_mf_retain(const ps_manifest_t* m, ps_pool_t* p) {
    for (uint32_t i = 0; i < m->nruns; i++) {
        for (uint32_t k = 0; k < m->runs[i].npages; k++) ps_ref(p, m->runs[i].id + k);
    }
}

void ps_mf_release(const ps_manifest_t* m, ps_pool_t* p) {
    for (uint32_t i = 0; i < m->nruns; i++) {
        for (uint32_t k = 0; k < m->runs[i].npages; k++) ps_unref(p, m->runs[i].id + k);
    }
}

#define MF_MAGIC 0x31464D50u   /* "PMF1" */

typedef struct {
    uint32_t magic;
    uint32_t nruns;
    uint64_t epoch;
    uint32_t crc;          /* of the runs */
    uint32_t reserved;
} mf_header_t;

uint32_t ps_mf_bytes(const ps_manifest_t* m) {
    return (uint32_t)sizeof(mf_header_t) + m->nruns * (uint32_t)sizeof(ps_run_t);
}

uint32_t ps_mf_encode(const ps_manifest_t* m, uint8_t* out, uint32_t cap) {
    uint32_t n = ps_mf_bytes(m);
    if (n > cap) return 0;
    uint32_t body = m->nruns * (uint32_t)sizeof(ps_run_t);
    mf_header_t h = { MF_MAGIC, m->nruns, m->epoch, snapshot_crc32(0, m->runs, body), 0 };
    copy(out, &h, sizeof(h));
    copy(out + sizeof(h), m->runs, body);
    return n;
}

int ps_mf_decode(ps_manifest_t* m, const uint8_t* in, uint32_t len, uint32_t nslots,
                 ps_grow_fn grow) {
    mf_header_t h;
    if (len < sizeof(h)) return PS_ERR_CORRUPT;
    copy(&h, in, sizeof(h));
    if (h.magic != MF_MAGIC || h.nruns > (len - sizeof(h)) / sizeof(ps_run_t) ||
        len != sizeof(h) + h.nruns * sizeof(ps_run_t) ||
        snapshot_crc32(0, in + sizeof(h), h.nruns * (uint32_t)sizeof(ps_run_t)) != h.crc) {
        return PS_ERR_CORRUPT;
    }
    ps_mf_init(m, h.epoch);
    if (reserve(m, h.nruns ? h.nruns : 1, grow) != PS_OK) return PS_ERR_FULL;
    copy(m->runs, in + sizeof(h), h.nruns * sizeof(ps_run_t));
    m->nruns = h.nruns;
    for (uint32_t i = 0; i < m->nruns; i++) {
        const ps_run_t* r = &m->runs[i];
        if (r->npages == 0 || r->id >= nslots || r->npages > nslots - r->id || (r->va & 0xFFF)) {
            return PS_ERR_CORRUPT;
        }
        if (i) {
            const ps_run_t* q = &m->runs[i - 1];
            if (r->pid < q->pid || (r->pid == q->pid && r->va < q->va + (uint64_t)q->npages * 4096)) {
                return PS_ERR_CORRUPT;
            }
        }
        m->npages += r->npages;
    }
    return PS_OK;
}

/* ---- Frame cache ---- */

uint64_t ps_cache_bytes(uint32_t cap) {
    return (uint64_t)cap * (sizeof(uint32_t) + sizeof(uint64_t) + sizeof(uint32_t));
}

void ps_cache_init(ps_cache_t* c, uint32_t cap, void* mem) {
    uint8_t* m = (uint8_t*)mem;
    c->cap = cap;
    c->frames = (uint64_t*)m;
    c->ids = (uint32_t*)(m + (uint64_t)cap * 8);
    c->order = (uint32_t*)(m + (uint64_t)cap * 12);
    for (uint32_t i = 0; i < cap; i++) {
        c->ids[i] = PS_NONE;
        c->frames[i] = 0;
        c->order[i] = PS_NONE;
    }
    c->head = 0;
    c->flen = 0;
    c->count = 0;
}

static uint32_t slot_of(const ps_cache_t* c, uint32_t id) {
    uint32_t h = (id * 2654435761u) & (c->cap - 1);
    for (uint32_t k = 0; k < c->cap; k++) {
        uint32_t s = (h + k) & (c->cap - 1);
        if (c->ids[s] == id || c->ids[s] == PS_NONE) return s;
    }
    return PS_NONE;
}

/* Remove the entry at hash slot s, re-placing the probe chain after it. */
static void remove_at(ps_cache_t* c, uint32_t s) {
    c->ids[s] = PS_NONE;
    c->frames[s] = 0;
    c->count--;
    uint32_t i = (s + 1) & (c->cap - 1);
    while (c->ids[i] != PS_NONE) {
        uint32_t id = c->ids[i];
        uint64_t f = c->frames[i];
        c->ids[i] = PS_NONE;
        c->frames[i] = 0;
        uint32_t t = slot_of(c, id);
        c->ids[t] = id;
        c->frames[t] = f;
        i = (i + 1) & (c->cap - 1);
    }
}

uint64_t ps_cache_peek(const ps_cache_t* c, uint32_t id) {
    if (!c->cap) return 0;
    uint32_t s = slot_of(c, id);
    return s != PS_NONE && c->ids[s] == id ? c->frames[s] : 0;
}

uint64_t ps_cache_take(ps_cache_t* c, uint32_t id) {
    if (!c->cap) return 0;
    uint32_t s = slot_of(c, id);
    if (s == PS_NONE || c->ids[s] != id) return 0;
    uint64_t f = c->frames[s];
    remove_at(c, s);
    return f;   /* its FIFO entry goes stale and is skipped on eviction */
}

/* Pop the oldest FIFO entry; if it is still cached, evict it and return its
 * frame (else 0). Entries whose id was taken since are stale and just drop. */
static uint64_t pop_oldest(ps_cache_t* c) {
    uint32_t old = c->order[c->head];
    c->order[c->head] = PS_NONE;
    c->head = (c->head + 1) & (c->cap - 1);
    c->flen--;
    if (old == PS_NONE) return 0;
    uint32_t s = slot_of(c, old);
    if (s == PS_NONE || c->ids[s] != old) return 0;
    uint64_t f = c->frames[s];
    remove_at(c, s);
    return f;
}

bool ps_cache_put(ps_cache_t* c, uint32_t id, uint64_t frame, uint64_t* evicted) {
    *evicted = 0;
    if (!c->cap || ps_cache_peek(c, id)) return false;
    /* Room in the FIFO, and the table at most 3/4 full: evict the oldest
     * (at most one live entry goes per insert). */
    while (c->flen == c->cap || c->count >= c->cap - c->cap / 4) {
        uint64_t f = pop_oldest(c);
        if (f) {
            *evicted = f;
            break;
        }
    }
    if (c->flen == c->cap || c->count >= c->cap - c->cap / 4) {
        /* The one eviction was not enough (only stale entries in the way are
         * popped for free, so this needs a nearly full table): refuse. */
        return false;
    }
    uint32_t s = slot_of(c, id);
    c->ids[s] = id;
    c->frames[s] = frame;
    c->count++;
    c->order[(c->head + c->flen) & (c->cap - 1)] = id;
    c->flen++;
    return true;
}
