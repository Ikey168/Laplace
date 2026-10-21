/* Large-memory probe (#239, #244).
 *
 *   bigmem resident N R   fill N MiB, then R rounds that each rewrite a small
 *                         working set (64 pages) and burn some time, so the
 *                         keyframes of those rounds see N MiB resident but
 *                         only a few pages dirty
 *   bigmem churn N        malloc and free N MiB in varied sizes (16 bytes to
 *                         1 MiB), keeping about N/4 MiB live at a time
 *
 * Each mode ends with a checksum of what it wrote, so a replay that lost or
 * misplaced a page ends in a different state.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sched.h>

static uint32_t g_seed = 4242;
static uint32_t rnd(void) {
    g_seed = g_seed * 1103515245u + 12345u;
    return g_seed >> 8;
}

static volatile uint32_t g_spin;
static void spin(uint32_t n) {
    for (uint32_t i = 0; i < n; i++) g_spin += i;
}

static uint32_t g_sum;   /* kept in memory: part of the replayed state */

static int resident(uint32_t mib, uint32_t rounds) {
    size_t len = (size_t)mib << 20;
    uint8_t* m = malloc(len);
    if (!m) { printf("bigmem: no memory for %u MiB\n", mib); return 1; }
    for (size_t off = 0; off < len; off += 4096) {
        m[off] = (uint8_t)(off >> 12);
        m[off + 4095] = (uint8_t)(off >> 20);
    }
    printf("bigmem: %u MiB resident\n", mib);
    size_t pages = len / 4096;
    for (uint32_t r = 0; r < rounds; r++) {
        for (uint32_t k = 0; k < 64; k++) {
            size_t pg = (rnd() % 256) * (pages / 256);   /* a fixed set of 256 pages */
            m[pg * 4096 + (rnd() % 4096)] ^= (uint8_t)(r + k + 1);
        }
        spin(300000);
        sched_yield();   /* a kernel entry, where keyframes are taken */
    }
    uint32_t h = 2166136261u;
    for (size_t off = 0; off < len; off += 509) h = (h ^ m[off]) * 16777619u;
    g_sum = h;
    printf("bigmem: %u rounds over a 64-page working set: sum %08x\n", rounds, h);
    free(m);
    return 0;
}

#define SLOTS 512

static int churn(uint32_t mib) {
    static void* slot[SLOTS];
    static size_t len[SLOTS];
    uint64_t total = 0, goal = (uint64_t)mib << 20, live = 0, peak = 0;
    uint32_t h = 2166136261u, ops = 0;
    while (total < goal) {
        uint32_t k = rnd() % SLOTS;
        if (slot[k]) {
            uint8_t* p = slot[k];
            for (size_t i = 0; i < len[k]; i += 251) h = (h ^ p[i]) * 16777619u;
            free(slot[k]);
            slot[k] = 0;
            live -= len[k];
        } else if (live < goal / 4) {
            /* Sizes from 16 bytes to 1 MiB, log-uniform. */
            uint32_t shift = 4 + rnd() % 17;
            len[k] = ((size_t)1 << shift) + rnd() % ((size_t)1 << shift);
            if (len[k] > (1u << 20)) len[k] = 1u << 20;
            uint8_t* p = malloc(len[k]);
            if (!p) { printf("bigmem: malloc of %zu failed\n", len[k]); return 1; }
            for (size_t i = 0; i < len[k]; i += 512) p[i] = (uint8_t)(k + i);
            p[len[k] - 1] = (uint8_t)k;
            slot[k] = p;
            live += len[k];
            total += len[k];
            if (live > peak) peak = live;
        }
        ops++;
    }
    for (uint32_t k = 0; k < SLOTS; k++) {
        if (slot[k]) { free(slot[k]); slot[k] = 0; }
    }
    g_sum = h;
    printf("bigmem: churned %llu MiB in %u operations, peak %llu MiB live: sum %08x\n",
           (unsigned long long)(total >> 20), ops, (unsigned long long)(peak >> 20), h);
    return 0;
}

int main(int argc, char** argv) {
    if (argc >= 3 && strcmp(argv[1], "resident") == 0) {
        return resident((uint32_t)atoi(argv[2]), argc >= 4 ? (uint32_t)atoi(argv[3]) : 40);
    }
    if (argc >= 3 && strcmp(argv[1], "churn") == 0) return churn((uint32_t)atoi(argv[2]));
    printf("usage: bigmem resident MiB [rounds] | bigmem churn MiB\n");
    return 2;
}
