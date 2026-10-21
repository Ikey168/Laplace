/* Laplace core: process memory (#239). See include/core/vm.h. */

#include "core/vm.h"
#include "core/mm.h"
#include "core/proc.h"
#include "core/linux.h"
#include "core/timetravel.h"
#include "core/console.h"

void* memcpy(void* dst, const void* src, size_t n);

#define MAP_SHARED          0x01
#define MAP_PRIVATE         0x02
#define MAP_FIXED           0x10
#define MAP_ANONYMOUS       0x20
#define MAP_FIXED_NOREPLACE 0x100000

#define MREMAP_MAYMOVE      1
#define MREMAP_FIXED        2

#define MADV_DONTNEED       4

static inline int64_t err(int e) { return -(int64_t)e; }

uint32_t vm_pte_flags(const vm_region_t* r) {
    if (!(r->flags & (VMM_FLAG_READ | VMM_FLAG_WRITE | VMM_FLAG_EXEC))) return 0;  /* PROT_NONE */
    return PAGE_USER | ((r->flags & VMM_FLAG_WRITE) ? PAGE_WRITABLE : 0);
}

static uint32_t prot_to_flags(uint64_t prot) {
    return VMM_FLAG_USER | ((prot & LX_PROT_READ) ? VMM_FLAG_READ : 0) |
           ((prot & LX_PROT_WRITE) ? VMM_FLAG_WRITE : 0) | ((prot & LX_PROT_EXEC) ? VMM_FLAG_EXEC : 0);
}

bool vm_zero_backed(vm_space_t* s, uint64_t va) {
    vm_region_t* r = vmm_find_region(s, va);
    return r && (vm_pte_flags(r) & PAGE_USER);
}

bool vm_fault_in(vm_space_t* s, uint64_t va, bool write) {
    vm_region_t* r = vmm_find_region(s, va);
    if (!r) return false;
    uint32_t f = vm_pte_flags(r);
    if (!(f & PAGE_USER) || (write && !(f & PAGE_WRITABLE))) return false;
    uint64_t page = va & ~0xFFFULL;
    if (vmm_get_physical_addr(s, page)) return true;
    tt_before_mapping_change();
    uint64_t phys = pmm_alloc();
    if (!phys) return false;
    return vmm_map_page(s, page, phys, f) == 0;
}

/* ---- Region list surgery ---- */

static void unlink_region(vm_space_t* s, vm_region_t* r) {
    if (r->prev) r->prev->next = r->next; else s->regions = r->next;
    if (r->next) r->next->prev = r->prev;
    s->region_count--;
    kfree(r);
}

/* Split the region containing `addr` (strictly inside it) in two. */
static bool split_at(vm_space_t* s, uint64_t addr) {
    vm_region_t* r = vmm_find_region(s, addr);
    if (!r || r->start_addr == addr) return true;
    vm_region_t* hi = vmm_create_region(s, addr, r->end_addr - addr, r->flags, r->type, r->name);
    if (!hi) return false;
    r->end_addr = addr;
    return true;
}

/* Join neighbours that differ only in where they start, as Linux merges
 * VMAs: a range mprotect split and then made uniform again is one region
 * again, which mremap needs. */
static void merge_regions(vm_space_t* s) {
    vm_region_t* r = s->regions;
    while (r && r->next) {
        vm_region_t* n = r->next;
        bool same_name = true;
        for (int i = 0; i < (int)sizeof(r->name) && (r->name[i] || n->name[i]); i++) {
            if (r->name[i] != n->name[i]) { same_name = false; break; }
        }
        if (r->end_addr == n->start_addr && r->flags == n->flags && r->type == n->type && same_name) {
            r->end_addr = n->end_addr;
            unlink_region(s, n);
        } else {
            r = n;
        }
    }
}

static void unmap_pages(vm_space_t* s, uint64_t lo, uint64_t hi) {
    for (uint64_t a = lo; a < hi; a += PAGE_SIZE) {
        if (vmm_get_physical_addr(s, a)) vmm_unmap_page(s, a);
    }
}

/* Remove [lo, hi) from the space: pages and regions. */
static bool remove_range(vm_space_t* s, uint64_t lo, uint64_t hi) {
    if (!split_at(s, lo) || !split_at(s, hi)) return false;
    vm_region_t* r = s->regions;
    while (r) {
        vm_region_t* n = r->next;
        if (r->start_addr >= lo && r->end_addr <= hi) {
            unmap_pages(s, r->start_addr, r->end_addr);
            unlink_region(s, r);
        }
        r = n;
    }
    return true;
}

static bool range_free(vm_space_t* s, uint64_t lo, uint64_t hi) {
    for (vm_region_t* r = s->regions; r; r = r->next) {
        if (r->start_addr < hi && r->end_addr > lo) return false;
    }
    return true;
}

/* The highest free range of `len` bytes below LP_MMAP_TOP. */
static uint64_t find_gap(vm_space_t* s, uint64_t len) {
    vm_region_t* last = s->regions;
    while (last && last->next) last = last->next;
    uint64_t hi = LP_MMAP_TOP;
    for (vm_region_t* r = last; r; r = r->prev) {
        if (r->start_addr >= hi) continue;
        if (r->end_addr <= hi && hi - r->end_addr >= len) return hi - len;
        hi = r->start_addr;
    }
    return hi - LP_MMAP_BOTTOM >= len ? hi - len : 0;
}

/* ---- brk ---- */

void vm_set_brk(process_t* p, uint64_t start) {
    proc_arch(p)->brk_start = start;
    proc_arch(p)->brk_cur = start;
}

int64_t vm_brk(process_t* p, uint64_t addr) {
    lp_arch_t* a = proc_arch(p);
    vm_space_t* s = p->address_space;
    if (!a->brk_start || addr < a->brk_start || addr > a->brk_start + (1ULL << 40)) {
        return (int64_t)a->brk_cur;
    }
    uint64_t old_end = PAGE_ALIGN(a->brk_cur), new_end = PAGE_ALIGN(addr);
    if (new_end > old_end) {
        if (!range_free(s, old_end, new_end)) return (int64_t)a->brk_cur;
        tt_before_mapping_change();
        vm_region_t* heap = old_end > a->brk_start ? vmm_find_region(s, old_end - 1) : 0;
        if (heap && heap->type == VMM_REGION_HEAP) {
            heap->end_addr = new_end;
        } else if (!vmm_create_region(s, old_end, new_end - old_end,
                                      VMM_FLAG_READ | VMM_FLAG_WRITE | VMM_FLAG_USER,
                                      VMM_REGION_HEAP, "heap")) {
            return (int64_t)a->brk_cur;
        }
    } else if (new_end < old_end) {
        tt_before_mapping_change();
        remove_range(s, new_end, old_end);
    }
    a->brk_cur = addr;
    return (int64_t)addr;
}

/* ---- mmap family ---- */

int64_t vm_mmap(process_t* p, uint64_t addr, uint64_t len, uint64_t prot, uint64_t flags,
                int64_t fd, uint64_t off) {
    vm_space_t* s = p->address_space;
    (void)off;
    if (len == 0 || len > LP_USER_END) return err(LX_EINVAL);
    if (!(flags & MAP_ANONYMOUS)) return fd < 0 ? err(LX_EBADF) : err(LX_ENODEV);
    if (!(flags & (MAP_SHARED | MAP_PRIVATE))) return err(LX_EINVAL);
    len = PAGE_ALIGN(len);
    bool fixed = (flags & (MAP_FIXED | MAP_FIXED_NOREPLACE)) != 0;
    if (fixed && (addr & 0xFFF)) return err(LX_EINVAL);

    uint64_t at = 0;
    if (fixed) {
        if (addr < LP_USER_BASE || addr + len > LP_USER_END) return err(LX_ENOMEM);
        if (!range_free(s, addr, addr + len)) {
            if (flags & MAP_FIXED_NOREPLACE) return err(LX_EEXIST);
            tt_before_mapping_change();
            remove_range(s, addr, addr + len);
        }
        at = addr;
    } else {
        addr &= ~0xFFFULL;
        if (addr >= LP_USER_BASE && addr + len <= LP_USER_END && range_free(s, addr, addr + len)) {
            at = addr;
        } else {
            at = find_gap(s, len);
            if (!at) return err(LX_ENOMEM);
        }
    }
    tt_before_mapping_change();
    if (!vmm_create_region(s, at, len, prot_to_flags(prot), VMM_REGION_MMAP, "mmap")) {
        return err(LX_ENOMEM);
    }
    return (int64_t)at;
}

int64_t vm_munmap(process_t* p, uint64_t addr, uint64_t len) {
    if ((addr & 0xFFF) || len == 0) return err(LX_EINVAL);
    tt_before_mapping_change();
    return remove_range(p->address_space, addr, addr + PAGE_ALIGN(len)) ? 0 : err(LX_ENOMEM);
}

int64_t vm_mprotect(process_t* p, uint64_t addr, uint64_t len, uint64_t prot) {
    vm_space_t* s = p->address_space;
    if (addr & 0xFFF) return err(LX_EINVAL);
    uint64_t lo = addr, hi = addr + PAGE_ALIGN(len);
    /* Every page of the range must be in some region. */
    for (uint64_t a = lo; a < hi;) {
        vm_region_t* r = vmm_find_region(s, a);
        if (!r) return err(LX_ENOMEM);
        a = r->end_addr;
    }
    tt_before_mapping_change();
    if (!split_at(s, lo) || !split_at(s, hi)) return err(LX_ENOMEM);
    for (vm_region_t* r = s->regions; r; r = r->next) {
        if (r->start_addr < lo || r->end_addr > hi) continue;
        r->flags = prot_to_flags(prot);
        uint32_t f = vm_pte_flags(r);
        for (uint64_t a = r->start_addr; a < r->end_addr; a += PAGE_SIZE) {
            pte_t* pte = vmm_get_page_table(s, a, PT_LEVEL, false);
            if (!pte || !(*pte & PAGE_PRESENT)) continue;
            pte_t e = *pte & ~(pte_t)(PAGE_USER | PAGE_WRITABLE);
            if (!(f & PAGE_WRITABLE)) {
                /* Nothing can write it now: its content is the keyframe's. */
                e &= ~(pte_t)PAGE_SNAPSHOT_COW;
            } else if (!(e & PAGE_SNAPSHOT_COW)) {
                e |= PAGE_WRITABLE;   /* a snapshot-COW page becomes writable on its fault */
            }
            *pte = e | (f & PAGE_USER);
            vmm_flush_tlb_page(a);
        }
    }
    merge_regions(s);
    return 0;
}

int64_t vm_mremap(process_t* p, uint64_t old, uint64_t old_len, uint64_t new_len,
                  uint64_t flags, uint64_t new_addr) {
    vm_space_t* s = p->address_space;
    if ((old & 0xFFF) || new_len == 0 || (flags & MREMAP_FIXED)) return err(LX_EINVAL);
    old_len = PAGE_ALIGN(old_len);
    new_len = PAGE_ALIGN(new_len);
    (void)new_addr;
    vm_region_t* r = vmm_find_region(s, old);
    if (!r || old + old_len > r->end_addr) return err(LX_EFAULT);
    if (new_len <= old_len) {
        if (new_len < old_len) vm_munmap(p, old + new_len, old_len - new_len);
        return (int64_t)old;
    }
    tt_before_mapping_change();
    if (old + old_len == r->end_addr && range_free(s, r->end_addr, old + new_len)) {
        r->end_addr = old + new_len;   /* grow in place */
        return (int64_t)old;
    }
    if (!(flags & MREMAP_MAYMOVE)) return err(LX_ENOMEM);
    uint64_t at = find_gap(s, new_len);
    if (!at) return err(LX_ENOMEM);
    if (!split_at(s, old) || !split_at(s, old + old_len)) return err(LX_ENOMEM);
    r = vmm_find_region(s, old);
    if (!vmm_create_region(s, at, new_len, r->flags, r->type, r->name)) return err(LX_ENOMEM);
    /* Move the pages: the frames keep their contents, only the PTEs move. */
    for (uint64_t off = 0; off < old_len; off += PAGE_SIZE) {
        pte_t* from = vmm_get_page_table(s, old + off, PT_LEVEL, false);
        if (!from || !(*from & PAGE_PRESENT)) continue;
        pte_t* to = vmm_get_page_table(s, at + off, PT_LEVEL, true);
        if (!to) return err(LX_ENOMEM);
        *to = *from;
        *from = 0;
        vmm_flush_tlb_page(old + off);
    }
    unlink_region(s, r);
    return (int64_t)at;
}

int64_t vm_madvise(process_t* p, uint64_t addr, uint64_t len, uint64_t advice) {
    if (addr & 0xFFF) return err(LX_EINVAL);
    if (advice == MADV_DONTNEED) {
        /* The range reads as zeros afterwards: drop its pages. */
        tt_before_mapping_change();
        unmap_pages(p->address_space, addr, addr + PAGE_ALIGN(len));
    }
    return 0;   /* other advice changes nothing a program can observe */
}
