/* Laplace core: the kernel state a keyframe holds (#243).
 *
 * Every kernel subsystem with state a program can observe registers a section
 * in the keyframe's kernel-state blob (include/kstate_format.h): how to save
 * it, how to rebuild it, and how to checksum it. Built in, in restore order:
 *
 *   procs     the process table: pid, state, name, executable, and each
 *             address space's regions and break (registers and pages are
 *             keyframe records of their own)
 *   cpuid     what user CPUID answered while recording (#241)
 *   clock     how TSC reads became times, and the time at the keyframe (#238)
 *   machine   the epoch, the current process, the next pid, the pending entry
 *   probe     a one-word test subsystem: the self-test perturbs its restore
 *             to show that verify names the subsystem that diverged
 *
 * A subsystem with a checksum callback becomes its own divergence component,
 * numbered from KSTATE_COMPONENT_BASE in registration order, so a divergence
 * report names it.
 */

#ifndef CORE_KSTATE_H
#define CORE_KSTATE_H

#include <stdint.h>
#include <stdbool.h>
#include "kstate_format.h"

/* Section ids: stable, never reused. */
#define KS_ID_PROCS    1
#define KS_ID_CPUID    2
#define KS_ID_CLOCK    3
#define KS_ID_MACHINE  4
#define KS_ID_PROBE    5

/* The first divergence component id for subsystems (after KDIVERGE_*). */
#define KSTATE_COMPONENT_BASE 6

/* Register the built-in subsystems. Once, at boot. */
void kstate_init(void);

/* Register another subsystem (files, futexes, ...). Before the first keyframe. */
int  kstate_register(const ksf_subsys_t* sub);

/* Add every subsystem's checksum to the divergence sources (called by
 * kdiverge_register_kernel_sources). */
void kstate_register_checksums(void);

/* The blob for the current state, kmalloc'd; NULL on failure. */
uint8_t* kstate_encode(uint64_t epoch, uint32_t* size);

/* The epoch a blob was taken at, without restoring anything. */
bool kstate_blob_epoch(const uint8_t* blob, uint32_t size, uint64_t* epoch);

/* Rebuild every subsystem from a blob. The process table is replaced. Returns
 * a KSF_* code; on failure *failed names the subsystem's id. */
int  kstate_restore(const uint8_t* blob, uint32_t size, uint32_t* failed);

/* Monotonic time at the last restored keyframe (for a resume, #238). */
uint64_t kstate_restored_now_ns(void);

/* A divergence component's name ("process table", "user pages", "probe"). */
const char* kstate_component_name(uint32_t component);
/* A section id's subsystem name, or "?". */
const char* kstate_subsys_name(uint32_t id);

/* Test hook: the next restore of subsystem `id` changes its state by one bit.
 * A subsystem's restore asks with kstate_take_perturb (true once). */
void kstate_perturb_next_restore(uint32_t id);
bool kstate_take_perturb(uint32_t id);

#endif /* CORE_KSTATE_H */
