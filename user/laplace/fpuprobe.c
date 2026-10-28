/* Extended-state probe (#237): keeps a double accumulator in an XMM register,
 * and with AVX a vector in YMM8, live across kernel entries, so two copies
 * preempted into each other corrupt each other's results unless the kernel
 * saves and restores x87/SSE/AVX state at every switch. Each copy checks its
 * results against a second computation done with integers only, and stops the
 * machine (code 0xF0) on a mismatch.
 *
 * Built without -mgeneral-regs-only, like every user program since #237. */

#include "ulib.h"

static inline void raw_cpuid(uint32_t leaf, uint32_t r[4]) {
    __asm__ volatile("cpuid" : "=a"(r[0]), "=b"(r[1]), "=c"(r[2]), "=d"(r[3]) : "a"(leaf), "c"(0));
}

static int have_avx(void) {
    uint32_t r[4];
    raw_cpuid(1, r);
    if (!((r[2] >> 27) & 1) || !((r[2] >> 28) & 1)) return 0;   /* OSXSAVE, AVX */
    uint32_t lo, hi;
    __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
    return (lo & 6) == 6;
}

/* sum_{i<n} (i % 7) * 0.5 accumulated in a double, entering the kernel every
 * 64 iterations with the accumulator still in a register. */
static double sse_sum(uint64_t n) {
    double acc = 0.0;
    for (uint64_t i = 0; i < n; i++) {
        acc += (double)(i % 7) * 0.5;
        if ((i & 63) == 0) {
            long r;
            /* No XMM clobbers: acc stays in its register across the entry. */
            __asm__ volatile("int $0x80" : "=a"(r) : "a"(SYS_GETPID) : "memory");
        }
    }
    return acc;
}

/* The same sum in fixed point (halves), integers only. */
static uint64_t int_sum_halves(uint64_t n) {
    uint64_t acc = 0;
    for (uint64_t i = 0; i < n; i++) acc += i % 7;
    return acc;
}

/* YMM8 += {1, 2, 3, 4} n times, entering the kernel inside the loop. */
static void avx_sum(uint64_t n, double out[4]) {
    static const double inc[4] = { 1.0, 2.0, 3.0, 4.0 };
    __asm__ volatile(
        "vxorpd %%ymm8, %%ymm8, %%ymm8\n\t"
        "vmovupd (%[inc]), %%ymm9\n\t"
        "1:\n\t"
        "vaddpd %%ymm9, %%ymm8, %%ymm8\n\t"
        "test $31, %[n]\n\t"
        "jnz 2f\n\t"
        "mov %[nr], %%eax\n\t"
        "int $0x80\n\t"
        "2:\n\t"
        "dec %[n]\n\t"
        "jnz 1b\n\t"
        "vmovupd %%ymm8, (%[out])\n\t"
        "vzeroupper\n\t"
        : [n] "+r"(n)
        : [inc] "r"(inc), [out] "r"(out), [nr] "i"(SYS_GETPID)
        : "rax", "xmm8", "xmm9", "memory", "cc");
}

/* The report is one line, written at once: the two copies preempt each
 * other at any instruction (#245), and must not interleave their words. */
static char g_line[160];
static unsigned long g_len;
static void l_puts(const char* t) { while (*t && g_len < sizeof(g_line)) g_line[g_len++] = *t++; }
static void l_u64(uint64_t v) {
    char d[20];
    int k = 0;
    do { d[k++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (k && g_len < sizeof(g_line)) g_line[g_len++] = d[--k];
}

int main(void) {
    const uint64_t n = 200000;
    double s = sse_sum(n);
    uint64_t want = int_sum_halves(n);
    l_puts("fpuprobe: pid ");
    l_u64((uint64_t)sys_getpid());
    l_puts(" sse sum*2 ");
    l_u64((uint64_t)(s * 2.0));
    u_assert((uint64_t)(s * 2.0) == want, 0xF0);
    if (have_avx()) {
        double v[4];
        avx_sum(50000, v);
        l_puts(" avx ");
        for (int i = 0; i < 4; i++) {
            l_u64((uint64_t)v[i]);
            l_puts(i < 3 ? "," : "");
            u_assert((uint64_t)v[i] == 50000ULL * (uint64_t)(i + 1), 0xF0);
        }
    } else {
        l_puts(" (no AVX)");
    }
    l_puts(" ok\n");
    sys_write(g_line, g_len);
    return 0;
}
