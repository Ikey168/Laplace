/* Laplace core: what the CPU's performance counters offer (#236).
 * See include/core/pmu.h. */

#include "core/pmu.h"
#include "core/cpu.h"
#include "core/console.h"

static pmu_info_t g_pmu;
static char       g_hypervisor[13];

const pmu_info_t* pmu_info(void) { return &g_pmu; }
const char* pmu_hypervisor(void) { return g_hypervisor; }
bool pmu_tcg(void) { return g_hypervisor[0] == 'T' && g_hypervisor[1] == 'C' && g_hypervisor[2] == 'G'; }

/* CPUID.1:ECX.31 and the vendor of leaf 0x40000000 ("KVMKVMKVM",
 * "TCGTCGTCGTCG", ...); empty on bare metal. */
static void find_hypervisor(void) {
    uint32_t r[4];
    cpu_cpuid(1, 0, r);
    if (!(r[2] & (1u << 31))) return;
    cpu_cpuid(0x40000000, 0, r);
    *(uint32_t*)&g_hypervisor[0] = r[1];
    *(uint32_t*)&g_hypervisor[4] = r[2];
    *(uint32_t*)&g_hypervisor[8] = r[3];
    g_hypervisor[12] = 0;
    for (int i = 0; i < 12; i++) {
        if (g_hypervisor[i] < 0x20 || g_hypervisor[i] > 0x7E) g_hypervisor[i] = 0;
    }
    if (!g_hypervisor[0]) {
        const char* u = "unknown";
        for (int i = 0; u[i]; i++) g_hypervisor[i] = u[i];
    }
}

/* Retired branches over a loop of exactly `n` taken conditional branches (and
 * one not taken), counted by an already-programmed counter `ctr`, kernel
 * mode. */
static uint64_t count_loop(uint32_t ctr, uint32_t n) {
    uint64_t before, after;
    if (!cpu_rdmsr_safe(ctr, &before)) return ~0ULL;
    __asm__ volatile("1: dec %0\n\tjnz 1b" : "+r"(n) : : "cc");
    if (!cpu_rdmsr_safe(ctr, &after)) return ~0ULL;
    return after - before;
}

/* Program `sel` with `event`, run the loop twice, and decide whether the
 * counter is exact: both runs agree and count the loop's branches. */
static bool probe(uint32_t sel, uint32_t ctr, uint64_t event, uint64_t* got) {
    if (!cpu_wrmsr_safe(sel, 0) || !cpu_wrmsr_safe(ctr, 0) || !cpu_wrmsr_safe(sel, event)) return false;
    uint64_t a = count_loop(ctr, 10000), b = count_loop(ctr, 10000);
    cpu_wrmsr_safe(sel, 0);
    *got = a;
    /* The loop's 10000 branches, plus the few of the rdmsr helper around it. */
    return a == b && a >= 10000 && a < 10000 + 64;
}

void pmu_init(void) {
    find_hypervisor();
    kprintf("boot: hypervisor: %s\n", g_hypervisor[0] ? g_hypervisor : "none (bare metal)");
    uint32_t r[4];
    cpu_cpuid(0, 0, r);
    char vendor[13];
    *(uint32_t*)&vendor[0] = r[1];
    *(uint32_t*)&vendor[4] = r[3];
    *(uint32_t*)&vendor[8] = r[2];
    vendor[12] = 0;
    uint32_t max = r[0];
    bool intel = vendor[0] == 'G' && vendor[7] == 'I';           /* GenuineIntel */
    bool amd = vendor[0] == 'A' || vendor[0] == 'H';             /* AuthenticAMD, HygonGenuine */
    const uint64_t USR = 1ULL << 16, OS = 1ULL << 17, EN = 1ULL << 22;
    uint64_t got = 0;
    if (intel && max >= 0xA) {
        cpu_cpuid(0xA, 0, r);
        g_pmu.version = r[0] & 0xFF;
        g_pmu.counters = (r[0] >> 8) & 0xFF;
        g_pmu.width = (r[0] >> 16) & 0xFF;
        bool branches = ((r[0] >> 24) & 0xFF) > 5 && !(r[1] & (1u << 5));   /* EBX bit 5: not available */
        if (g_pmu.version && g_pmu.counters && branches) {
            g_pmu.present = true;
            /* BR_INST_RETIRED.CONDITIONAL (0xC4/0x01), else all branches. */
            if (probe(0x186, 0xC1, 0xC4 | (0x01 << 8) | USR | OS | EN, &got)) {
                g_pmu.exact_branches = true;
                g_pmu.conditional_only = true;
            } else if (probe(0x186, 0xC1, 0xC4 | USR | OS | EN, &got)) {
                g_pmu.exact_branches = true;
            }
        }
    } else if (amd) {
        cpu_cpuid(0x80000000, 0, r);
        bool ext = false;
        if (r[0] >= 0x80000001) {
            cpu_cpuid(0x80000001, 0, r);
            ext = (r[2] >> 23) & 1;                              /* PerfCtrExtCore */
        }
        uint32_t sel = ext ? 0xC0010200 : 0xC0010000, ctr = ext ? 0xC0010201 : 0xC0010004;
        g_pmu.counters = ext ? 6 : 4;
        g_pmu.width = 48;
        uint64_t probe_val;
        if (cpu_rdmsr_safe(sel, &probe_val)) {
            /* Retired conditional branch instructions (0xD1). An emulator
             * may accept the registers and count nothing. */
            bool exact = probe(sel, ctr, 0xD1 | USR | OS | EN, &got);
            g_pmu.present = got && got != ~0ULL;
            g_pmu.version = ext ? 2 : 1;
            g_pmu.exact_branches = exact;
            g_pmu.conditional_only = exact;
        }
    }
    if (!g_pmu.present) {
        kprintf("boot: PMU: none on this %s CPU (no architectural performance counters; QEMU TCG and "
                "KVM without a vPMU look like this)\n", vendor);
    } else {
        kprintf("boot: PMU: %s, version %u, %u counters x %u bits; retired %s %s (%lu for 10000)\n",
                vendor, g_pmu.version, g_pmu.counters, g_pmu.width,
                g_pmu.conditional_only ? "conditional branches" : "branches",
                g_pmu.exact_branches ? "count exactly" : "do not count exactly", got);
    }
    if (!g_pmu.exact_branches) {
        kprintf("boot: preemption points are found by state alone (#245): no branch counter to skip "
                "breakpoint hits with\n");
    }
}
