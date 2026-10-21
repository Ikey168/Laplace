/* Memory probe (#239): malloc and free, anonymous mmap, munmap, mprotect,
 * mremap, brk, and a stack that grows on demand. Every round ends in a
 * checksum kept in static memory, so a replay that maps, protects, or fills a
 * page differently ends in a different state. The peak footprint stays well
 * inside one keyframe slot. */

#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#define ROUNDS 6
#define SLOTS  96

static uint32_t g_seed = 12345;
static uint32_t g_sum[ROUNDS];

static uint32_t rnd(void) {
    g_seed = g_seed * 1103515245u + 12345u;
    return g_seed >> 8;
}

static uint32_t mix(uint32_t h, const void* p, size_t n) {
    const uint8_t* b = (const uint8_t*)p;
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 16777619u;
    return h;
}

/* Burn time between operations, so keyframes fall between a mapping's
 * creation, use, and removal. */
static volatile uint32_t g_spin;
static void spin(void) {
    for (uint32_t i = 0; i < 400000; i++) g_spin += i;
}

/* Recursion that uses about `depth` KiB of stack. */
static uint32_t deep(int depth) {
    volatile uint8_t frame[1024];
    for (int i = 0; i < 1024; i += 64) frame[i] = (uint8_t)(depth + i);
    if (depth == 0) return frame[0];
    return deep(depth - 1) + frame[64];
}

static int heap_round(uint32_t* h) {
    static void* slot[SLOTS];
    static size_t len[SLOTS];
    for (int i = 0; i < 400; i++) {
        int k = (int)(rnd() % SLOTS);
        if (slot[k]) {
            *h = mix(*h, slot[k], len[k]);
            free(slot[k]);
            slot[k] = 0;
        } else {
            len[k] = 16 + rnd() % 1500;
            slot[k] = malloc(len[k]);
            if (!slot[k]) return -1;
            memset(slot[k], (int)(k + i), len[k]);
        }
        if (i % 50 == 7 && slot[k]) {   /* realloc grows a block in place or moves it */
            size_t n = len[k] + 200;
            void* q = realloc(slot[k], n);
            if (!q) return -1;
            memset((uint8_t*)q + len[k], 0x5A, n - len[k]);
            slot[k] = q;
            len[k] = n;
        }
    }
    for (int k = 0; k < SLOTS; k++) {
        if (slot[k]) { *h = mix(*h, slot[k], len[k]); free(slot[k]); slot[k] = 0; }
    }
    return 0;
}

static int mmap_round(uint32_t* h) {
    long pg = sysconf(_SC_PAGESIZE);
    /* A 64 KiB anonymous mapping: untouched pages read as zeros. */
    uint8_t* m = mmap(0, 16 * pg, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (m == MAP_FAILED) return -1;
    for (long i = 0; i < 16 * pg; i += pg) if (m[i + 17]) return -2;
    for (long i = 0; i < 8 * pg; i += 97) m[i] = (uint8_t)(i * 31 + rnd());
    *h = mix(*h, m, 8 * pg);
    spin();

    /* Read-only, then no access, then writable again: contents survive. */
    if (mprotect(m, 8 * pg, PROT_READ) != 0) return -3;
    *h = mix(*h, m + 3 * pg, 64);
    spin();
    if (mprotect(m + 4 * pg, 4 * pg, PROT_NONE) != 0) return -4;
    spin();
    if (mprotect(m, 8 * pg, PROT_READ | PROT_WRITE) != 0) return -5;
    m[5 * pg] ^= 0xFF;
    spin();
    *h = mix(*h, m, 8 * pg);

    /* Grow it: in place if the gap allows, else moved with its contents. */
    uint8_t want = m[5 * pg];
    uint8_t* r = mremap(m, 16 * pg, 32 * pg, MREMAP_MAYMOVE);
    if (r == MAP_FAILED) return -6;
    if (r[5 * pg] != want) return -7;
    r[31 * pg] = 0x77;
    *h = mix(*h, r, 8 * pg);
    *h = mix(*h, r + 31 * pg, 16);
    spin();

    /* Drop a hole in the middle, then the rest. */
    if (munmap(r + 8 * pg, 8 * pg) != 0) return -8;
    spin();
    if (madvise(r, pg, MADV_DONTNEED) != 0 || r[100] != 0) return -9;
    if (munmap(r, 32 * pg) != 0) return -10;

    /* A fixed mapping at a chosen address, replacing nothing. */
    uint8_t* f = mmap((void*)0x200000000000ULL, 2 * pg, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (f != (uint8_t*)0x200000000000ULL) return -11;
    f[pg] = 9;
    spin();
    *h = mix(*h, f + pg, 1);
    munmap(f, 2 * pg);
    return 0;
}

static int brk_round(uint32_t* h) {
    uintptr_t b0 = ((uintptr_t)syscall(SYS_brk, 0) + 4095) & ~(uintptr_t)4095;
    uintptr_t b1 = (uintptr_t)syscall(SYS_brk, b0 + 3 * 4096 + 100);
    if (b1 != b0 + 3 * 4096 + 100) return -1;
    uint8_t* p = (uint8_t*)b0;
    for (int i = 0; i < 3 * 4096; i += 511) p[i] = (uint8_t)i;
    *h = mix(*h, p, 3 * 4096);
    spin();
    if ((uintptr_t)syscall(SYS_brk, b0) != b0) return -2;
    /* Shrunk and grown again: the pages come back zeroed. */
    syscall(SYS_brk, b0 + 4096);
    if (p[511] != 0) return -3;
    syscall(SYS_brk, b0);
    return 0;
}

/* Unbounded recursion: the stack's 8 MiB fill and the next page is in no
 * region, so the machine stops there with a fault. */
static uint32_t overflow(uint32_t n) {
    volatile uint8_t frame[1024];
    frame[0] = (uint8_t)n;
    return overflow(n + 1) + frame[0];
}

int main(int argc, char** argv) {
    if (argc > 1 && strcmp(argv[1], "overflow") == 0) {
        printf("vmprobe: recursing without bound\n");
        return (int)overflow(0);
    }
    uint32_t total = 0;
    for (int r = 0; r < ROUNDS; r++) {
        uint32_t h = 2166136261u;
        int e;
        if ((e = heap_round(&h)) != 0) { printf("vmprobe: heap round %d failed (%d)\n", r, e); return 1; }
        if ((e = mmap_round(&h)) != 0) { printf("vmprobe: mmap round %d failed (%d)\n", r, e); return 1; }
        if ((e = brk_round(&h)) != 0) { printf("vmprobe: brk round %d failed (%d)\n", r, e); return 1; }
        h += deep(96 + 16 * r);   /* 96..176 KiB of stack */
        g_sum[r] = h;
        total = total * 31 + h;
    }
    printf("vmprobe: %d rounds of malloc, mmap, mprotect, mremap, brk, and stack: sum %08x\n",
           ROUNDS, total);
    return 0;
}
