/* IKOS Virtual Memory Manager (VMM)
 * Provides paging-based virtual memory management with isolated address spaces
 */

#ifndef VMM_H
#define VMM_H

#include <stdint.h>
#include <stdbool.h>

/* Page size and alignment */
#define PAGE_SIZE           4096

/* A physical frame as the code reading it sees it (#238): through the
 * kernel's direct map in the booted kernel, the address itself in the host
 * tests (whose mock frames are ordinary host memory). */
#ifdef LAPLACE_KERNEL
#define PHYS_TO_VIRT(p) ((void*)((uint64_t)(p) + 0xFFFF800000000000ULL))
#else
#define PHYS_TO_VIRT(p) ((void*)(uintptr_t)(p))
#endif
#define PAGE_ALIGN(addr)    (((addr) + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1))
#define PAGE_FRAME(addr)    ((addr) >> 12)
#define FRAME_ADDR(frame)   ((frame) << 12)

/* Virtual memory layout */
#define KERNEL_VIRTUAL_BASE     0xFFFFFFFF80000000ULL  /* -2GB */
#define USER_VIRTUAL_BASE       0x0000000000400000ULL  /* 4MB */
#define USER_VIRTUAL_END        0x00007FFFFFFFFFFFULL  /* 128TB */
#define USER_STACK_TOP          0x00007FFFFFFFEFFFULL  /* Stack top */
#define USER_HEAP_BASE          0x0000000000800000ULL  /* 8MB */

/* Page table hierarchy levels */
#define PML4_LEVEL          3
#define PDPT_LEVEL          2
#define PD_LEVEL            1
#define PT_LEVEL            0

/* Page flags */
#define PAGE_PRESENT        0x001   /* Page is present in memory */
#define PAGE_WRITABLE       0x002   /* Page is writable */
#define PAGE_USER           0x004   /* Page is accessible from user mode */
#define PAGE_WRITETHROUGH   0x008   /* Write-through caching */
#define PAGE_CACHEDISABLE   0x010   /* Cache disabled */
#define PAGE_ACCESSED       0x020   /* Page has been accessed */
#define PAGE_DIRTY          0x040   /* Page has been written to */
#define PAGE_LARGE          0x080   /* Large page (2MB for PD entries) */
#define PAGE_GLOBAL         0x100   /* Global page (not flushed on CR3 reload) */
/* Bits 9-11 are available for software use and ignored by the CPU. */
#define PAGE_SNAPSHOT_COW   0x200   /* Page is copy-on-write for a checkpoint snapshot (issue #111) */
#define PAGE_NX             0x8000000000000000ULL /* No-execute bit */
#define PAGE_NO_EXECUTE     0x8000000000000000ULL /* NX bit */

/* VMM flags */
#define VMM_FLAG_READ       0x01    /* Read permission */
#define VMM_FLAG_WRITE      0x02    /* Write permission */
#define VMM_FLAG_EXEC       0x04    /* Execute permission */
#define VMM_FLAG_USER       0x08    /* User accessible */
#define VMM_FLAG_SHARED     0x10    /* Shared between processes */
#define VMM_FLAG_COW        0x20    /* Copy-on-write */
#define VMM_FLAG_LAZY       0x40    /* Lazy allocation */
#define VMM_FLAG_LOCKED     0x80    /* Memory locked (no swap) */

/* Memory protection flags */
#define VMM_PROT_READ       0x1     /* Memory is readable */
#define VMM_PROT_WRITE      0x2     /* Memory is writable */
#define VMM_PROT_EXEC       0x4     /* Memory is executable */
#define VMM_PROT_NONE       0x0     /* Memory is not accessible */

/* Memory mapping flags */
#define VMM_MMAP_FIXED      0x10    /* Fixed mapping address */
#define VMM_MMAP_LAZY       0x20    /* Lazy allocation */
#define VMM_MMAP_SHARED     0x40    /* Shared mapping */
typedef enum {
    VMM_REGION_CODE     = 0,    /* Code/text segment */
    VMM_REGION_DATA     = 1,    /* Data segment */
    VMM_REGION_HEAP     = 2,    /* Heap memory */
    VMM_REGION_STACK    = 3,    /* Stack memory */
    VMM_REGION_MMAP     = 4,    /* Memory-mapped region */
    VMM_REGION_SHARED   = 5,    /* Shared memory */
    VMM_REGION_KERNEL   = 6     /* Kernel memory */
} vmm_region_type_t;

/* Page table entry structure */
typedef uint64_t pte_t;

/* Page frame information */
typedef struct page_frame {
    uint32_t frame_number;          /* Physical frame number */
    uint32_t ref_count;             /* Reference count */
    uint32_t flags;                 /* Frame flags */
    uint32_t owner_pid;             /* Process that owns this frame */
    struct page_frame* next;        /* Next in free list */
} page_frame_t;

/* Virtual memory region */
typedef struct vm_region {
    uint64_t start_addr;            /* Virtual start address */
    uint64_t end_addr;              /* Virtual end address */
    uint32_t flags;                 /* Region flags */
    vmm_region_type_t type;         /* Region type */
    uint32_t file_offset;           /* Offset in backing file (if any) */
    char name[32];                  /* Region name */
    struct vm_region* next;         /* Next region */
    struct vm_region* prev;         /* Previous region */
} vm_region_t;

/* Virtual address space */
typedef struct vm_space {
    uint64_t pml4_phys;             /* Physical address of PML4 table */
    pte_t* pml4_virt;               /* Virtual address of PML4 table */
    vm_region_t* regions;           /* List of memory regions */
    uint64_t heap_start;            /* Heap start address */
    uint64_t heap_end;              /* Current heap end */
    uint64_t stack_start;           /* Stack start address */
    uint64_t mmap_start;            /* Memory mapping start */
    uint32_t region_count;          /* Number of regions */
    uint32_t page_count;            /* Number of allocated pages */
    uint32_t owner_pid;             /* Process ID */
    uint64_t checkpoint_epoch;      /* Checkpoint epoch this space was last marked at (issue #111) */
    uint64_t snapshot_map_index;    /* Index/ref into the on-disk snapshot map (0 = none yet) */
} vm_space_t;

/* Page fault information */
typedef struct page_fault_info {
    uint64_t fault_addr;            /* Faulting virtual address */
    uint64_t error_code;            /* CPU error code */
    uint64_t instruction_ptr;       /* RIP when fault occurred */
    bool is_write;                  /* Was it a write access? */
    bool is_user;                   /* Was it from user mode? */
    bool is_present;                /* Was page present? */
    bool is_instruction_fetch;      /* Was it instruction fetch? */
} page_fault_info_t;

/* VMM statistics */
typedef struct vmm_stats {
    uint64_t total_pages;           /* Total pages in system */
    uint64_t free_pages;            /* Free pages available */
    uint64_t allocated_pages;       /* Pages currently allocated */
    uint64_t shared_pages;          /* Shared pages */
    uint64_t cow_pages;             /* Copy-on-write pages */
    uint64_t page_faults;           /* Total page faults */
    uint64_t major_faults;          /* Major page faults */
    uint64_t minor_faults;          /* Minor page faults */
    uint64_t cow_faults;            /* Copy-on-write faults */
    uint64_t swap_pages;            /* Pages swapped out */
    uint64_t memory_usage;          /* Total memory usage */
} vmm_stats_t;

/* VMM initialization and management */
int vmm_init(uint64_t memory_size);
void vmm_shutdown(void);

/* Address space management */
vm_space_t* vmm_create_address_space(uint32_t pid);
void vmm_destroy_address_space(vm_space_t* space);
int vmm_switch_address_space(vm_space_t* space);
vm_space_t* vmm_get_current_space(void);

/* Memory region management */
vm_region_t* vmm_create_region(vm_space_t* space, uint64_t start, uint64_t size, 
                               uint32_t flags, vmm_region_type_t type, const char* name);
int vmm_destroy_region(vm_space_t* space, uint64_t start);
vm_region_t* vmm_find_region(vm_space_t* space, uint64_t addr);
int vmm_expand_region(vm_space_t* space, vm_region_t* region, uint64_t new_size);
int vmm_split_region(vm_space_t* space, uint64_t split_addr);

/* Page allocation and mapping */
uint64_t vmm_alloc_page(void);
void vmm_free_page(uint64_t phys_addr);
int vmm_map_page(vm_space_t* space, uint64_t virt_addr, uint64_t phys_addr, uint32_t flags);
int vmm_unmap_page(vm_space_t* space, uint64_t virt_addr);
uint64_t vmm_get_physical_addr(vm_space_t* space, uint64_t virt_addr);

/* Memory allocation functions */
void* vmm_alloc_virtual(vm_space_t* space, uint64_t size, uint32_t flags);
void vmm_free_virtual(vm_space_t* space, void* addr, uint64_t size);
void* vmm_alloc_at(vm_space_t* space, uint64_t addr, uint64_t size, uint32_t flags);
void* vmm_alloc_user_pages(vm_space_t* space, uint64_t size);

/* Page table management */
pte_t* vmm_get_page_table(vm_space_t* space, uint64_t virt_addr, int level, bool create);
int vmm_set_page_flags(vm_space_t* space, uint64_t virt_addr, uint32_t flags);
uint32_t vmm_get_page_flags(vm_space_t* space, uint64_t virt_addr);

/* Page fault handling */
int vmm_handle_page_fault(page_fault_info_t* fault_info);
void vmm_page_fault_handler(uint64_t fault_addr, uint64_t error_code);

/* Copy-on-write support */
int vmm_mark_cow(vm_space_t* space, uint64_t start, uint64_t size);
int vmm_handle_cow_fault(vm_space_t* space, uint64_t fault_addr);

/* Process memory management */
int vmm_setup_user_space(vm_space_t* space);
int vmm_clone_address_space(vm_space_t* dest, vm_space_t* src);
int vmm_load_elf_segment(vm_space_t* space, uint64_t virt_addr, uint64_t size, 
                         uint32_t flags, void* data);

/* Heap management */
void* vmm_sbrk(vm_space_t* space, int64_t increment);
void* vmm_expand_heap(vm_space_t* space, int64_t increment);

/* Memory mapping (mmap) */
void* vmm_mmap(vm_space_t* space, void* addr, uint64_t size, uint32_t prot, 
               uint32_t flags);
int vmm_munmap(vm_space_t* space, void* addr, uint64_t size);
int vmm_mprotect(vm_space_t* space, void* addr, uint64_t size, uint32_t prot);

/* Physical memory management */
int vmm_init_physical_memory(uint64_t memory_size);
page_frame_t* vmm_alloc_frame(void);
void vmm_free_frame(page_frame_t* frame);
uint64_t vmm_get_free_memory(void);

/* Cache and TLB management */
void vmm_flush_tlb(void);
void vmm_flush_tlb_page(uint64_t virt_addr);
void vmm_invalidate_page(uint64_t virt_addr);

/* Debugging and introspection */
void vmm_dump_address_space(vm_space_t* space);
void vmm_dump_page_tables(vm_space_t* space, uint64_t virt_addr);
vmm_stats_t* vmm_get_stats(void);
void vmm_print_memory_map(vm_space_t* space);

/* Utility functions */
bool vmm_is_user_addr(uint64_t addr);
bool vmm_is_kernel_addr(uint64_t addr);
uint64_t vmm_align_down(uint64_t addr, uint64_t alignment);
uint64_t vmm_align_up(uint64_t addr, uint64_t alignment);

/* Error codes */
#define VMM_SUCCESS             0
#define VMM_ERROR_NOMEM         -1
#define VMM_ERROR_INVALID_ADDR  -2
#define VMM_ERROR_PERM_DENIED   -3
#define VMM_ERROR_NOT_FOUND     -4
#define VMM_ERROR_EXISTS        -5
#define VMM_ERROR_FAULT         -6
#define VMM_ERROR_INVALID_SIZE  -7
#define VMM_ERROR_INVALID_FLAGS -8

/* Protection flags for mmap/mprotect */
#define PROT_NONE               0x0
#define PROT_READ               0x1
#define PROT_WRITE              0x2
#define PROT_EXEC               0x4

/* mmap flags */
#define MAP_SHARED              0x01
#define MAP_PRIVATE             0x02
#define MAP_FIXED               0x10
#define MAP_ANONYMOUS           0x20

#endif /* VMM_H */
