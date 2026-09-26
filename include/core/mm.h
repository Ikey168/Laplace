/* Laplace core: physical frames, kernel heap, and address spaces (#220).
 *
 * Physical memory below 1 GiB is identity-mapped by boot.asm, so every frame
 * the allocator hands out is directly addressable by the kernel. User address
 * spaces live at [LP_USER_BASE, LP_USER_END); each one shares the supervisor-
 * only kernel identity map (PDPT[0]) and owns the rest of its tables.
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

#define LP_IDENTITY_LIMIT  0x40000000ULL   /* 1 GiB identity map */
#define LP_USER_BASE       0x40000000ULL   /* user space starts at 1 GiB */
#define LP_USER_END        0x8000000000ULL /* one PML4 entry: 512 GiB */
#define LP_PTE_ADDR_MASK   0x000FFFFFFFFFF000ULL

/* Bring up the frame allocator and heap from the Multiboot memory map. */
void     mm_init(uint32_t mb_info);
uint64_t pmm_alloc(void);           /* zeroed frame, or 0 when out of memory */
void     pmm_free(uint64_t phys);
uint64_t pmm_free_frames(void);

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
bool uaccess_read(vm_space_t* space, uint64_t uaddr, void* dst, uint64_t len);
bool uaccess_write(vm_space_t* space, uint64_t uaddr, const void* src, uint64_t len);

#endif /* CORE_MM_H */
