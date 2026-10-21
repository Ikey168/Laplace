/* Laplace: the generic input journal (#242). See include/iorec.h. */

#include "iorec.h"

static void copy(void* dst, const void* src, uint64_t n) {
    __builtin_memcpy(dst, src, n);
}

static void zero(void* p, uint64_t n) {
    __builtin_memset(p, 0, n);
}

void iorec_init(iorec_log_t* l, const iorec_env_t* env) {
    zero(l, sizeof(*l));
    l->env = *env;
    l->cache_id = IOREC_NONE;
}

void iorec_reset(iorec_log_t* l) {
    l->nrecs = 0;
    l->npages = 0;
    l->len = 0;
    l->cursor = 0;
    l->cache_id = IOREC_NONE;
    l->journaled_pages = 0;
    l->journaled_recs = 0;
}

static bool grow(iorec_log_t* l, void** arr, uint32_t* cap, uint32_t need, uint32_t elem) {
    if (need <= *cap) return true;
    uint32_t n = *cap ? *cap : 64;
    while (n < need) n *= 2;
    void* a = l->env.mem_alloc((uint64_t)n * elem);
    if (!a) return false;
    if (*arr) {
        copy(a, *arr, (uint64_t)*cap * elem);
        l->env.mem_free(*arr);
    }
    *arr = a;
    *cap = n;
    return true;
}

static bool set_page(iorec_log_t* l, uint32_t index, uint32_t id) {
    if (!grow(l, (void**)&l->pages, &l->pcap, index + 1, sizeof(uint32_t))) return false;
    while (l->npages <= index) l->pages[l->npages++] = IOREC_NONE;
    l->pages[index] = id;
    return true;
}

/* Write the tail as stream page `index`. */
static int write_tail(iorec_log_t* l, uint32_t index) {
    uint32_t id = l->env.page_alloc(l->env.ctx);
    if (id == IOREC_NONE) return IOREC_ERR_FULL;
    if (l->env.page_write(l->env.ctx, id, l->tail) != 0) return IOREC_ERR_IO;
    if (!set_page(l, index, id)) return IOREC_ERR_FULL;
    return IOREC_OK;
}

int iorec_append(iorec_log_t* l, uint64_t step, uint32_t kind, uint32_t nr, uint64_t args,
                 int64_t ret, const void* payload, uint32_t len) {
    if (!grow(l, (void**)&l->recs, &l->cap, l->nrecs + 1, sizeof(iorec_t))) return IOREC_ERR_FULL;
    iorec_t r = { step, kind, nr, args, ret, l->len, len };
    const uint8_t* p = (const uint8_t*)payload;
    uint32_t done = 0;
    while (done < len) {
        uint32_t at = (uint32_t)(l->len % IOREC_PAGE);
        uint32_t chunk = IOREC_PAGE - at < len - done ? IOREC_PAGE - at : len - done;
        copy(l->tail + at, p + done, chunk);
        done += chunk;
        l->len += chunk;
        if (l->len % IOREC_PAGE == 0) {
            int rc = write_tail(l, (uint32_t)(l->len / IOREC_PAGE - 1));
            if (rc != IOREC_OK) return rc;
            zero(l->tail, IOREC_PAGE);
        }
    }
    l->recs[l->nrecs++] = r;
    return IOREC_OK;
}

int iorec_flush(iorec_log_t* l) {
    if (l->len % IOREC_PAGE == 0) return IOREC_OK;
    uint32_t index = (uint32_t)(l->len / IOREC_PAGE);
    if (index < l->npages && l->pages[index] != IOREC_NONE) return IOREC_OK;   /* already */
    return write_tail(l, index);
}

uint32_t iorec_take_events(iorec_log_t* l, iorec_event_t* out, uint32_t max) {
    uint32_t n = 0;
    while (l->journaled_recs < l->nrecs && n + 2 <= max) {
        const iorec_t* r = &l->recs[l->journaled_recs++];
        iorec_event_t h = { IOREC_EV_HEAD, (r->kind << 16) | (r->nr & 0xFFFF), r->step,
                                      (uint64_t)r->ret };
        iorec_event_t b = { IOREC_EV_BODY, r->len, r->args, r->off };
        out[n++] = h;
        out[n++] = b;
    }
    /* Pages as runs of consecutive ids: a large input is a few events. */
    while (l->journaled_pages < l->npages && n < max) {
        uint32_t i = l->journaled_pages;
        if (l->pages[i] == IOREC_NONE) break;   /* the tail, not written yet */
        uint32_t count = 1;
        while (i + count < l->npages && l->pages[i + count] == l->pages[i] + count) count++;
        iorec_event_t pg = { IOREC_EV_PAGE, 0, i, l->pages[i] | ((uint64_t)count << 32) };
        out[n++] = pg;
        l->journaled_pages += count;
    }
    return n;
}

/* ---- Replay ---- */

int iorec_load_event(iorec_log_t* l, uint32_t type, uint32_t len, uint64_t lclock, uint64_t value) {
    switch (type) {
    case IOREC_EV_HEAD: {
        if (!grow(l, (void**)&l->recs, &l->cap, l->nrecs + 1, sizeof(iorec_t))) return IOREC_ERR_FULL;
        iorec_t r = { lclock, len >> 16, len & 0xFFFF, 0, (int64_t)value, 0, 0 };
        l->recs[l->nrecs++] = r;
        l->journaled_recs = l->nrecs;
        return IOREC_OK;
    }
    case IOREC_EV_BODY:
        if (!l->nrecs) return IOREC_CORRUPT;
        l->recs[l->nrecs - 1].args = lclock;
        l->recs[l->nrecs - 1].off = value;
        l->recs[l->nrecs - 1].len = len;
        if (value + len > l->len) l->len = value + len;
        return IOREC_OK;
    case IOREC_EV_PAGE: {
        uint32_t id = (uint32_t)value, count = (uint32_t)(value >> 32);
        if (lclock > (1u << 26) || count == 0 || count > (1u << 26) || id == IOREC_NONE ||
            (uint64_t)id + count > IOREC_NONE) {
            return IOREC_CORRUPT;
        }
        for (uint32_t k = 0; k < count; k++) {
            if (!set_page(l, (uint32_t)lclock + k, id + k)) return IOREC_ERR_FULL;
        }
        if (l->journaled_pages < l->npages) l->journaled_pages = l->npages;
        return IOREC_OK;
    }
    default:
        return IOREC_OK;   /* not ours */
    }
}

int iorec_next(iorec_log_t* l, uint64_t step, uint32_t kind, uint32_t nr, uint64_t args,
               iorec_t* out) {
    if (l->cursor >= l->nrecs) return IOREC_EXHAUSTED;
    const iorec_t* r = &l->recs[l->cursor];
    if (r->step != step || r->kind != kind || r->nr != (nr & 0xFFFF) || r->args != args) {
        *out = *r;
        return IOREC_MISMATCH;
    }
    *out = *r;
    l->cursor++;
    return IOREC_OK;
}

int iorec_read(iorec_log_t* l, uint64_t off, void* buf, uint32_t len) {
    uint8_t* out = (uint8_t*)buf;
    uint32_t done = 0;
    while (done < len) {
        uint64_t pos = off + done;
        uint32_t index = (uint32_t)(pos / IOREC_PAGE);
        uint32_t at = (uint32_t)(pos % IOREC_PAGE);
        uint32_t chunk = IOREC_PAGE - at < len - done ? IOREC_PAGE - at : len - done;
        if (index >= l->npages || l->pages[index] == IOREC_NONE) {
            /* Recording continued after a replay: the live tail. */
            if (index == l->len / IOREC_PAGE) {
                copy(out + done, l->tail + at, chunk);
                done += chunk;
                continue;
            }
            return IOREC_CORRUPT;
        }
        if (l->cache_id != l->pages[index]) {
            if (l->env.page_read(l->env.ctx, l->pages[index], l->cache) != 0) return IOREC_ERR_IO;
            l->cache_id = l->pages[index];
        }
        copy(out + done, l->cache + at, chunk);
        done += chunk;
    }
    return IOREC_OK;
}

void iorec_continue(iorec_log_t* l) {
    /* Records past the cursor were not reached: the continuation replaces
     * them (a new timeline from here). */
    l->nrecs = l->cursor;
    l->journaled_recs = l->nrecs;
    l->len = (l->len + IOREC_PAGE - 1) / IOREC_PAGE * IOREC_PAGE;
    l->npages = (uint32_t)(l->len / IOREC_PAGE) < l->npages ? (uint32_t)(l->len / IOREC_PAGE) : l->npages;
    l->journaled_pages = l->npages;
    zero(l->tail, IOREC_PAGE);
}

uint32_t iorec_pages(const iorec_log_t* l, const uint32_t** ids) {
    *ids = l->pages;
    return l->npages;
}

uint64_t iorec_bytes(const iorec_log_t* l) { return l->len; }
uint32_t iorec_count(const iorec_log_t* l) { return l->nrecs; }
