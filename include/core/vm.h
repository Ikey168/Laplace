/* Laplace core: process memory (#239).
 *
 * A process's address space is a sorted list of regions (vm_region_t) and the
 * pages mapped in them. Pages appear on first touch: a fault inside a region
 * whose permissions allow the access maps a zeroed page and resumes the
 * instruction. Such faults are a deterministic function of the program and of
 * which pages are present (which keyframes record), so like snapshot-COW
 * captures they are not steps: a replay takes the same faults at the same
 * instructions without counting them.
 *
 * Layout (no randomization, so replays and reruns agree):
 *   ELF segments              where the binary says (static PIE: 0x555555554000)
 *   brk heap                  from the page after the highest segment, upward
 *   mmap                      top-down from LP_MMAP_TOP
 *   stack (Linux programs)    8 MiB below LP_LINUX_STACK_TOP, grown on demand
 *
 * Before any change to a space's regions or pages while recording, a pending
 * keyframe writeback is finished (tt_before_mapping_change): the keyframe must
 * be written from the regions and pages it was taken with.
 */

#ifndef CORE_VM_H
#define CORE_VM_H

#include <stdint.h>
#include <stdbool.h>
#include "process.h"
#include "vmm.h"

#define LP_MMAP_TOP          0x00007FFF00000000ULL
#define LP_MMAP_BOTTOM       0x0000100000000000ULL
#define LP_LINUX_STACK_TOP   0x00007FFFFFFF0000ULL
#define LP_LINUX_STACK_SIZE  (8ULL << 20)

#define LX_PROT_READ   1
#define LX_PROT_WRITE  2
#define LX_PROT_EXEC   4

/* PTE flags for a page of region `r` (no user bit for PROT_NONE). */
uint32_t vm_pte_flags(const vm_region_t* r);

/* A region of `s` covers `va` and lets the program read it: an absent page
 * there reads as zeros. */
bool vm_zero_backed(vm_space_t* s, uint64_t va);

/* Map a zeroed page at `va` if a region of `s` covers it and allows the
 * access. True when the page is (now) present. */
bool vm_fault_in(vm_space_t* s, uint64_t va, bool write);

/* The heap starts at `start` (page aligned). */
void vm_set_brk(process_t* p, uint64_t start);

int64_t vm_brk(process_t* p, uint64_t addr);
int64_t vm_mmap(process_t* p, uint64_t addr, uint64_t len, uint64_t prot, uint64_t flags,
                int64_t fd, uint64_t off);
int64_t vm_munmap(process_t* p, uint64_t addr, uint64_t len);
int64_t vm_mprotect(process_t* p, uint64_t addr, uint64_t len, uint64_t prot);
int64_t vm_mremap(process_t* p, uint64_t old, uint64_t old_len, uint64_t new_len,
                  uint64_t flags, uint64_t new_addr);
int64_t vm_madvise(process_t* p, uint64_t addr, uint64_t len, uint64_t advice);

#endif /* CORE_VM_H */
