/* Laplace core: physical frames, kernel heap, and address spaces (#220).
 * See include/core/mm.h. */

#include "core/mm.h"
#include "core/cpu.h"
#include "core/console.h"
#include "checkpoint.h"   /* checkpoint_handle_write_fault (copy_to_user) */

extern uint8_t _kernel_end[];
extern uint64_t boot_pml4[512];
extern uint64_t boot_pd[512];

void* memset(void* dst, int c, size_t n);
void* memcpy(void* dst, const void* src, size_t n);

/* ---- Physical frames: a bitmap over the 1 GiB identity map ---- */

#define FRAMES (LP_IDENTITY_LIMIT / PAGE_SIZE)
static uint8_t  g_frame_used[FRAMES / 8];
static uint64_t g_free_frames;
static uint64_t g_hint;

static inline bool frame_used(uint64_t f) { return g_frame_used[f / 8] & (1u << (f % 8)); }
static inline void frame_set(uint64_t f, bool used) {
    if (used) g_frame_used[f / 8] |= (uint8_t)(1u << (f % 8));
    else      g_frame_used[f / 8] &= (uint8_t)~(1u << (f % 8));
}

static void mark_range(uint64_t start, uint64_t end, bool used) {
    if (end > LP_IDENTITY_LIMIT) end = LP_IDENTITY_LIMIT;
    for (uint64_t a = PAGE_ALIGN(start); a + PAGE_SIZE <= end; a += PAGE_SIZE) {
        uint64_t f = a / PAGE_SIZE;
        if (frame_used(f) != used) {
            frame_set(f, used);
            if (used) g_free_frames--; else g_free_frames++;
        }
    }
}

uint64_t pmm_alloc(void) {
    for (uint64_t i = 0; i < FRAMES; i++) {
        uint64_t f = (g_hint + i) % FRAMES;
        if (!frame_used(f)) {
            frame_set(f, true);
            g_free_frames--;
            g_hint = f + 1;
            uint64_t phys = f * PAGE_SIZE;
            memset((void*)phys, 0, PAGE_SIZE);
            return phys;
        }
    }
    return 0;
}

void pmm_free(uint64_t phys) {
    uint64_t f = phys / PAGE_SIZE;
    if (phys == 0 || f >= FRAMES || !frame_used(f)) {
        panic("pmm_free: bad frame %lx", phys);
    }
    frame_set(f, false);
    g_free_frames++;
}

uint64_t pmm_free_frames(void) { return g_free_frames; }

/* ---- Kernel heap: first-fit over a contiguous region after the kernel ---- */

#define HEAP_SIZE   (32ULL * 1024 * 1024)
#define HEAP_MAGIC  0x4C415048u  /* "LAPH" */

typedef struct block {
    uint64_t      size;     /* payload bytes */
    struct block* prev;     /* physically previous block */
    struct block* next;     /* physically next block */
    uint32_t      magic;
    uint32_t      free;
} block_t;                  /* 32 bytes: keeps payloads 16-byte aligned */

static block_t* g_heap;
static uint64_t g_heap_end;

static void heap_init(uint64_t base) {
    g_heap = (block_t*)base;
    g_heap_end = base + HEAP_SIZE;
    g_heap->size = HEAP_SIZE - sizeof(block_t);
    g_heap->prev = 0;
    g_heap->next = 0;
    g_heap->magic = HEAP_MAGIC;
    g_heap->free = 1;
}

void* kmalloc(size_t size) {
    if (size == 0) size = 16;
    size = (size + 15) & ~(size_t)15;
    for (block_t* b = g_heap; b; b = b->next) {
        if (!b->free || b->size < size) continue;
        if (b->size >= size + sizeof(block_t) + 64) {
            block_t* rest = (block_t*)((uint8_t*)(b + 1) + size);
            rest->size = b->size - size - sizeof(block_t);
            rest->prev = b;
            rest->next = b->next;
            rest->magic = HEAP_MAGIC;
            rest->free = 1;
            if (b->next) b->next->prev = rest;
            b->next = rest;
            b->size = size;
        }
        b->free = 0;
        return b + 1;
    }
    return 0;
}

void* kzalloc(size_t size) {
    void* p = kmalloc(size);
    if (p) memset(p, 0, size);
    return p;
}

void kfree(void* ptr) {
    if (!ptr) return;
    block_t* b = (block_t*)ptr - 1;
    if (b->magic != HEAP_MAGIC || b->free) panic("kfree: bad pointer %p", ptr);
    b->free = 1;
    if (b->next && b->next->free) {
        block_t* n = b->next;
        b->size += sizeof(block_t) + n->size;
        b->next = n->next;
        if (n->next) n->next->prev = b;
        n->magic = 0;
    }
    if (b->prev && b->prev->free) {
        block_t* p = b->prev;
        p->size += sizeof(block_t) + b->size;
        p->next = b->next;
        if (b->next) b->next->prev = p;
        b->magic = 0;
    }
}

/* ---- Multiboot memory map ---- */

typedef struct __attribute__((packed)) {
    uint32_t size;
    uint64_t addr;
    uint64_t len;
    uint32_t type;
} mb_mmap_entry_t;

void mm_init(uint32_t mb_info) {
    const uint32_t* mbi = (const uint32_t*)(uint64_t)mb_info;
    memset(g_frame_used, 0xFF, sizeof(g_frame_used));
    g_free_frames = 0;

    uint32_t flags = mbi[0];
    if (flags & (1u << 6)) {
        uint32_t len = mbi[11];
        uint64_t addr = mbi[12];
        for (uint64_t p = addr; p < addr + len; ) {
            const mb_mmap_entry_t* e = (const mb_mmap_entry_t*)p;
            if (e->type == 1) mark_range(e->addr, e->addr + e->len, false);
            p += e->size + 4;
        }
    } else if (flags & 1u) {
        /* mem_upper: KiB above 1 MiB. */
        mark_range(0x100000, 0x100000 + (uint64_t)mbi[2] * 1024, false);
    } else {
        panic("no memory information from the boot loader");
    }

    /* Reserve low memory, the kernel image, the multiboot structures, and the
     * heap (placed right after the kernel). */
    uint64_t heap_base = PAGE_ALIGN((uint64_t)_kernel_end);
    mark_range(0, heap_base + HEAP_SIZE, true);
    mark_range(mb_info & ~0xFFFULL, (mb_info & ~0xFFFULL) + PAGE_SIZE, true);
    heap_init(heap_base);
    g_hint = (heap_base + HEAP_SIZE) / PAGE_SIZE;
}

/* ---- Address spaces ---- */

static vm_space_t  g_kernel_space;
static vm_space_t* g_current;

vm_space_t* vmm_kernel_space(void) {
    if (!g_kernel_space.pml4_phys) {
        g_kernel_space.pml4_phys = (uint64_t)boot_pml4;
        g_kernel_space.pml4_virt = (pte_t*)boot_pml4;
        g_kernel_space.owner_pid = 0;
    }
    return &g_kernel_space;
}

vm_space_t* vmm_get_current_space(void) {
    return g_current ? g_current : vmm_kernel_space();
}

int vmm_switch_address_space(vm_space_t* space) {
    if (!space) space = vmm_kernel_space();
    if (read_cr3() != space->pml4_phys) write_cr3(space->pml4_phys);
    g_current = space;
    return 0;
}

void vmm_flush_tlb_page(uint64_t virt_addr) {
    invlpg(virt_addr);
}

uint64_t vmm_alloc_page(void) {
    return pmm_alloc();
}

vm_space_t* vmm_create_address_space(uint32_t pid) {
    vm_space_t* s = (vm_space_t*)kzalloc(sizeof(vm_space_t));
    if (!s) return 0;
    uint64_t pml4 = pmm_alloc();
    uint64_t pdpt = pmm_alloc();
    if (!pml4 || !pdpt) {
        if (pml4) pmm_free(pml4);
        if (pdpt) pmm_free(pdpt);
        kfree(s);
        return 0;
    }
    /* PML4[0] is user-accessible so the user half of this PDPT is reachable;
     * PDPT[0] (the kernel identity map) is supervisor-only. */
    ((pte_t*)pml4)[0] = pdpt | PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER;
    ((pte_t*)pdpt)[0] = (uint64_t)boot_pd | PAGE_PRESENT | PAGE_WRITABLE;
    s->pml4_phys = pml4;
    s->pml4_virt = (pte_t*)pml4;
    s->owner_pid = pid;
    return s;
}

static pte_t* next_table(pte_t* table, int idx, bool create) {
    if (!(table[idx] & PAGE_PRESENT)) {
        if (!create) return 0;
        uint64_t t = pmm_alloc();
        if (!t) return 0;
        table[idx] = t | PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER;
    }
    return (pte_t*)(table[idx] & LP_PTE_ADDR_MASK);
}

pte_t* vmm_get_page_table(vm_space_t* space, uint64_t va, int level, bool create) {
    if (!space || level != PT_LEVEL) return 0;
    if (va < LP_USER_BASE || va >= LP_USER_END) return 0;
    pte_t* pdpt = next_table(space->pml4_virt, 0, false);
    if (!pdpt) return 0;
    pte_t* pd = next_table(pdpt, (va >> 30) & 511, create);
    if (!pd) return 0;
    pte_t* pt = next_table(pd, (va >> 21) & 511, create);
    if (!pt) return 0;
    return &pt[(va >> 12) & 511];
}

int vmm_map_page(vm_space_t* space, uint64_t va, uint64_t pa, uint32_t flags) {
    pte_t* pte = vmm_get_page_table(space, va & ~0xFFFULL, PT_LEVEL, true);
    if (!pte) return -1;
    *pte = (pa & LP_PTE_ADDR_MASK) | flags | PAGE_PRESENT;
    if (space == g_current) invlpg(va);
    space->page_count++;
    return 0;
}

int vmm_unmap_page(vm_space_t* space, uint64_t va) {
    pte_t* pte = vmm_get_page_table(space, va & ~0xFFFULL, PT_LEVEL, false);
    if (!pte || !(*pte & PAGE_PRESENT)) return -1;
    pmm_free(*pte & LP_PTE_ADDR_MASK);
    *pte = 0;
    if (space == g_current) invlpg(va);
    space->page_count--;
    return 0;
}

uint64_t vmm_get_physical_addr(vm_space_t* space, uint64_t va) {
    if (!space || va < LP_IDENTITY_LIMIT) return va;   /* identity map */
    pte_t* pte = vmm_get_page_table(space, va & ~0xFFFULL, PT_LEVEL, false);
    if (!pte || !(*pte & PAGE_PRESENT)) return 0;
    return (*pte & LP_PTE_ADDR_MASK) | (va & 0xFFF);
}

void vmm_for_each_page(vm_space_t* space, vmm_page_fn fn, void* ctx) {
    pte_t* pdpt = next_table(space->pml4_virt, 0, false);
    if (!pdpt) return;
    for (int i3 = 1; i3 < 512; i3++) {
        if (!(pdpt[i3] & PAGE_PRESENT)) continue;
        pte_t* pd = (pte_t*)(pdpt[i3] & LP_PTE_ADDR_MASK);
        for (int i2 = 0; i2 < 512; i2++) {
            if (!(pd[i2] & PAGE_PRESENT)) continue;
            pte_t* pt = (pte_t*)(pd[i2] & LP_PTE_ADDR_MASK);
            for (int i1 = 0; i1 < 512; i1++) {
                if (!(pt[i1] & PAGE_PRESENT)) continue;
                uint64_t va = ((uint64_t)i3 << 30) | ((uint64_t)i2 << 21) | ((uint64_t)i1 << 12);
                fn(ctx, va, pt[i1] & LP_PTE_ADDR_MASK, pt[i1]);
            }
        }
    }
}

void vmm_destroy_address_space(vm_space_t* space) {
    if (!space || space == &g_kernel_space) return;
    if (g_current == space) vmm_switch_address_space(0);
    pte_t* pdpt = next_table(space->pml4_virt, 0, false);
    if (pdpt) {
        for (int i3 = 1; i3 < 512; i3++) {
            if (!(pdpt[i3] & PAGE_PRESENT)) continue;
            pte_t* pd = (pte_t*)(pdpt[i3] & LP_PTE_ADDR_MASK);
            for (int i2 = 0; i2 < 512; i2++) {
                if (!(pd[i2] & PAGE_PRESENT)) continue;
                pte_t* pt = (pte_t*)(pd[i2] & LP_PTE_ADDR_MASK);
                for (int i1 = 0; i1 < 512; i1++) {
                    if (pt[i1] & PAGE_PRESENT) pmm_free(pt[i1] & LP_PTE_ADDR_MASK);
                }
                pmm_free((uint64_t)pt);
            }
            pmm_free((uint64_t)pd);
        }
        pmm_free((uint64_t)pdpt);
    }
    pmm_free(space->pml4_phys);
    vm_region_t* r = space->regions;
    while (r) {
        vm_region_t* n = r->next;
        kfree(r);
        r = n;
    }
    kfree(space);
}

vm_region_t* vmm_create_region(vm_space_t* space, uint64_t start, uint64_t size,
                               uint32_t flags, vmm_region_type_t type, const char* name) {
    vm_region_t* r = (vm_region_t*)kzalloc(sizeof(vm_region_t));
    if (!r) return 0;
    r->start_addr = start;
    r->end_addr = start + size;
    r->flags = flags;
    r->type = type;
    for (int i = 0; name && name[i] && i < (int)sizeof(r->name) - 1; i++) r->name[i] = name[i];
    /* Keep the list sorted by address so every walk is deterministic. */
    vm_region_t** link = &space->regions;
    vm_region_t* prev = 0;
    while (*link && (*link)->start_addr < start) {
        prev = *link;
        link = &(*link)->next;
    }
    r->next = *link;
    r->prev = prev;
    if (*link) (*link)->prev = r;
    *link = r;
    space->region_count++;
    return r;
}

vm_region_t* vmm_find_region(vm_space_t* space, uint64_t addr) {
    for (vm_region_t* r = space ? space->regions : 0; r; r = r->next) {
        if (addr >= r->start_addr && addr < r->end_addr) return r;
    }
    return 0;
}

/* ---- User memory access ---- */

bool uaccess_read(vm_space_t* space, uint64_t uaddr, void* dst, uint64_t len) {
    uint8_t* d = (uint8_t*)dst;
    while (len) {
        if (uaddr < LP_USER_BASE || uaddr >= LP_USER_END) return false;
        uint64_t phys = vmm_get_physical_addr(space, uaddr);
        if (!phys) return false;
        uint64_t chunk = PAGE_SIZE - (uaddr & 0xFFF);
        if (chunk > len) chunk = len;
        memcpy(d, (const void*)phys, chunk);
        d += chunk;
        uaddr += chunk;
        len -= chunk;
    }
    return true;
}

bool uaccess_write(vm_space_t* space, uint64_t uaddr, const void* src, uint64_t len) {
    const uint8_t* s = (const uint8_t*)src;
    while (len) {
        if (uaddr < LP_USER_BASE || uaddr >= LP_USER_END) return false;
        pte_t* pte = vmm_get_page_table(space, uaddr & ~0xFFFULL, PT_LEVEL, false);
        if (!pte || !(*pte & PAGE_PRESENT) || !(*pte & PAGE_USER)) return false;
        if (!(*pte & PAGE_WRITABLE)) {
            /* A snapshot-COW page: preserve the pre-write image first. */
            if (!(*pte & PAGE_SNAPSHOT_COW) || !checkpoint_handle_write_fault(space, uaddr)) {
                return false;
            }
        }
        uint64_t phys = (*pte & LP_PTE_ADDR_MASK) | (uaddr & 0xFFF);
        uint64_t chunk = PAGE_SIZE - (uaddr & 0xFFF);
        if (chunk > len) chunk = len;
        memcpy((void*)phys, s, chunk);
        s += chunk;
        uaddr += chunk;
        len -= chunk;
    }
    return true;
}
