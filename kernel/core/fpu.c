/* Laplace core: x87, SSE, and AVX state of user processes (#237).
 * See include/core/fpu.h. */

#include "core/fpu.h"
#include "core/cpu.h"
#include "divergence.h"   /* divergence_checksum */
#include <stddef.h>

void* memset(void* dst, int c, size_t n);

/* Legacy (FXSAVE) region. */
#define OFF_FCW        0
#define OFF_FSW        2
#define OFF_FTW        4
#define OFF_FOP        6
#define OFF_MXCSR      24
#define OFF_MXCSR_MASK 28
#define OFF_ST         32
#define OFF_XMM        FPU_XMM_OFFSET
#define LEGACY_END     416     /* after XMM15; 416..511 reserved / software */
/* XSAVE header and the AVX component (CPUID 0xD.2: 256 bytes at 576). */
#define OFF_XSTATE_BV  512
#define OFF_XCOMP_BV   520
#define OFF_HDR_RSVD   528
#define HDR_END        576
#define OFF_YMM_HI     576
#define YMM_HI_SIZE    256

#define CR0_MP (1ULL << 1)
#define CR0_EM (1ULL << 2)
#define CR0_TS (1ULL << 3)
#define CR0_NE (1ULL << 5)
#define CR4_OSFXSR     (1ULL << 9)
#define CR4_OSXMMEXCPT (1ULL << 10)
#define CR4_OSXSAVE    (1ULL << 18)

static bool     g_xsave;
static uint64_t g_xcr0;
static uint32_t g_size = 512;
static uint32_t g_mxcsr_mask = 0xFFBF;

static inline uint64_t read_cr0(void) {
    uint64_t v; __asm__ volatile("mov %%cr0, %0" : "=r"(v)); return v;
}
static inline void write_cr0(uint64_t v) {
    __asm__ volatile("mov %0, %%cr0" : : "r"(v) : "memory");
}
static inline void xsetbv0(uint64_t v) {
    __asm__ volatile("xsetbv" : : "c"(0), "a"((uint32_t)v), "d"((uint32_t)(v >> 32)));
}

static inline uint16_t rd16(const uint8_t* a, uint32_t off) {
    return (uint16_t)(a[off] | (a[off + 1] << 8));
}
static inline void wr16(uint8_t* a, uint32_t off, uint16_t v) {
    a[off] = (uint8_t)v;
    a[off + 1] = (uint8_t)(v >> 8);
}
static inline uint32_t rd32(const uint8_t* a, uint32_t off) {
    return a[off] | (a[off + 1] << 8) | (a[off + 2] << 16) | ((uint32_t)a[off + 3] << 24);
}
static inline void wr32(uint8_t* a, uint32_t off, uint32_t v) {
    for (int i = 0; i < 4; i++) a[off + i] = (uint8_t)(v >> (8 * i));
}
static inline uint64_t rd64(const uint8_t* a, uint32_t off) {
    return rd32(a, off) | ((uint64_t)rd32(a, off + 4) << 32);
}
static inline void wr64(uint8_t* a, uint32_t off, uint64_t v) {
    wr32(a, off, (uint32_t)v);
    wr32(a, off + 4, (uint32_t)(v >> 32));
}

void fpu_init(void) {
    uint64_t cr0 = read_cr0();
    cr0 &= ~(CR0_EM | CR0_TS);
    cr0 |= CR0_MP | CR0_NE;
    write_cr0(cr0);
    write_cr4(read_cr4() | CR4_OSFXSR | CR4_OSXMMEXCPT);

    uint32_t r[4];
    cpu_cpuid(1, 0, r);
    g_xsave = (r[2] >> 26) & 1;
    bool avx = (r[2] >> 28) & 1;
    if (g_xsave) {
        write_cr4(read_cr4() | CR4_OSXSAVE);
        uint32_t d[4];
        cpu_cpuid(0xD, 0, d);
        uint64_t supported = d[0] | ((uint64_t)d[3] << 32);
        g_xcr0 = XCR0_X87 | XCR0_SSE | ((avx && (supported & XCR0_AVX)) ? XCR0_AVX : 0);
        xsetbv0(g_xcr0);
        cpu_cpuid(0xD, 0, d);
        g_size = d[1];
        if (g_size > FPU_AREA_MAX) {
            g_xcr0 = XCR0_X87 | XCR0_SSE;
            xsetbv0(g_xcr0);
            cpu_cpuid(0xD, 0, d);
            g_size = d[1];
        }
    }
    __asm__ volatile("fninit");
    uint32_t mxcsr = 0x1F80;
    __asm__ volatile("ldmxcsr %0" : : "m"(mxcsr));

    /* The MXCSR bits this CPU accepts (an unset mask means the default). */
    static uint8_t probe[FPU_AREA_MAX] __attribute__((aligned(64)));
    memset(probe, 0, sizeof(probe));
    __asm__ volatile("fxsave64 %0" : "=m"(*(uint8_t(*)[512])probe));
    uint32_t mask = rd32(probe, OFF_MXCSR_MASK);
    g_mxcsr_mask = mask ? mask : 0xFFBF;
}

bool fpu_has_xsave(void) { return g_xsave; }
uint64_t fpu_xcr0(void) { return g_xcr0; }
uint32_t fpu_area_size(void) { return g_size; }

void fpu_area_init(uint8_t* a) {
    memset(a, 0, FPU_AREA_MAX);
    wr16(a, OFF_FCW, 0x037F);
    wr32(a, OFF_MXCSR, 0x1F80);
    wr32(a, OFF_MXCSR_MASK, g_mxcsr_mask);
    if (g_xsave) wr64(a, OFF_XSTATE_BV, g_xcr0);
}

/* Write components the processor reported as initial (XSTATE_BV clear) as
 * their initial values, and clear what XSAVE does not write. */
static void canonicalize(uint8_t* a) {
    if (g_xsave) {
        uint64_t bv = rd64(a, OFF_XSTATE_BV);
        if (!(bv & XCR0_X87)) {
            wr16(a, OFF_FCW, 0x037F);
            wr16(a, OFF_FSW, 0);
            a[OFF_FTW] = 0;
            memset(a + OFF_FOP, 0, OFF_MXCSR - OFF_FOP);
            memset(a + OFF_ST, 0, OFF_XMM - OFF_ST);
        }
        if (!(bv & XCR0_SSE)) memset(a + OFF_XMM, 0, LEGACY_END - OFF_XMM);
        if ((g_xcr0 & XCR0_AVX) && !(bv & XCR0_AVX)) memset(a + OFF_YMM_HI, 0, YMM_HI_SIZE);
        wr64(a, OFF_XSTATE_BV, g_xcr0);
        wr64(a, OFF_XCOMP_BV, 0);
        memset(a + OFF_HDR_RSVD, 0, HDR_END - OFF_HDR_RSVD);
    }
    a[OFF_FTW + 1] = 0;
    memset(a + LEGACY_END, 0, 512 - LEGACY_END);
    wr32(a, OFF_MXCSR_MASK, g_mxcsr_mask);
}

void fpu_save(uint8_t* a) {
    if (g_xsave) {
        __asm__ volatile("xsave64 %0" : "+m"(*(uint8_t(*)[FPU_AREA_MAX])a)
                         : "a"((uint32_t)g_xcr0), "d"((uint32_t)(g_xcr0 >> 32)));
    } else {
        __asm__ volatile("fxsave64 %0" : "=m"(*(uint8_t(*)[512])a));
    }
    canonicalize(a);
}

void fpu_restore(const uint8_t* a) {
    if (g_xsave) {
        __asm__ volatile("xrstor64 %0" : : "m"(*(const uint8_t(*)[FPU_AREA_MAX])a),
                         "a"((uint32_t)g_xcr0), "d"((uint32_t)(g_xcr0 >> 32)));
    } else {
        __asm__ volatile("fxrstor64 %0" : : "m"(*(const uint8_t(*)[512])a));
    }
}

void fpu_sanitize(uint8_t* a) {
    /* A keyframe from before #237 carries no state (zeros here). */
    if (rd16(a, OFF_FCW) == 0 && rd32(a, OFF_MXCSR) == 0 && a[OFF_FTW] == 0) {
        fpu_area_init(a);
        return;
    }
    wr32(a, OFF_MXCSR, rd32(a, OFF_MXCSR) & g_mxcsr_mask);
    if (g_xsave) {
        /* Components this CPU does not enable are dropped; then canonical. */
        wr64(a, OFF_XSTATE_BV, rd64(a, OFF_XSTATE_BV) & g_xcr0);
        canonicalize(a);
    } else {
        memset(a + 512, 0, FPU_AREA_MAX - 512);
        canonicalize(a);
    }
}

uint32_t fpu_checksum(uint32_t crc, const uint8_t* a) {
    /* Control/status/tag words and MXCSR, the eight x87 registers (80 bits
     * each), XMM0-15, and the upper YMM halves. The last-instruction pointers
     * (FOP/FIP/FDP) are left out: CPUs differ in when they store them. */
    crc = divergence_checksum(crc, a + OFF_FCW, 5);
    crc = divergence_checksum(crc, a + OFF_MXCSR, 4);
    for (uint32_t i = 0; i < 8; i++) crc = divergence_checksum(crc, a + OFF_ST + 16 * i, 10);
    crc = divergence_checksum(crc, a + OFF_XMM, LEGACY_END - OFF_XMM);
    if (g_xcr0 & XCR0_AVX) crc = divergence_checksum(crc, a + OFF_YMM_HI, YMM_HI_SIZE);
    return crc;
}
