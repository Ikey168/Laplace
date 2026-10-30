/* A compute-bound microbenchmark for the overhead table (#255): no system
 * calls between the start and the result, so recording costs only its
 * keyframes and the preemption points they need. argv[1]: rounds. */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

static uint32_t sieve[1 << 16];

int main(int argc, char** argv) {
    int rounds = argc > 1 ? atoi(argv[1]) : 40;
    uint64_t h = 1469598103934665603ULL;
    uint32_t primes = 0;
    for (int r = 0; r < rounds; r++) {
        /* A sieve over 2^21 numbers in a bitmap, then a hash of the bitmap. */
        for (uint32_t i = 0; i < (1u << 16); i++) sieve[i] = 0;
        primes = 0;
        for (uint32_t n = 2; n < (1u << 21); n++) {
            if (sieve[n >> 5] & (1u << (n & 31))) continue;
            primes++;
            for (uint64_t m = (uint64_t)n * n; m < (1u << 21); m += n) sieve[m >> 5] |= 1u << (m & 31);
        }
        for (uint32_t i = 0; i < (1u << 16); i++) h = (h ^ sieve[i]) * 1099511628211ULL;
    }
    printf("benchcpu: %d rounds, %u primes below 2^21, hash %016llx\n", rounds, primes,
           (unsigned long long)h);
    return 0;
}
