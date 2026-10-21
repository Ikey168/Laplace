/* Determinism probe (#241): reads the time stamp counter and the CPU's
 * identity with raw RDTSC, RDTSCP, and CPUID instead of system calls, and
 * branches on what it reads. The kernel traps and journals all three, so the
 * recording replays byte-exact; before #241 the replay diverged. */

#include "ulib.h"

static inline uint64_t raw_rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static inline uint64_t raw_rdtscp(uint32_t* aux) {
    uint32_t lo, hi;
    __asm__ volatile("rdtscp" : "=a"(lo), "=d"(hi), "=c"(*aux));
    return ((uint64_t)hi << 32) | lo;
}

static inline void raw_cpuid(uint32_t leaf, uint32_t r[4]) {
    __asm__ volatile("cpuid" : "=a"(r[0]), "=b"(r[1]), "=c"(r[2]), "=d"(r[3]) : "a"(leaf), "c"(0));
}

static uint64_t g_mix;
static uint64_t g_odd;

int main(void) {
    uint32_t r[4];
    raw_cpuid(0, r);
    u_puts("tscprobe: cpuid max leaf ");
    u_put_u64(r[0]);
    raw_cpuid(1, r);
    u_puts(", rdrand advertised ");
    u_put_u64((r[2] >> 30) & 1);
    raw_cpuid(0x80000001, r);
    int has_rdtscp = (r[3] >> 27) & 1;
    u_puts(", rdtscp ");
    u_put_u64((uint64_t)has_rdtscp);
    u_puts("\n");
    for (uint64_t i = 0; i < 3000; i++) {
        uint32_t aux;
        uint64_t t = (i & 1) || !has_rdtscp ? raw_rdtsc() : raw_rdtscp(&aux);
        /* Branch on the counter, so a replay that saw other values would take
         * other paths and leave other state. */
        if (t & 1) g_odd++;
        g_mix = (g_mix ^ t) * 0x9E3779B97F4A7C15ULL;
        if ((i & 63) == 0) {
            raw_cpuid((uint32_t)(i & 7), r);
            g_mix ^= r[0] ^ r[1] ^ r[2] ^ r[3];
        }
    }
    u_puts("tscprobe: odd reads ");
    u_put_u64(g_odd);
    u_puts(", mix ");
    u_put_hex(g_mix);
    u_puts("\n");
    return 0;
}
