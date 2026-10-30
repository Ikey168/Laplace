/* Laplace core: physical frames, kernel heap, and address spaces (#220, #238).
 *
 * Virtual layout (#238):
 *   [LP_USER_BASE, LP_USER_END)   user programs: the whole lower half, so
 *                                 Linux binaries load at their link address
 *   LP_DIRECT_MAP + phys          every physical frame, read and written by
 *                                 the kernel through P2V()
 *   LP_KERNEL_VMA + phys          the kernel image, BSS, and heap (first GiB)
 * Every address space shares the kernel half (PML4[256..511], supervisor
 * only) and owns its lower half.
 *
 * The vmm_* functions declared in include/vmm.h that the checkpoint engine
 * calls are implemented by kernel/core/mm.c.
 */

#ifndef CORE_MM_H
#define CORE_MM_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "vmm.h"

#define LP_DIRECT_MAP      0xFFFF800000000000ULL
#define LP_KERNEL_VMA      0xFFFFFFFF80000000ULL
#define LP_MAX_PHYS        (64ULL << 30)           /* RAM the frame allocator covers */
#define LP_USER_BASE       0x10000ULL              /* nothing below 64 KiB (NULL guard) */
#define LP_USER_END        0x00007FFFFFFFF000ULL
#define LP_PTE_ADDR_MASK   0x000FFFFFFFFFF000ULL

/* Physical frame <-> the kernel's view of it. */
static inline void* P2V(uint64_t phys) { return (void*)(phys + LP_DIRECT_MAP); }
/* Physical address of a kernel-image, BSS, heap, or direct-map address. */
static inline uint64_t V2P(const void* v) {
    uint64_t a = (uint64_t)v;
    return a >= LP_KERNEL_VMA ? a - LP_KERNEL_VMA : a - LP_DIRECT_MAP;
}

/* Bring up the frame allocator and heap from the Multiboot memory map, extend
 * the direct map to all RAM, and drop the boot identity map. `mbi` is the
 * Multiboot information structure (through the direct map). */
void     mm_init(const uint32_t* mbi);
uint64_t pmm_alloc(void);           /* zeroed frame, or 0 when out of memory */
void     pmm_free(uint64_t phys);
uint64_t pmm_free_frames(void);
uint64_t pmm_max_phys(void);        /* end of the highest usable RAM */

/* Device registers at `phys` (`len` bytes), mapped uncached; NULL if they
 * cannot be. */
void* mm_mmio(uint64_t phys, uint64_t len);

void* kmalloc(size_t size);
void  kfree(void* ptr);
void* kzalloc(size_t size);

/* The kernel's own address space (boot page tables), current when no user
 * process runs. */
vm_space_t* vmm_kernel_space(void);

/* Walk every present user page of `space` in ascending address order. */
typedef void (*vmm_page_fn)(void* ctx, uint64_t virt, uint64_t phys, pte_t pte);
void vmm_for_each_page(vm_space_t* space, vmm_page_fn fn, void* ctx);

/* Copy between the kernel and a user space by physical address (no CR3
 * switch). copy_to_user resolves snapshot-COW pages through the checkpoint
 * hook first, so a pending keyframe keeps the pre-write image. Both fail
 * (return false) if any byte is unmapped or outside user space; copy_to_user
 * also fails on a page that is read-only for the user. */
/* Frames shared between address spaces by fork (#248). A frame has one owner
 * unless shared: frame_share adds a reference, frame_put drops one and frees
 * the frame with the last. */
void frame_share(uint64_t phys);
bool frame_shared(uint64_t phys);
void frame_put(uint64_t phys);

/* A user write to a present page that is not writable: preserve the keyframe
 * image (snapshot COW, #111/#244) and break fork sharing (#248). True when the
 * write may now proceed. */
bool vmm_user_write_fault(vm_space_t* space, uint64_t va);

/* Every present user PTE of `space`, in ascending address order, by pointer
 * (the callback may change it; the caller flushes the TLB). */
typedef void (*vmm_pte_fn)(void* ctx, uint64_t va, pte_t* pte);
void vmm_for_each_pte(vm_space_t* space, vmm_pte_fn fn, void* ctx);

bool uaccess_read(vm_space_t* space, uint64_t uaddr, void* dst, uint64_t len);
bool uaccess_write(vm_space_t* space, uint64_t uaddr, const void* src, uint64_t len);

#endif /* CORE_MM_H */
