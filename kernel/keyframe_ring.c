/* IKOS Orthogonal Persistence - Keyframe Retention Ring (#168, epic #159)
 *
 * See include/keyframe_ring.h and docs/architecture/time-travel.md.
 *
 * Pure ring-index core: no allocator, no hardware, no I/O.
 */

#include "crc32_ieee.h"
#include "keyframe_ring.h"

/* IEEE 802.3 reflected CRC32 (poly 0xEDB88320), via crc32_ieee.h so this
 * core stays dependency-free, like the journal and divergence cores. */
uint32_t keyframe_ring_crc32(uint32_t crc, const void* data, uint32_t len) {
    return crc32_ieee(crc, data, len);
}

int keyframe_ring_init(keyframe_ring_t* r, uint32_t capacity) {
    if (!r || capacity == 0 || capacity > KEYFRAME_RING_MAX)
        return KEYFRAME_RING_ERR_PARAM;
    r->capacity = capacity;
    r->count = 0;
    r->head = 0;
    r->reserved = 0;
    r->reclaimed_valid = 0;
    r->reserved2 = 0;
    r->reclaimed_epoch = 0;
    for (uint32_t i = 0; i < KEYFRAME_RING_MAX; i++) {
        r->slots[i].epoch = 0;
        r->slots[i].valid = 0;
        r->slots[i].reserved = 0;
    }
    return KEYFRAME_RING_OK;
}

uint32_t keyframe_ring_advance(keyframe_ring_t* r, uint64_t epoch) {
    if (!r || r->capacity == 0) return 0;
    uint32_t slot = r->head;

    /* Record whether we are evicting a valid keyframe from this slot. */
    r->reclaimed_valid = r->slots[slot].valid;
    r->reclaimed_epoch = r->slots[slot].valid ? r->slots[slot].epoch : 0;

    r->slots[slot].epoch = epoch;
    r->slots[slot].valid = 1;

    r->head = (r->head + 1) % r->capacity;
    if (r->count < r->capacity) r->count++;
    return slot;
}

bool keyframe_ring_find(const keyframe_ring_t* r, uint64_t target,
                        uint32_t* slot_out, uint64_t* epoch_out) {
    if (!r) return false;
    bool found = false;
    uint64_t best = 0;
    uint32_t best_slot = 0;
    for (uint32_t i = 0; i < r->capacity; i++) {
        if (!r->slots[i].valid) continue;
        uint64_t e = r->slots[i].epoch;
        if (e <= target && (!found || e > best)) {
            best = e;
            best_slot = i;
            found = true;
        }
    }
    if (found) {
        if (slot_out) *slot_out = best_slot;
        if (epoch_out) *epoch_out = best;
    }
    return found;
}

bool keyframe_ring_oldest(const keyframe_ring_t* r, uint64_t* epoch_out) {
    if (!r || r->count == 0) return false;
    bool found = false;
    uint64_t best = 0;
    for (uint32_t i = 0; i < r->capacity; i++) {
        if (!r->slots[i].valid) continue;
        if (!found || r->slots[i].epoch < best) { best = r->slots[i].epoch; found = true; }
    }
    if (found && epoch_out) *epoch_out = best;
    return found;
}

bool keyframe_ring_newest(const keyframe_ring_t* r, uint64_t* epoch_out) {
    if (!r || r->count == 0) return false;
    bool found = false;
    uint64_t best = 0;
    for (uint32_t i = 0; i < r->capacity; i++) {
        if (!r->slots[i].valid) continue;
        if (!found || r->slots[i].epoch > best) { best = r->slots[i].epoch; found = true; }
    }
    if (found && epoch_out) *epoch_out = best;
    return found;
}

bool keyframe_ring_reclaimed(const keyframe_ring_t* r, uint64_t* epoch_out) {
    if (!r || !r->reclaimed_valid) return false;
    if (epoch_out) *epoch_out = r->reclaimed_epoch;
    return true;
}

uint32_t keyframe_ring_count(const keyframe_ring_t* r) {
    return r ? r->count : 0;
}

/* ---- Persistence: pack the index with a trailing CRC32 ---- */

uint32_t keyframe_ring_packed_size(void) {
    return (uint32_t)sizeof(keyframe_ring_t) + 4u; /* struct + crc */
}

static uint8_t* put_bytes(uint8_t* p, const void* src, uint32_t n) {
    const uint8_t* s = (const uint8_t*)src;
    for (uint32_t i = 0; i < n; i++) p[i] = s[i];
    return p + n;
}

int keyframe_ring_pack(const keyframe_ring_t* r, void* buf, uint32_t buflen) {
    if (!r || !buf) return KEYFRAME_RING_ERR_PARAM;
    uint32_t need = keyframe_ring_packed_size();
    if (buflen < need) return KEYFRAME_RING_ERR_SIZE;
    uint8_t* p = (uint8_t*)buf;
    p = put_bytes(p, r, sizeof(*r));
    uint32_t crc = keyframe_ring_crc32(0, r, sizeof(*r));
    put_bytes(p, &crc, sizeof(crc));
    return (int)need;
}

int keyframe_ring_unpack(keyframe_ring_t* r, const void* buf, uint32_t buflen) {
    if (!r || !buf) return KEYFRAME_RING_ERR_PARAM;
    uint32_t need = keyframe_ring_packed_size();
    if (buflen < need) return KEYFRAME_RING_ERR_SIZE;
    const uint8_t* p = (const uint8_t*)buf;
    keyframe_ring_t tmp;
    uint8_t* d = (uint8_t*)&tmp;
    for (uint32_t i = 0; i < sizeof(tmp); i++) d[i] = p[i];
    uint32_t stored;
    uint8_t* cp = (uint8_t*)&stored;
    for (uint32_t i = 0; i < sizeof(stored); i++) cp[i] = p[sizeof(tmp) + i];
    if (keyframe_ring_crc32(0, &tmp, sizeof(tmp)) != stored)
        return KEYFRAME_RING_ERR_CRC;
    if (tmp.capacity == 0 || tmp.capacity > KEYFRAME_RING_MAX)
        return KEYFRAME_RING_ERR_PARAM;
    *r = tmp;
    return KEYFRAME_RING_OK;
}
