/* Laplace: finding an asynchronous preemption point again by its state
 * (#245). See include/pmatch.h. Pure. */

#include "pmatch.h"

static uint64_t mix(uint64_t h, uint64_t v) {
    h ^= v + 0x9E3779B97F4A7C15ULL + (h << 6) + (h >> 2);
    h ^= h >> 29;
    h *= 0xBF58476D1CE4E5B9ULL;
    return h ^ (h >> 32);
}

uint64_t pm_page_hash(uint64_t va, const uint8_t* page) {
    uint64_t h = mix(0x5A17ULL, va);
    if (!page) return mix(h, 0xAB5E47ULL);           /* not present */
    const uint64_t* w = (const uint64_t*)page;
    uint64_t a = h, b = ~h;
    for (uint32_t i = 0; i < PM_PAGE / 8; i += 2) {
        a = (a ^ w[i]) * 0x100000001B3ULL;
        b = (b ^ w[i + 1]) * 0xC6A4A7935BD1E995ULL;
    }
    return mix(mix(h, a), b);
}

bool pm_rep_decode(const uint8_t* insn, uint32_t len, pm_rep_t* out) {
    bool rep = false, osz = false, asz = false, w = false;
    uint32_t i = 0;
    for (; i < len; i++) {
        uint8_t c = insn[i];
        if (c == 0xF2 || c == 0xF3) rep = true;
        else if (c == 0x66) osz = true;
        else if (c == 0x67) asz = true;
        else if (c == 0xF0 || c == 0x2E || c == 0x36 || c == 0x3E || c == 0x26 || c == 0x64 ||
                 c == 0x65) continue;
        else break;
    }
    if (i < len && (insn[i] & 0xF0) == 0x40) {         /* REX */
        w = (insn[i] & 8) != 0;
        i++;
    }
    if (!rep || i >= len) return false;
    uint8_t op = insn[i];
    pm_rep_op_t k;
    if (op == 0x6C || op == 0x6D) k = PM_REP_INS;
    else if (op == 0x6E || op == 0x6F) k = PM_REP_OUTS;
    else if (op == 0xA4 || op == 0xA5) k = PM_REP_MOVS;
    else if (op == 0xA6 || op == 0xA7) k = PM_REP_CMPS;
    else if (op == 0xAA || op == 0xAB) k = PM_REP_STOS;
    else if (op == 0xAC || op == 0xAD) k = PM_REP_LODS;
    else if (op == 0xAE || op == 0xAF) k = PM_REP_SCAS;
    else return false;
    out->op = k;
    out->size = (op & 1) == 0 ? 1 : w ? 8 : osz ? 2 : 4;
    out->len = i + 1;
    out->addr32 = asz;
    out->uses_si = k == PM_REP_MOVS || k == PM_REP_CMPS || k == PM_REP_LODS || k == PM_REP_OUTS;
    out->uses_di = k == PM_REP_MOVS || k == PM_REP_CMPS || k == PM_REP_STOS || k == PM_REP_SCAS ||
                   k == PM_REP_INS;
    return true;
}

bool pm_is_rep_string(const uint8_t* insn, uint32_t len) {
    pm_rep_t r;
    return pm_rep_decode(insn, len, &r);
}

static uint64_t absdiff(uint64_t a, uint64_t b) { return a > b ? a - b : b - a; }

/* ---- Recording ---- */

void pm_builder_init(pm_builder_t* b, void* (*mem_alloc)(uint64_t), void (*mem_free)(void*)) {
    b->runs = 0;
    b->hashes = 0;
    b->nruns = b->rcap = b->npages = b->hcap = 0;
    b->mem_alloc = mem_alloc;
    b->mem_free = mem_free;
}

void pm_builder_reset(pm_builder_t* b) {
    b->nruns = 0;
    b->npages = 0;
}

void pm_builder_free(pm_builder_t* b) {
    if (b->runs) b->mem_free(b->runs);
    if (b->hashes) b->mem_free(b->hashes);
    pm_builder_init(b, b->mem_alloc, b->mem_free);
}

static bool grow(pm_builder_t* b, void** arr, uint32_t* cap, uint32_t elem) {
    uint32_t ncap = *cap ? *cap * 2 : 64;
    uint8_t* n = (uint8_t*)b->mem_alloc((uint64_t)ncap * elem);
    if (!n) return false;
    const uint8_t* o = (const uint8_t*)*arr;
    for (uint64_t i = 0; i < (uint64_t)*cap * elem; i++) n[i] = o[i];
    if (*arr) b->mem_free(*arr);
    *arr = n;
    *cap = ncap;
    return true;
}

bool pm_builder_add(pm_builder_t* b, uint64_t va, const uint8_t* page) {
    if (b->npages == b->hcap && !grow(b, (void**)&b->hashes, &b->hcap, 8)) return false;
    pm_run_t* last = b->nruns ? &b->runs[b->nruns - 1] : 0;
    if (!last || last->va + last->pages * PM_PAGE != va) {
        if (b->nruns == b->rcap && !grow(b, (void**)&b->runs, &b->rcap, sizeof(pm_run_t))) return false;
        b->runs[b->nruns].va = va;
        b->runs[b->nruns].pages = 0;
        last = &b->runs[b->nruns++];
    }
    last->pages++;
    b->hashes[b->npages++] = pm_page_hash(va, page);
    return true;
}

uint32_t pm_encoded_size(const pm_builder_t* b) {
    return (uint32_t)(sizeof(pm_record_t) + b->nruns * sizeof(pm_run_t) + b->npages * 8ULL);
}

static void copy(uint8_t* d, const void* s, uint64_t n) {
    const uint8_t* p = (const uint8_t*)s;
    for (uint64_t i = 0; i < n; i++) d[i] = p[i];
}

uint32_t pm_encode(const pm_record_t* hdr, const pm_builder_t* b, uint8_t* out, uint32_t cap) {
    uint32_t need = pm_encoded_size(b);
    if (cap < need) return 0;
    pm_record_t h = *hdr;
    h.version = PM_VERSION;
    h.nruns = b->nruns;
    h.npages = b->npages;
    copy(out, &h, sizeof(h));
    copy(out + sizeof(h), b->runs, b->nruns * sizeof(pm_run_t));
    copy(out + sizeof(h) + b->nruns * sizeof(pm_run_t), b->hashes, b->npages * 8ULL);
    return need;
}

bool pm_decode(const uint8_t* buf, uint32_t len, pm_view_t* v) {
    if (len < sizeof(pm_record_t) || ((uint64_t)buf & 7)) return false;
    copy((uint8_t*)&v->hdr, buf, sizeof(pm_record_t));
    if (v->hdr.version != PM_VERSION) return false;
    uint64_t need = sizeof(pm_record_t) + (uint64_t)v->hdr.nruns * sizeof(pm_run_t) + v->hdr.npages * 8ULL;
    if (need != len) return false;
    v->runs = (const pm_run_t*)(buf + sizeof(pm_record_t));
    v->hashes = (const uint64_t*)(buf + sizeof(pm_record_t) + v->hdr.nruns * sizeof(pm_run_t));
    uint64_t pages = 0;
    for (uint32_t i = 0; i < v->hdr.nruns; i++) pages += v->runs[i].pages;
    return pages == v->hdr.npages;
}

uint64_t pm_digest(const uint8_t* buf, uint32_t len) {
    uint64_t h = mix(0xD16E57ULL, len);
    uint32_t i = 0;
    for (; i + 8 <= len; i += 8) {
        uint64_t w = 0;
        for (uint32_t k = 0; k < 8; k++) w |= (uint64_t)buf[i + k] << (8 * k);
        h = mix(h, w);
    }
    for (; i < len; i++) h = mix(h, buf[i]);
    return h;
}

/* ---- Matching ---- */

static uint64_t page_va(const pm_view_t* v, uint32_t index) {
    for (uint32_t r = 0; r < v->hdr.nruns; r++) {
        if (index < v->runs[r].pages) return v->runs[r].va + (uint64_t)index * PM_PAGE;
        index -= (uint32_t)v->runs[r].pages;
    }
    return 0;
}

static bool page_matches(const pm_view_t* v, uint32_t index, const pm_mem_t* mem, uint8_t* scratch) {
    uint64_t va = page_va(v, index);
    bool present = mem->read_page(mem->ctx, va, scratch);
    return pm_page_hash(va, present ? scratch : 0) == v->hashes[index];
}

static bool regs_equal(const pm_record_t* r, const pm_state_t* s, uint64_t ignore) {
    for (uint32_t i = 0; i < PM_NREGS; i++) {
        if (ignore & (1ULL << i)) continue;
        uint64_t a = r->regs[i], b = s->regs[i];
        if (i == PM_RFLAGS) {
            a &= PM_RFLAGS_MASK;
            b &= PM_RFLAGS_MASK;
        }
        if (a != b) return false;
    }
    return true;
}

pm_verdict_t pm_check(const pm_view_t* v, const pm_state_t* s, const pm_mem_t* mem,
                      uint8_t* scratch, uint32_t* hint) {
    const pm_record_t* r = &v->hdr;
    if (s->regs[PM_RIP] != r->regs[PM_RIP]) return PM_NO;
    if (!regs_equal(r, s, 0)) {
        /* An earlier iteration of the recorded string instruction: the
         * pointers and count (and, for cmps/scas, the flags) differ. */
        uint64_t moving = (1ULL << PM_RCX) | (1ULL << PM_RSI) | (1ULL << PM_RDI) | (1ULL << PM_RFLAGS);
        uint8_t insn[15];
        pm_rep_t rep;
        if (!s->insn || !regs_equal(r, s, moving) || !pm_rep_decode(insn, s->insn(s->ctx, insn), &rep)) {
            return PM_NO;
        }
        uint32_t size = rep.size;
        bool si = rep.uses_si, di = rep.uses_di;
        /* Only a point the remaining iterations can reach: rcx counts down,
         * and each pointer moves by the element size per iteration. */
        uint64_t rc = s->regs[PM_RCX], want = r->regs[PM_RCX];
        if (rc < want) return PM_NO;
        uint64_t left = (rc - want) * size;
        if (si ? absdiff(s->regs[PM_RSI], r->regs[PM_RSI]) != left : s->regs[PM_RSI] != r->regs[PM_RSI]) {
            return PM_NO;
        }
        if (di ? absdiff(s->regs[PM_RDI], r->regs[PM_RDI]) != left : s->regs[PM_RDI] != r->regs[PM_RDI]) {
            return PM_NO;
        }
        return PM_STEP;
    }
    if (s->fs_base != r->fs_base || s->gs_base != r->gs_base) return PM_NO;
    if (s->fpu_crc && s->fpu_crc(s->ctx) != r->fpu_crc) return PM_NO;
    uint32_t first = hint && *hint < r->npages ? *hint : 0;
    if (r->npages && !page_matches(v, first, mem, scratch)) return PM_NO;
    for (uint32_t i = 0; i < r->npages; i++) {
        if (i == first) continue;
        if (!page_matches(v, i, mem, scratch)) {
            if (hint) *hint = i;
            return PM_NO;
        }
    }
    return PM_MATCH;
}
