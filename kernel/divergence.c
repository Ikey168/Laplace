/* IKOS Orthogonal Persistence - Divergence Detector (#166, epic #159)
 *
 * See include/divergence.h and docs/architecture/time-travel.md.
 *
 * Pure decision core: no allocator, no hardware, no I/O. Components are
 * checksummed by the caller and fed in by id, so this is host-testable.
 */

#include "crc32_ieee.h"
#include "divergence.h"

/* IEEE 802.3 reflected CRC32 (poly 0xEDB88320), via crc32_ieee.h so this
 * core stays dependency-free, like the journal and disk cores. */
uint32_t divergence_checksum(uint32_t crc, const void* data, uint32_t len) {
    return crc32_ieee(crc, data, len);
}

static void clear_epoch(divergence_t* d) {
    for (uint32_t i = 0; i < DIVERGE_MAX_COMPONENTS; i++) {
        d->sums[i] = 0;
        d->present[i] = false;
    }
}

int divergence_init(divergence_t* d, diverge_mode_t mode) {
    if (!d) return DIVERGE_ERR_PARAM;
    d->mode = mode;
    d->epoch = 0;
    clear_epoch(d);
    d->diverged = false;
    d->diverged_epoch = 0;
    d->diverged_component = 0;
    d->diverged_expected = 0;
    d->diverged_actual = 0;
    d->checks = 0;
    d->mismatches = 0;
    return DIVERGE_OK;
}

void divergence_set_mode(divergence_t* d, diverge_mode_t mode) {
    if (!d) return;
    d->mode = mode;
}

void divergence_begin_epoch(divergence_t* d, uint64_t epoch) {
    if (!d) return;
    d->epoch = epoch;
    clear_epoch(d);
}

int divergence_record(divergence_t* d, uint32_t component, uint32_t checksum) {
    if (!d || component >= DIVERGE_MAX_COMPONENTS) return DIVERGE_ERR_PARAM;
    if (d->mode != DIVERGE_RECORD) return DIVERGE_OK;
    d->sums[component] = checksum;
    d->present[component] = true;
    return DIVERGE_OK;
}

uint32_t divergence_recorded(const divergence_t* d, uint32_t component, bool* present) {
    if (!d || component >= DIVERGE_MAX_COMPONENTS) {
        if (present) *present = false;
        return 0;
    }
    if (present) *present = d->present[component];
    return d->sums[component];
}

int divergence_expect(divergence_t* d, uint64_t epoch,
                      const uint32_t* sums, uint32_t n) {
    if (!d || (!sums && n > 0) || n > DIVERGE_MAX_COMPONENTS) return DIVERGE_ERR_PARAM;
    d->epoch = epoch;
    clear_epoch(d);
    for (uint32_t i = 0; i < n; i++) {
        d->sums[i] = sums[i];
        d->present[i] = true;
    }
    return DIVERGE_OK;
}

bool divergence_check(divergence_t* d, uint32_t component, uint32_t checksum) {
    if (!d || component >= DIVERGE_MAX_COMPONENTS) return false;
    if (d->mode != DIVERGE_REPLAY) return true;

    d->checks++;

    /* A component with no expected value is itself a divergence (it appeared on
     * replay but was not recorded). */
    bool ok = d->present[component] && d->sums[component] == checksum;
    if (!ok) {
        d->mismatches++;
        if (!d->diverged) {
            d->diverged = true;
            d->diverged_epoch = d->epoch;
            d->diverged_component = component;
            d->diverged_expected = d->present[component] ? d->sums[component] : 0;
            d->diverged_actual = checksum;
        }
        return false;
    }
    return true;
}

bool divergence_has_diverged(const divergence_t* d) {
    return d ? d->diverged : false;
}
