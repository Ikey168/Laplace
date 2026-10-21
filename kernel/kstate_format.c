/* Laplace: the kernel-state format of a keyframe (#243). See include/kstate_format.h. */

#include "kstate_format.h"
#include "snapshot_store.h"   /* snapshot_crc32 */

typedef struct {
    uint64_t magic;
    uint32_t version;
    uint32_t nsections;
    uint32_t total;
    uint32_t table_crc;
} ksf_header_t;

typedef struct {
    uint32_t id;
    uint32_t version;
    uint32_t offset;
    uint32_t size;
    uint32_t crc;
} ksf_entry_t;

static void copy(void* dst, const void* src, uint32_t n) {
    uint8_t* d = (uint8_t*)dst;
    const uint8_t* s = (const uint8_t*)src;
    for (uint32_t i = 0; i < n; i++) d[i] = s[i];
}

/* ---- Writer / reader ---- */

void ksf_put(ksf_writer_t* w, const void* data, uint32_t len) {
    if (w->buf && w->len + len <= w->cap) copy(w->buf + w->len, data, len);
    w->len += len;
}

void ksf_put_u32(ksf_writer_t* w, uint32_t v) { ksf_put(w, &v, sizeof(v)); }
void ksf_put_u64(ksf_writer_t* w, uint64_t v) { ksf_put(w, &v, sizeof(v)); }

bool ksf_get(ksf_reader_t* r, void* out, uint32_t len) {
    if (!r->ok || len > r->len - r->pos) {
        r->ok = false;
        return false;
    }
    copy(out, r->buf + r->pos, len);
    r->pos += len;
    return true;
}

uint32_t ksf_get_u32(ksf_reader_t* r) {
    uint32_t v = 0;
    ksf_get(r, &v, sizeof(v));
    return v;
}

uint64_t ksf_get_u64(ksf_reader_t* r) {
    uint64_t v = 0;
    ksf_get(r, &v, sizeof(v));
    return v;
}

/* ---- Registry ---- */

void ksf_registry_init(ksf_registry_t* reg) {
    reg->count = 0;
}

const ksf_subsys_t* ksf_find(const ksf_registry_t* reg, uint32_t id) {
    for (uint32_t i = 0; i < reg->count; i++) {
        if (reg->subs[i].id == id) return &reg->subs[i];
    }
    return 0;
}

int ksf_register(ksf_registry_t* reg, const ksf_subsys_t* sub) {
    if (!reg || !sub || !sub->save || !sub->restore || ksf_find(reg, sub->id)) return KSF_ERR_PARAM;
    if (reg->count >= KSF_MAX_SUBSYS) return KSF_ERR_FULL;
    reg->subs[reg->count++] = *sub;
    return KSF_OK;
}

/* ---- Encode ---- */

static uint32_t table_bytes(uint32_t n) {
    return (uint32_t)sizeof(ksf_header_t) + n * (uint32_t)sizeof(ksf_entry_t);
}

uint32_t ksf_measure(const ksf_registry_t* reg) {
    uint32_t total = table_bytes(reg->count);
    for (uint32_t i = 0; i < reg->count; i++) {
        ksf_writer_t w = { 0, 0, 0 };
        reg->subs[i].save(&w, reg->subs[i].ctx);
        total += w.len;
    }
    return total;
}

int64_t ksf_encode(const ksf_registry_t* reg, uint8_t* out, uint32_t cap) {
    if (!reg || !out) return KSF_ERR_PARAM;
    uint32_t n = reg->count;
    uint32_t off = table_bytes(n);
    if (off > cap) return KSF_ERR_FULL;
    ksf_entry_t* table = (ksf_entry_t*)(out + sizeof(ksf_header_t));
    for (uint32_t i = 0; i < n; i++) {
        ksf_writer_t w = { out + off, cap - off, 0 };
        reg->subs[i].save(&w, reg->subs[i].ctx);
        if (w.len > cap - off) return KSF_ERR_FULL;
        ksf_entry_t e = { reg->subs[i].id, reg->subs[i].version, off, w.len,
                          snapshot_crc32(0, out + off, w.len) };
        copy(&table[i], &e, sizeof(e));
        off += w.len;
    }
    ksf_header_t h = { KSF_MAGIC, KSF_VERSION, n, off,
                       snapshot_crc32(0, table, n * (uint32_t)sizeof(ksf_entry_t)) };
    copy(out, &h, sizeof(h));
    return off;
}

/* ---- Decode ---- */

static int read_header(const uint8_t* blob, uint32_t len, ksf_header_t* h) {
    if (!blob || len < sizeof(*h)) return KSF_ERR_CORRUPT;
    copy(h, blob, sizeof(*h));
    if (h->magic != KSF_MAGIC || h->version != KSF_VERSION || h->total != len ||
        h->nsections > (len - sizeof(*h)) / sizeof(ksf_entry_t)) {
        return KSF_ERR_CORRUPT;
    }
    return KSF_OK;
}

static void entry_at(const uint8_t* blob, uint32_t i, ksf_entry_t* e) {
    copy(e, blob + sizeof(ksf_header_t) + i * sizeof(ksf_entry_t), sizeof(*e));
}

int ksf_validate(const uint8_t* blob, uint32_t len) {
    ksf_header_t h;
    int rc = read_header(blob, len, &h);
    if (rc != KSF_OK) return rc;
    if (snapshot_crc32(0, blob + sizeof(h), h.nsections * (uint32_t)sizeof(ksf_entry_t)) != h.table_crc) {
        return KSF_ERR_CORRUPT;
    }
    uint32_t data_start = table_bytes(h.nsections);
    for (uint32_t i = 0; i < h.nsections; i++) {
        ksf_entry_t e;
        entry_at(blob, i, &e);
        if (e.offset < data_start || e.offset > len || e.size > len - e.offset) return KSF_ERR_CORRUPT;
        if (snapshot_crc32(0, blob + e.offset, e.size) != e.crc) return KSF_ERR_CORRUPT;
        for (uint32_t j = 0; j < i; j++) {
            ksf_entry_t o;
            entry_at(blob, j, &o);
            if (o.id == e.id) return KSF_ERR_CORRUPT;
        }
    }
    return KSF_OK;
}

bool ksf_section(const uint8_t* blob, uint32_t len, uint32_t id, ksf_reader_t* out,
                 uint32_t* version) {
    ksf_header_t h;
    if (read_header(blob, len, &h) != KSF_OK) return false;
    for (uint32_t i = 0; i < h.nsections; i++) {
        ksf_entry_t e;
        entry_at(blob, i, &e);
        if (e.id != id || e.offset > len || e.size > len - e.offset) continue;
        out->buf = blob + e.offset;
        out->len = e.size;
        out->pos = 0;
        out->ok = true;
        if (version) *version = e.version;
        return true;
    }
    return false;
}

int ksf_restore(const ksf_registry_t* reg, const uint8_t* blob, uint32_t len,
                uint32_t* failed_id) {
    if (!reg) return KSF_ERR_PARAM;
    int rc = ksf_validate(blob, len);
    if (rc != KSF_OK) return rc;
    ksf_header_t h;
    read_header(blob, len, &h);
    for (uint32_t i = 0; i < h.nsections; i++) {
        ksf_entry_t e;
        entry_at(blob, i, &e);
        if (!ksf_find(reg, e.id)) {
            if (failed_id) *failed_id = e.id;
            return KSF_ERR_UNKNOWN;
        }
    }
    for (uint32_t i = 0; i < reg->count; i++) {
        const ksf_subsys_t* s = &reg->subs[i];
        ksf_reader_t r;
        uint32_t version = 0;
        if (!ksf_section(blob, len, s->id, &r, &version)) {
            if (failed_id) *failed_id = s->id;
            return KSF_ERR_MISSING;
        }
        if (s->restore(&r, version, s->ctx) != 0 || !r.ok || r.pos != r.len) {
            if (failed_id) *failed_id = s->id;
            return KSF_ERR_RESTORE;
        }
    }
    return KSF_OK;
}
