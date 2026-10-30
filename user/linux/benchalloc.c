/* An allocation-heavy microbenchmark for the overhead table (#255): malloc
 * and free of mixed sizes, small ones from the heap and large ones from mmap,
 * each written as it is allocated. argv[1]: operations. */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define SLOTS 4096

static void* slot[SLOTS];
static size_t size[SLOTS];

int main(int argc, char** argv) {
    long ops = argc > 1 ? atol(argv[1]) : 200000;
    uint64_t x = 88172645463325252ULL, sum = 0, peak = 0, live = 0;
    for (long i = 0; i < ops; i++) {
        x ^= x << 13; x ^= x >> 7; x ^= x << 17;
        uint32_t k = (uint32_t)(x % SLOTS);
        if (slot[k]) {
            sum += ((unsigned char*)slot[k])[size[k] - 1];
            live -= size[k];
            free(slot[k]);
            slot[k] = 0;
        } else {
            /* Mostly small; one in 64 between 64 KiB and 320 KiB (mmap). */
            size_t n = (x >> 32) % 64 == 0 ? 65536 + (x >> 40) % 262144 : 16 + (x >> 40) % 2048;
            slot[k] = malloc(n);
            if (!slot[k]) return 1;
            memset(slot[k], (int)(i & 0xFF), n);
            size[k] = n;
            live += n;
            if (live > peak) peak = live;
        }
    }
    printf("benchalloc: %ld operations, peak %llu KiB live, check %llu\n", ops,
           (unsigned long long)(peak >> 10), (unsigned long long)sum);
    return 0;
}
