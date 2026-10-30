/* IEEE 802.3 reflected CRC32 (poly 0xEDB88320), seed 0, slicing by 8.
 *
 * Header-only so each core that checksums (snapshot store, journal, keyframe
 * ring, divergence) stays dependency-free and host-testable on its own; each
 * gets its own 8 KiB table, built on first use. The result is the same as
 * the bit-at-a-time loop these cores used before, which spent about 40
 * instructions a byte on every keyframe page (#255). */

#ifndef CRC32_IEEE_H
#define CRC32_IEEE_H

#include <stdint.h>

static uint32_t crc32_ieee_table[8][256];
static int      crc32_ieee_ready;

static void crc32_ieee_init(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int b = 0; b < 8; b++) c = (c >> 1) ^ (0xEDB88320u & -(c & 1u));
        crc32_ieee_table[0][i] = c;
    }
    for (uint32_t i = 0; i < 256; i++) {
        for (int t = 1; t < 8; t++) {
            uint32_t c = crc32_ieee_table[t - 1][i];
            crc32_ieee_table[t][i] = (c >> 8) ^ crc32_ieee_table[0][c & 0xFF];
        }
    }
    crc32_ieee_ready = 1;
}

static inline uint32_t crc32_ieee(uint32_t crc, const void* data, uint32_t len) {
    if (!crc32_ieee_ready) crc32_ieee_init();
    const uint8_t* p = (const uint8_t*)data;
    uint32_t (*T)[256] = crc32_ieee_table;
    crc = ~crc;
    while (len && ((uintptr_t)p & 7)) {
        crc = (crc >> 8) ^ T[0][(crc ^ *p++) & 0xFF];
        len--;
    }
    for (; len >= 8; len -= 8, p += 8) {
        uint32_t lo = crc ^ ((uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24);
        uint32_t hi = (uint32_t)p[4] | (uint32_t)p[5] << 8 | (uint32_t)p[6] << 16 | (uint32_t)p[7] << 24;
        crc = T[7][lo & 0xFF] ^ T[6][(lo >> 8) & 0xFF] ^ T[5][(lo >> 16) & 0xFF] ^ T[4][lo >> 24] ^
              T[3][hi & 0xFF] ^ T[2][(hi >> 8) & 0xFF] ^ T[1][(hi >> 16) & 0xFF] ^ T[0][hi >> 24];
    }
    while (len--) crc = (crc >> 8) ^ T[0][(crc ^ *p++) & 0xFF];
    return ~crc;
}

#endif /* CRC32_IEEE_H */
