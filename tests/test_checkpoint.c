/* Host-side unit test for the checkpoint engine core (#112) and the
 * page-fault capture hook (#113).
 *
 * Verifies the testable policy/walk/capture core without the full kernel:
 *   1. checkpoint_mark_pte() flips exactly the right bits and only for
 *      present+writable pages, never copying anything.
 *   2. checkpoint_mark_space() marks every writable page in writable regions,
 *      skips read-only regions/pages and unmapped pages, records the epoch,
 *      and preserves each PTE's physical frame bits. A second take re-marks
 *      nothing (no eager copy / no double-work).
 *   3. checkpoint_resolve_pte() is the exact inverse of mark.
 *   4. checkpoint_capture_page() preserves an independent copy of the page,
 *      tags it with the current epoch, and re-arms the PTE writable.
 *   5. checkpoint_handle_write_fault() captures a snapshot-COW page end-to-end
 *      and ignores ordinary write faults.
 *
 * Build: gcc -I../include -o test_checkpoint test_checkpoint.c \
 *            ../kernel/checkpoint.c ../kernel/snapshot_store.c \
 *            ../kernel/checkpoint_extstate.c ../kernel/checkpoint_barrier.c
 * (checkpoint.c + snapshot_store.c are linked; this file mocks the VMM/PM
 * symbols they call.)
 */

#include <stdint.h>
#include <stdbool.h>

/* Avoid libc headers (their <sys/types.h> ssize_t clashes with IKOS vfs.h). */
typedef __SIZE_TYPE__ size_t;
extern int   printf(const char*, ...);
extern void* malloc(size_t);
extern void  free(void*);
extern void* aligned_alloc(size_t, size_t);
extern void* memcpy(void*, const void*, size_t);
extern int   memcmp(const void*, const void*, size_t);
extern void* memset(void*, int, size_t);

/* checkpoint.c calls these freestanding kernel helpers; map to libc. */
void* kmalloc(size_t s) { return malloc(s); }
void  kfree(void* p) { free(p); }

#include "checkpoint.h"   /* pulls vmm.h only — safe */

/* ===================== Mock page table ===================== */

#define MAX_PTES 64
static struct { uint64_t addr; pte_t pte; bool mapped; } g_ptes[MAX_PTES];
static int g_pte_count;
static int g_flushes;

static void map_pte(uint64_t addr, pte_t value) {
    g_ptes[g_pte_count].addr = addr;
    g_ptes[g_pte_count].pte = value;
    g_ptes[g_pte_count].mapped = true;
    g_pte_count++;
}

/* ----- Mocked VMM symbols used by checkpoint_mark_space ----- */

pte_t* vmm_get_page_table(vm_space_t* space, uint64_t virt_addr, int level, bool create) {
    (void)space; (void)level; (void)create;
    for (int i = 0; i < g_pte_count; i++) {
        if (g_ptes[i].mapped && g_ptes[i].addr == virt_addr) {
            return &g_ptes[i].pte;
        }
    }
    return 0; /* unmapped */
}
vm_space_t* vmm_get_current_space(void) { return 0; }
void vmm_flush_tlb_page(uint64_t virt_addr) { (void)virt_addr; g_flushes++; }
uint64_t vmm_get_physical_addr(vm_space_t* space, uint64_t virt_addr) {
    (void)space; (void)virt_addr; return 0; /* clean-page walk unused here */
}
vm_space_t* vmm_create_address_space(uint32_t pid) { (void)pid; return 0; }
uint64_t vmm_alloc_page(void) { return 0; }
int vmm_map_page(vm_space_t* s, uint64_t v, uint64_t p, uint32_t f) {
    (void)s; (void)v; (void)p; (void)f; return 0; /* restore path unused here */
}

/* ----- Stubs so checkpoint.c links (checkpoint_take path, unused here) ----- */
struct process;
int pm_get_process_list(uint32_t* pids, uint32_t max, uint32_t* count) {
    (void)pids; (void)max; if (count) *count = 0; return 0;
}
struct process* pm_get_process(uint32_t pid) { (void)pid; return 0; }
/* Process-reconstruction stubs (checkpoint_register_kernel, unused here). */
struct process* process_get_by_pid(int pid) { (void)pid; return 0; }
struct process* process_create(const char* a, const char* b) { (void)a; (void)b; return 0; }
int pm_table_add_process(struct process* p) { (void)p; return 0; }
int scheduler_add_process(struct process* p) { (void)p; return 0; }

/* ===================== Test harness ===================== */

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("  FAIL: %s\n", msg); failures++; } \
    else { printf("  ok:   %s\n", msg); } \
} while (0)

#define PHYS_A 0x00123000ULL
#define PHYS_B 0x00456000ULL
#define PHYS_C 0x00789000ULL

int main(void) {
    /* === Test 1: the pure primitive === */
    printf("Test 1: checkpoint_mark_pte bit policy\n");
    {
        pte_t rw = PHYS_A | PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER;
        CHECK(checkpoint_mark_pte(&rw) == true, "present+writable returns true");
        CHECK(!(rw & PAGE_WRITABLE), "writable bit cleared");
        CHECK((rw & PAGE_SNAPSHOT_COW) != 0, "snapshot-COW bit set");
        CHECK((rw & PAGE_PRESENT) != 0, "still present");
        CHECK((rw & ~0xFFFULL) == PHYS_A, "physical frame preserved");

        pte_t ro = PHYS_B | PAGE_PRESENT | PAGE_USER;     /* read-only */
        pte_t ro_before = ro;
        CHECK(checkpoint_mark_pte(&ro) == false, "read-only returns false");
        CHECK(ro == ro_before, "read-only PTE unchanged");

        pte_t absent = PHYS_C | PAGE_WRITABLE;            /* not present */
        pte_t absent_before = absent;
        CHECK(checkpoint_mark_pte(&absent) == false, "not-present returns false");
        CHECK(absent == absent_before, "not-present PTE unchanged");

        CHECK(checkpoint_mark_pte(0) == false, "NULL returns false");
    }

    /* === Test 2: per-space walk === */
    printf("Test 2: checkpoint_mark_space\n");
    {
        checkpoint_init();
        g_pte_count = 0; g_flushes = 0;

        /* Writable region covering 4 pages at 0x400000..0x404000:
         *   page0 writable, page1 writable, page2 read-only, page3 unmapped. */
        uint64_t rstart = 0x400000ULL;
        map_pte(rstart + 0*PAGE_SIZE, PHYS_A | PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER);
        map_pte(rstart + 1*PAGE_SIZE, PHYS_B | PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER);
        map_pte(rstart + 2*PAGE_SIZE, PHYS_C | PAGE_PRESENT | PAGE_USER); /* read-only */
        /* page3 intentionally not mapped */

        /* A read-only region with a writable PTE that must be SKIPPED. */
        uint64_t roregion = 0x500000ULL;
        map_pte(roregion, 0x00AAA000ULL | PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER);

        vm_region_t ro_region = {0};
        ro_region.start_addr = roregion;
        ro_region.end_addr   = roregion + PAGE_SIZE;
        ro_region.flags      = VMM_FLAG_READ; /* no VMM_FLAG_WRITE */
        ro_region.next = 0;

        vm_region_t rw_region = {0};
        rw_region.start_addr = rstart;
        rw_region.end_addr   = rstart + 4*PAGE_SIZE;
        rw_region.flags      = VMM_FLAG_READ | VMM_FLAG_WRITE;
        rw_region.next = &ro_region;

        vm_space_t space = {0};
        space.regions = &rw_region;

        int marked = checkpoint_mark_space(&space, 7);
        CHECK(marked == 2, "exactly 2 pages marked (writable+present in writable region)");
        CHECK(space.checkpoint_epoch == 7, "space epoch recorded");

        pte_t p0 = *vmm_get_page_table(&space, rstart + 0*PAGE_SIZE, PT_LEVEL, false);
        pte_t p1 = *vmm_get_page_table(&space, rstart + 1*PAGE_SIZE, PT_LEVEL, false);
        pte_t p2 = *vmm_get_page_table(&space, rstart + 2*PAGE_SIZE, PT_LEVEL, false);
        pte_t pr = *vmm_get_page_table(&space, roregion, PT_LEVEL, false);

        CHECK(!(p0 & PAGE_WRITABLE) && (p0 & PAGE_SNAPSHOT_COW), "page0 now snapshot-COW");
        CHECK((p0 & ~0xFFFULL) == PHYS_A, "page0 physical frame preserved");
        CHECK(!(p1 & PAGE_WRITABLE) && (p1 & PAGE_SNAPSHOT_COW), "page1 now snapshot-COW");
        CHECK((p2 & PAGE_SNAPSHOT_COW) == 0, "page2 (read-only) untouched");
        CHECK((pr & PAGE_WRITABLE) && !(pr & PAGE_SNAPSHOT_COW),
              "writable page in READ-ONLY region skipped");

        /* === Test 3: a second take re-marks nothing === */
        int marked2 = checkpoint_mark_space(&space, 8);
        CHECK(marked2 == 0, "second take marks 0 (already read-only, no eager copy)");
        CHECK(space.checkpoint_epoch == 8, "space epoch advances to 8");
    }

    /* === Test 3: resolve primitive (inverse of mark) === */
    printf("Test 3: checkpoint_resolve_pte\n");
    {
        pte_t snap = PHYS_A | PAGE_PRESENT | PAGE_SNAPSHOT_COW; /* writable cleared */
        CHECK(checkpoint_resolve_pte(&snap) == true, "snapshot page resolves");
        CHECK((snap & PAGE_WRITABLE) != 0, "writable restored");
        CHECK((snap & PAGE_SNAPSHOT_COW) == 0, "snapshot tag cleared");
        CHECK((snap & ~0xFFFULL) == PHYS_A, "physical frame preserved");

        pte_t plain = PHYS_B | PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER;
        pte_t before = plain;
        CHECK(checkpoint_resolve_pte(&plain) == false, "non-snapshot page not resolved");
        CHECK(plain == before, "non-snapshot PTE unchanged");
    }

    /* === Test 4: capture preserves contents and re-arms the page === */
    printf("Test 4: checkpoint_capture_page\n");
    {
        checkpoint_init();
        CHECK(checkpoint_capture_count() == 0, "init clears capture list");

        uint8_t* page = (uint8_t*)aligned_alloc(PAGE_SIZE, PAGE_SIZE);
        for (int i = 0; i < (int)PAGE_SIZE; i++) page[i] = (uint8_t)(i * 5 + 1);

        pte_t pte = PHYS_A | PAGE_PRESENT | PAGE_SNAPSHOT_COW; /* marked, write-protected */
        int rc = checkpoint_capture_page(42, 0x401000, page, &pte);
        CHECK(rc == CHECKPOINT_OK, "capture returns OK");
        CHECK(checkpoint_capture_count() == 1, "one capture recorded");

        checkpoint_capture_t* c = checkpoint_captures();
        CHECK(c && c->pid == 42, "capture pid correct");
        CHECK(c && c->virt_addr == 0x401000, "capture virt_addr correct");
        CHECK(c && c->epoch == checkpoint_current_epoch(), "capture epoch == current");
        CHECK(c && memcmp(c->data, page, PAGE_SIZE) == 0, "captured contents match page");

        /* Mutating the page after capture must not change the saved copy. */
        page[0] ^= 0xFF;
        CHECK(c && ((uint8_t*)c->data)[0] != page[0], "captured copy is independent");

        CHECK((pte & PAGE_WRITABLE) != 0, "page re-armed writable after capture");
        CHECK((pte & PAGE_SNAPSHOT_COW) == 0, "snapshot tag cleared after capture");

        /* Capturing a non-snapshot page is a no-op error. */
        pte_t plain = PHYS_B | PAGE_PRESENT | PAGE_WRITABLE;
        int rc2 = checkpoint_capture_page(42, 0x402000, page, &plain);
        CHECK(rc2 == CHECKPOINT_ERR_STATE, "non-snapshot capture rejected");
        CHECK(checkpoint_capture_count() == 1, "rejected capture not recorded");

        free(page);
    }

    /* === Test 5: page-fault hook end-to-end (real host page) === */
    printf("Test 5: checkpoint_handle_write_fault\n");
    {
        checkpoint_init();
        g_pte_count = 0; g_flushes = 0;
        checkpoint_take(); /* open an epoch (no processes to mark) */

        /* Use a real, page-aligned host buffer as the "virtual page" so the
         * hook can read its contents at the fault address. */
        uint8_t* page = (uint8_t*)aligned_alloc(PAGE_SIZE, PAGE_SIZE);
        for (int i = 0; i < (int)PAGE_SIZE; i++) page[i] = (uint8_t)(i ^ 0x3C);
        uint64_t vaddr = (uint64_t)(uintptr_t)page; /* page-aligned */

        /* Map a present, snapshot-COW PTE for that page. */
        map_pte(vaddr, PHYS_A | PAGE_PRESENT | PAGE_SNAPSHOT_COW);

        vm_space_t space = {0};
        space.owner_pid = 99;

        bool handled = checkpoint_handle_write_fault(&space, vaddr + 17 /* mid-page */);
        CHECK(handled == true, "write fault on snapshot page handled");
        CHECK(checkpoint_capture_count() == 1, "hook recorded one capture");
        checkpoint_capture_t* c = checkpoint_captures();
        CHECK(c && c->pid == 99, "capture pid from space->owner_pid");
        CHECK(c && c->virt_addr == vaddr, "capture virt_addr page-aligned");
        CHECK(c && memcmp(c->data, page, PAGE_SIZE) == 0, "hook captured page contents");

        pte_t now = *vmm_get_page_table(&space, vaddr, PT_LEVEL, false);
        CHECK((now & PAGE_WRITABLE) && !(now & PAGE_SNAPSHOT_COW), "page writable after hook");
        CHECK(g_flushes == 1, "TLB flushed once for the page");

        /* A fault on a non-snapshot (ordinary) page is not ours. */
        uint8_t* page2 = (uint8_t*)aligned_alloc(PAGE_SIZE, PAGE_SIZE);
        uint64_t vaddr2 = (uint64_t)(uintptr_t)page2;
        map_pte(vaddr2, PHYS_B | PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER);
        bool handled2 = checkpoint_handle_write_fault(&space, vaddr2);
        CHECK(handled2 == false, "ordinary write fault not claimed by hook");
        CHECK(checkpoint_capture_count() == 1, "no extra capture for ordinary fault");

        free(page); free(page2);
    }

    /* === Test 6: a COW fault after the epoch's writeback committed (#224) === */
    printf("Test 6: COW fault with no open epoch resolves without capturing\n");
    {
        checkpoint_init();               /* epoch 0, nothing open */
        g_pte_count = 0; g_flushes = 0;
        uint8_t* page = (uint8_t*)aligned_alloc(PAGE_SIZE, PAGE_SIZE);
        uint64_t vaddr = (uint64_t)(uintptr_t)page;
        map_pte(vaddr, PHYS_A | PAGE_PRESENT | PAGE_SNAPSHOT_COW);
        vm_space_t space = {0};
        space.owner_pid = 7;

        bool handled = checkpoint_handle_write_fault(&space, vaddr);
        CHECK(handled == true, "stale snapshot-COW fault still handled");
        CHECK(checkpoint_capture_count() == 0, "no capture outside an open epoch");
        pte_t now = *vmm_get_page_table(&space, vaddr, PT_LEVEL, false);
        CHECK((now & PAGE_WRITABLE) && !(now & PAGE_SNAPSHOT_COW), "page writable again");
        CHECK(g_flushes == 1, "TLB flushed");
        free(page);
    }

    printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "PASSED",
           failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
