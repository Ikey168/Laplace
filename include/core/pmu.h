/* Laplace core: what the CPU's performance counters offer (#236).
 *
 * At boot: Intel's architectural performance monitoring (CPUID leaf 0xA), or
 * AMD's core performance counters (CPUID 0x80000001 ECX.PerfCtrExtCore, else
 * the four legacy ones), then a functional probe: a counter programmed for
 * retired (conditional) branches must count a known loop exactly, twice
 * alike. The boot log says what was found. QEMU's TCG and KVM without a
 * virtual PMU (GitHub-hosted runners) have none; preemption points (#245) do
 * not need one.
 */

#ifndef CORE_PMU_H
#define CORE_PMU_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    bool     present;            /* performance counters answer */
    uint32_t version, counters, width;
    bool     exact_branches;     /* a retired-branch count is exact and repeatable */
    bool     conditional_only;   /* ... of conditional branches only (what rr counts) */
} pmu_info_t;

void pmu_init(void);
const pmu_info_t* pmu_info(void);

/* The hypervisor's CPUID vendor ("" on bare metal), and whether it is QEMU's
 * TCG emulator, which applies RF to a whole translated block (#255). */
const char* pmu_hypervisor(void);
bool pmu_tcg(void);

#endif /* CORE_PMU_H */
