/* A deliberate nondeterminism leak (#241): RDRAND cannot be trapped, so a
 * program that executes it anyway (the CPUID table hides it) records values
 * the replay cannot reproduce. Verification must report the divergence and
 * name the RDRAND instruction. Needs a CPU model with RDRAND (QEMU -cpu max). */

#include "ulib.h"

static uint64_t g_cells[64];

static inline uint64_t raw_rdrand(void) {
    uint64_t v;
    uint8_t ok;
    __asm__ volatile("rdrand %0; setc %1" : "=r"(v), "=qm"(ok));
    return ok ? v : 0;
}

int main(void) {
    for (uint64_t i = 0; i < 2000; i++) {
        g_cells[i & 63] += i;
        if ((i & 15) == 0) sys_getpid();          /* kernel entries */
        if (i == 1500) g_cells[7] ^= raw_rdrand(); /* the leak, late in the run */
    }
    u_puts("rdrandleak: done\n");
    return (int)(g_cells[7] & 1);
}
