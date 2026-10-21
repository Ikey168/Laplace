/* Laplace core: x87, SSE, and AVX state of user processes (#237).
 *
 * The kernel builds with -mgeneral-regs-only and never touches the x87, XMM,
 * or YMM registers, so between kernel entries the register file always holds
 * the state of the process that ran last (the FPU owner). Loading a different
 * process saves the owner's state and restores the new one's; nothing traps,
 * so recording and replay see the same kernel entries.
 *
 * Saved areas are canonical: after XSAVE, any component the processor left
 * in its initial state (XSTATE_BV bit clear) is written out as that initial
 * state, so equal program-visible state means equal bytes. With XSAVE the
 * kernel enables x87, SSE, and (when present) AVX in XCR0; without it, FXSAVE.
 */

#ifndef CORE_FPU_H
#define CORE_FPU_H

#include <stdint.h>
#include <stdbool.h>

#define FPU_AREA_MAX   1024     /* x87 + SSE + AVX with XSAVE: 832 bytes */
#define FPU_XMM_OFFSET 160      /* XMM0 in the legacy (FXSAVE) region */

#define XCR0_X87 (1ULL << 0)
#define XCR0_SSE (1ULL << 1)
#define XCR0_AVX (1ULL << 2)

/* CR0/CR4/XCR0 setup; call once, before building the CPUID table. */
void     fpu_init(void);
bool     fpu_has_xsave(void);
uint64_t fpu_xcr0(void);           /* 0 without XSAVE */
uint32_t fpu_area_size(void);      /* bytes of a saved area */

/* Areas are FPU_AREA_MAX bytes, 64-byte aligned. */
void     fpu_area_init(uint8_t* area);         /* FNINIT state, MXCSR 0x1F80 */
void     fpu_save(uint8_t* area);              /* registers -> area (canonical) */
void     fpu_restore(const uint8_t* area);     /* area -> registers */
/* Make an area loaded from a keyframe safe to restore on this CPU. */
void     fpu_sanitize(uint8_t* area);
/* Fold the program-visible part of an area into a running checksum. */
uint32_t fpu_checksum(uint32_t crc, const uint8_t* area);

#endif /* CORE_FPU_H */
