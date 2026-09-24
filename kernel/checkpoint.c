/* IKOS Orthogonal Persistence - Checkpoint Engine
 *
 * See include/checkpoint.h and
 * docs/architecture/orthogonal-persistence.md. Issue #112 (epic #121).
 */

#include "checkpoint.h"
#include "checkpoint_extstate.h"
#include "checkpoint_barrier.h"
#include "process_manager.h"
#include <stddef.h>

/* Freestanding helpers provided by the kernel. */
extern void* kmalloc(size_t size);
extern void  kfree(void* ptr);
extern void* memcpy(void* dest, const void* src, size_t size);
extern void* memset(void* dest, int value, size_t size);

/* Engine state. */
static checkpoint_state_t g_checkpoint = {0};

/* In-memory capture list (the "snapshot log" drained by writeback, #115). */
static checkpoint_capture_t* g_captures = 0;
static uint64_t g_capture_count = 0;

/* Periodic-trigger state (#117). */
static bool     g_timer_enabled = false;
static uint64_t g_timer_interval = CHECKPOINT_DEFAULT_INTERVAL_TICKS;
static uint64_t g_timer_ticks = 0;

/* Quiescent-point barrier (#138). The trigger arms it; the checkpoint is taken
 * when every CPU has parked. Single CPU until SMP lands. */
static checkpoint_barrier_t g_barrier;

/* Post-commit journal hook (#194). Injected by the journal-capture adapter so
 * the checkpoint core stays free of the journal/record dependencies. */
static checkpoint_journal_hook_fn g_journal_hook = 0;

void checkpoint_set_journal_hook(checkpoint_journal_hook_fn hook) {
    g_journal_hook = hook;
}

void checkpoint_init(void) {
    g_checkpoint.current_epoch = 0;
    g_checkpoint.spaces_marked = 0;
    g_checkpoint.pages_marked = 0;
    g_checkpoint.total_takes = 0;
    g_checkpoint.epoch_open = false;
    g_timer_ticks = 0;
    checkpoint_barrier_init(&g_barrier, 1); /* single-CPU for now */
    checkpoint_clear_captures();
}

uint64_t checkpoint_current_epoch(void) {
    return g_checkpoint.current_epoch;
}

void checkpoint_set_epoch(uint64_t epoch) {
    /* A machine rebuilt from a keyframe continues numbering after it; any
     * capture log belongs to the discarded timeline. */
    checkpoint_clear_captures();
    g_checkpoint.current_epoch = epoch;
    g_checkpoint.epoch_open = false;
}

const checkpoint_state_t* checkpoint_get_state(void) {
    return &g_checkpoint;
}

/* ----- Pure marking primitive (the policy core) ----- */

bool checkpoint_mark_pte(pte_t* pte) {
    if (!pte) {
        return false;
    }
    pte_t entry = *pte;

    /* Only currently-writable, present pages need protecting: a read-only
     * page cannot change, so there is nothing to copy on a later write. */
    if (!(entry & PAGE_PRESENT) || !(entry & PAGE_WRITABLE)) {
        return false;
    }

    /* Drop write permission and tag as snapshot copy-on-write. The next write
     * faults; the page-fault hook (#113) preserves the page before allowing
     * the write to proceed. No copy happens here. */
    entry &= ~(pte_t)PAGE_WRITABLE;
    entry |= PAGE_SNAPSHOT_COW;
    *pte = entry;
    return true;
}

checkpoint_page_action_t checkpoint_page_action(bool region_writable, pte_t pte) {
    if (!(pte & PAGE_PRESENT)) {
        return CHECKPOINT_PAGE_SKIP;
    }
    if (region_writable) {
        /* A clean (unmodified) writable page is still tagged snapshot-COW; a
         * modified one had its tag cleared and is streamed via its capture. */
        return (pte & PAGE_SNAPSHOT_COW) ? CHECKPOINT_PAGE_PERSIST_RW
                                         : CHECKPOINT_PAGE_SKIP;
    }
    /* Read-only page (e.g. code): never changes, was never marked; persist it
     * directly so the restored process has its text. */
    return CHECKPOINT_PAGE_PERSIST_RO;
}

bool checkpoint_resolve_pte(pte_t* pte) {
    if (!pte) {
        return false;
    }
    pte_t entry = *pte;
    if (!(entry & PAGE_SNAPSHOT_COW)) {
        return false;
    }
    /* Restore write permission and drop the snapshot tag: the pre-write image
     * has been captured, so the page may be written normally again. */
    entry &= ~(pte_t)PAGE_SNAPSHOT_COW;
    entry |= PAGE_WRITABLE;
    *pte = entry;
    return true;
}

/* ----- Per-space walk ----- */

int checkpoint_mark_space(vm_space_t* space, uint64_t epoch) {
    if (!space) {
        return CHECKPOINT_ERR_PARAM;
    }

    int marked = 0;
    vm_space_t* current = vmm_get_current_space();

    /* Walk the space's regions; only writable regions can hold writable
     * pages worth protecting. */
    for (vm_region_t* region = space->regions; region; region = region->next) {
        if (!(region->flags & VMM_FLAG_WRITE)) {
            continue;
        }

        for (uint64_t addr = region->start_addr; addr < region->end_addr;
             addr += PAGE_SIZE) {
            pte_t* pte = vmm_get_page_table(space, addr, PT_LEVEL, false);
            if (!pte) {
                continue; /* not mapped */
            }
            if (checkpoint_mark_pte(pte)) {
                marked++;
                /* Only the active address space has live TLB entries to
                 * invalidate; others are flushed on their next CR3 load. */
                if (space == current) {
                    vmm_flush_tlb_page(addr);
                }
            }
        }
    }

    space->checkpoint_epoch = epoch;
    return marked;
}

/* ----- Capture (page-fault hook) ----- */

checkpoint_capture_t* checkpoint_captures(void) {
    return g_captures;
}

uint64_t checkpoint_capture_count(void) {
    return g_capture_count;
}

void checkpoint_clear_captures(void) {
    checkpoint_capture_t* c = g_captures;
    while (c) {
        checkpoint_capture_t* next = c->next;
        if (c->data) {
            kfree(c->data);
        }
        kfree(c);
        c = next;
    }
    g_captures = 0;
    g_capture_count = 0;
}

int checkpoint_capture_page(uint32_t pid, uint64_t virt_addr,
                            const void* page_contents, pte_t* pte) {
    if (!pte || !page_contents) {
        return CHECKPOINT_ERR_PARAM;
    }
    if (!(*pte & PAGE_SNAPSHOT_COW)) {
        /* Not a snapshot page: nothing to preserve. */
        return CHECKPOINT_ERR_STATE;
    }

    checkpoint_capture_t* rec =
        (checkpoint_capture_t*)kmalloc(sizeof(checkpoint_capture_t));
    if (!rec) {
        return CHECKPOINT_ERR_PARAM;
    }
    rec->data = kmalloc(PAGE_SIZE);
    if (!rec->data) {
        kfree(rec);
        return CHECKPOINT_ERR_PARAM;
    }

    /* Preserve the pre-write contents, then make the page writable again. */
    memcpy(rec->data, page_contents, PAGE_SIZE);
    rec->epoch = g_checkpoint.current_epoch;
    rec->pid = pid;
    rec->flags = 0;
    rec->virt_addr = virt_addr;
    rec->next = g_captures;
    g_captures = rec;
    g_capture_count++;

    checkpoint_resolve_pte(pte);
    return CHECKPOINT_OK;
}

int checkpoint_capture_context(uint32_t pid, const void* ctx, uint32_t ctx_size) {
    if (!ctx || ctx_size == 0 || ctx_size > PAGE_SIZE) {
        return CHECKPOINT_ERR_PARAM;
    }

    checkpoint_capture_t* rec =
        (checkpoint_capture_t*)kmalloc(sizeof(checkpoint_capture_t));
    if (!rec) {
        return CHECKPOINT_ERR_PARAM;
    }
    rec->data = kmalloc(PAGE_SIZE);
    if (!rec->data) {
        kfree(rec);
        return CHECKPOINT_ERR_PARAM;
    }

    /* Store the context zero-padded into a page-sized record so it rides the
     * existing page-record path; the flag marks it as a context, not memory. */
    memset(rec->data, 0, PAGE_SIZE);
    memcpy(rec->data, ctx, ctx_size);
    rec->epoch = g_checkpoint.current_epoch;
    rec->pid = pid;
    rec->flags = CHECKPOINT_REC_CONTEXT;
    rec->virt_addr = CHECKPOINT_CONTEXT_VADDR;
    rec->next = g_captures;
    g_captures = rec;
    g_capture_count++;
    return CHECKPOINT_OK;
}

int checkpoint_capture_kernel_blob(uint32_t tag, const void* data, uint32_t size) {
    if (!data || size == 0) {
        return CHECKPOINT_ERR_PARAM;
    }
    const uint8_t* p = (const uint8_t*)data;
    uint32_t chunks = (size + PAGE_SIZE - 1) / PAGE_SIZE;

    for (uint32_t i = 0; i < chunks; i++) {
        checkpoint_capture_t* rec =
            (checkpoint_capture_t*)kmalloc(sizeof(checkpoint_capture_t));
        if (!rec) {
            return CHECKPOINT_ERR_PARAM;
        }
        rec->data = kmalloc(PAGE_SIZE);
        if (!rec->data) {
            kfree(rec);
            return CHECKPOINT_ERR_PARAM;
        }

        uint32_t off = i * PAGE_SIZE;
        uint32_t n = (size - off) < PAGE_SIZE ? (size - off) : PAGE_SIZE;
        memset(rec->data, 0, PAGE_SIZE);
        memcpy(rec->data, p + off, n);

        rec->epoch = g_checkpoint.current_epoch;
        rec->pid = tag;
        rec->flags = CHECKPOINT_REC_KERNEL;
        /* Self-describing: total size in the high 32 bits, chunk index in the low. */
        rec->virt_addr = ((uint64_t)size << 32) | (uint64_t)i;
        rec->next = g_captures;
        g_captures = rec;
        g_capture_count++;
    }
    return CHECKPOINT_OK;
}

bool checkpoint_handle_write_fault(vm_space_t* space, uint64_t fault_addr) {
    if (!space) {
        return false;
    }
    uint64_t page_addr = fault_addr & ~((uint64_t)PAGE_SIZE - 1);

    pte_t* pte = vmm_get_page_table(space, page_addr, PT_LEVEL, false);
    if (!pte || !(*pte & PAGE_PRESENT) || !(*pte & PAGE_SNAPSHOT_COW)) {
        return false; /* not a snapshot-COW fault */
    }

    if (!g_checkpoint.epoch_open) {
        /* The epoch's writeback already committed, so the page's checkpoint
         * image is on disk: there is nothing to preserve. Just make the page
         * writable. Capturing here would leak a stale pre-write image into the
         * next checkpoint's stream (#224). */
        checkpoint_resolve_pte(pte);
        vmm_flush_tlb_page(page_addr);
        return true;
    }

    /* The faulting page is mapped at page_addr in the current address space,
     * so its current contents are readable there. */
    int rc = checkpoint_capture_page(space->owner_pid, page_addr,
                                     (const void*)page_addr, pte);
    if (rc != CHECKPOINT_OK) {
        return false;
    }

    vmm_flush_tlb_page(page_addr);
    return true;
}

/* ----- Writeback ----- */

/* Persist the pages of every live user space that are not already streamed via
 * a capture: clean (unmodified) writable pages, whose frames still hold the
 * checkpoint-time content, and read-only pages (e.g. code), which never change.
 * Read-only pages are tagged CHECKPOINT_REC_READONLY so restore re-maps them
 * without write permission. */
static int writeback_clean_pages(snapshot_writer_t* writer) {
    uint32_t pids[PM_MAX_PROCESSES];
    uint32_t count = 0;
    if (pm_get_process_list(pids, PM_MAX_PROCESSES, &count) != 0) {
        return CHECKPOINT_OK; /* nothing to walk */
    }

    uint8_t* buf = (uint8_t*)kmalloc(PAGE_SIZE);
    if (!buf) {
        return CHECKPOINT_ERR_PARAM;
    }

    for (uint32_t i = 0; i < count; i++) {
        process_t* proc = pm_get_process(pids[i]);
        if (!proc || !proc->address_space) {
            continue;
        }
        vm_space_t* space = proc->address_space;

        for (vm_region_t* region = space->regions; region; region = region->next) {
            bool writable = (region->flags & VMM_FLAG_WRITE) != 0;
            for (uint64_t addr = region->start_addr; addr < region->end_addr;
                 addr += PAGE_SIZE) {
                pte_t* pte = vmm_get_page_table(space, addr, PT_LEVEL, false);
                if (!pte) {
                    continue; /* unmapped */
                }
                checkpoint_page_action_t action = checkpoint_page_action(writable, *pte);
                if (action == CHECKPOINT_PAGE_SKIP) {
                    continue;
                }
                uint64_t phys = vmm_get_physical_addr(space, addr);
                if (!phys) {
                    continue;
                }
                /* Physical frames are directly addressable in the kernel, the
                 * same convention vmm.c uses to walk page tables. */
                memcpy(buf, (const void*)phys, PAGE_SIZE);
                uint32_t flags = (action == CHECKPOINT_PAGE_PERSIST_RO)
                                     ? CHECKPOINT_REC_READONLY : 0;
                if (snapshot_writer_add_page(writer, space->owner_pid, addr, flags,
                                             buf) != SNAPSHOT_OK) {
                    kfree(buf);
                    return CHECKPOINT_ERR_IO;
                }
            }
        }
    }

    kfree(buf);
    return CHECKPOINT_OK;
}

int checkpoint_stream_pages(snapshot_writer_t* writer) {
    if (!writer) {
        return CHECKPOINT_ERR_PARAM;
    }

    /* 1. Modified pages: the captured pre-checkpoint images. */
    for (checkpoint_capture_t* c = g_captures; c; c = c->next) {
        if (snapshot_writer_add_page(writer, c->pid, c->virt_addr, c->flags,
                                     c->data) != SNAPSHOT_OK) {
            return CHECKPOINT_ERR_IO;
        }
    }

    /* 2. Still-clean pages: live content (== checkpoint-time content). */
    return writeback_clean_pages(writer);
}

void checkpoint_after_commit(uint64_t epoch) {
    /* Persist this epoch's input journal alongside the just-committed
     * checkpoint (#194). Advisory: the checkpoint is already durable, so a
     * journal failure does not fail the checkpoint (replay simply cannot
     * re-drive past this keyframe until a later epoch journals cleanly). */
    if (g_journal_hook) {
        g_journal_hook(epoch);
    }

    /* Drop the in-memory snapshot log and close the epoch. */
    checkpoint_clear_captures();
    g_checkpoint.epoch_open = false;
}

int checkpoint_writeback(snapshot_store_t* store) {
    if (!store) {
        return CHECKPOINT_ERR_PARAM;
    }

    snapshot_writer_t writer;
    if (snapshot_store_begin(store, g_checkpoint.current_epoch, &writer) != SNAPSHOT_OK) {
        return CHECKPOINT_ERR_IO;
    }

    int rc = checkpoint_stream_pages(&writer);
    if (rc != CHECKPOINT_OK) {
        return rc;
    }

    /* Finalize: slot CRC + the superblock flip (the single commit point). */
    if (snapshot_store_commit(&writer) != SNAPSHOT_OK) {
        return CHECKPOINT_ERR_IO;
    }

    checkpoint_after_commit(g_checkpoint.current_epoch);
    return CHECKPOINT_OK;
}

/* ----- Restore ----- */

int checkpoint_restore(snapshot_store_t* store, checkpoint_apply_fn apply, void* ctx) {
    if (!store || !apply) {
        return CHECKPOINT_ERR_PARAM;
    }

    snapshot_reader_t reader;
    int rc = snapshot_store_load(store, &reader);
    if (rc != SNAPSHOT_OK) {
        /* No valid checkpoint (or corrupt): the caller should cold-boot. */
        return CHECKPOINT_ERR_NO_CHECKPOINT;
    }

    uint8_t* buf = (uint8_t*)kmalloc(PAGE_SIZE);
    if (!buf) {
        return CHECKPOINT_ERR_PARAM;
    }

    int restored = 0;
    snapshot_page_record_t rec;
    rec.page_data = buf;
    while (snapshot_reader_next(&reader, &rec) == SNAPSHOT_OK) {
        rc = apply(ctx, &rec);
        if (rc != CHECKPOINT_OK) {
            kfree(buf);
            return rc;
        }
        restored++;
    }
    kfree(buf);

    /* Resume at the checkpoint's epoch so the next take() advances from there. */
    g_checkpoint.current_epoch = reader.epoch;
    g_checkpoint.epoch_open = false;
    return restored;
}

/* ----- Restore finalize (generic) ----- */

int checkpoint_finalize_restore(const checkpoint_restored_process_t* procs, int count,
                                checkpoint_register_fn reg, void* reg_ctx) {
    if (!procs || !reg) {
        return CHECKPOINT_ERR_PARAM;
    }
    int registered = 0;
    for (int i = 0; i < count; i++) {
        int r = reg(reg_ctx, &procs[i]);
        if (r != CHECKPOINT_OK) {
            return r;
        }
        registered++;
    }
    return registered;
}

/* ----- Boot adapter: reconstruct processes from a checkpoint ----- */

/* pid -> reconstructed process, built up as records (pages + context) replay. */
#define CHECKPOINT_RESTORE_MAX_PROCS 64
typedef struct {
    checkpoint_restored_process_t procs[CHECKPOINT_RESTORE_MAX_PROCS];
    int count;
} checkpoint_restore_ctx_t;

static checkpoint_restored_process_t* restore_entry_for(checkpoint_restore_ctx_t* c,
                                                        uint32_t pid) {
    for (int i = 0; i < c->count; i++) {
        if (c->procs[i].pid == pid) {
            return &c->procs[i];
        }
    }
    if (c->count >= CHECKPOINT_RESTORE_MAX_PROCS) {
        return 0;
    }
    checkpoint_restored_process_t* e = &c->procs[c->count++];
    e->pid = pid;
    e->space = 0;
    e->context_size = 0;
    e->has_context = false;
    return e;
}

static int checkpoint_restore_apply_kernel(void* ctx, const snapshot_page_record_t* rec) {
    checkpoint_restore_ctx_t* c = (checkpoint_restore_ctx_t*)ctx;

    checkpoint_restored_process_t* e = restore_entry_for(c, rec->pid);
    if (!e) {
        return CHECKPOINT_ERR_PARAM;
    }

    /* Context records carry CPU state, not memory: stash them on the process
     * entry for the finalize/register step rather than mapping a page. */
    if (rec->flags & CHECKPOINT_REC_CONTEXT) {
        memcpy(e->context, rec->page_data, CHECKPOINT_CONTEXT_MAX);
        e->context_size = CHECKPOINT_CONTEXT_MAX;
        e->has_context = true;
        return CHECKPOINT_OK;
    }

    /* Kernel-state blob chunks are reassembled by the kernel-state restorers
     * (#140+); the page-restore path skips them. */
    if (rec->flags & CHECKPOINT_REC_KERNEL) {
        return CHECKPOINT_OK;
    }

    /* Page record: ensure the process has an address space, then map it. */
    if (!e->space) {
        e->space = vmm_create_address_space(rec->pid);
        if (!e->space) {
            return CHECKPOINT_ERR_PARAM;
        }
    }

    uint64_t phys = vmm_alloc_page();
    if (!phys) {
        return CHECKPOINT_ERR_PARAM;
    }

    /* Physical frames are directly addressable in the kernel (same convention
     * vmm.c uses); load the checkpointed page contents into the new frame. */
    memcpy((void*)phys, rec->page_data, PAGE_SIZE);

    /* Read-only pages (code) are re-mapped without write permission so they keep
     * their original protection; writable pages get PAGE_WRITABLE. */
    uint32_t page_flags = PAGE_PRESENT | PAGE_USER;
    if (!(rec->flags & CHECKPOINT_REC_READONLY)) {
        page_flags |= PAGE_WRITABLE;
    }
    if (vmm_map_page(e->space, rec->virt_addr, phys, page_flags) != 0) {
        return CHECKPOINT_ERR_PARAM;
    }
    return CHECKPOINT_OK;
}

/* Kernel registrar: put each reconstructed process into the process table with
 * its restored address space and CPU context, mark it runnable, and hand it to
 * the scheduler. */
extern int scheduler_add_process(process_t* proc);

static int checkpoint_register_kernel(void* reg_ctx, const checkpoint_restored_process_t* rp) {
    (void)reg_ctx;

    process_t* proc = process_get_by_pid((pid_t)rp->pid);
    if (!proc) {
        proc = process_create("restored", "");
        if (!proc) {
            return CHECKPOINT_ERR_PARAM;
        }
        proc->pid = (pid_t)rp->pid;
        pm_table_add_process(proc);
    }

    if (rp->space) {
        proc->address_space = rp->space;
    }
    if (rp->has_context) {
        uint32_t n = rp->context_size < sizeof(proc->context)
                         ? rp->context_size : (uint32_t)sizeof(proc->context);
        memcpy(&proc->context, rp->context, n);
    }

    /* Sever external / non-persistable handles (sockets, DMA, devices). Their
     * kind is tagged in each fd's flags; severed descriptors return a clean
     * error so the app re-establishes them (#118, #128). Regular files survive. */
    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        if (proc->fds[i].fd >= 0) {
            extstate_sever_fd(&proc->fds[i].flags);
        }
    }

    proc->state = PROCESS_STATE_READY;
    scheduler_add_process(proc); /* process->scheduler bridge */
    return CHECKPOINT_OK;
}

int checkpoint_restore_boot(snapshot_store_t* store) {
    checkpoint_restore_ctx_t ctx;
    ctx.count = 0;

    int restored = checkpoint_restore(store, checkpoint_restore_apply_kernel, &ctx);
    if (restored < 0) {
        return restored; /* CHECKPOINT_ERR_NO_CHECKPOINT or another error */
    }

    /* Register every reconstructed process (table + scheduler + context). */
    checkpoint_finalize_restore(ctx.procs, ctx.count, checkpoint_register_kernel, 0);
    return restored;
}

/* ----- Boot-store registration ----- */

static snapshot_store_t* g_boot_store = 0;

void checkpoint_set_boot_store(snapshot_store_t* store) {
    g_boot_store = store;
}

int checkpoint_boot(void) {
    if (!g_boot_store) {
        return CHECKPOINT_ERR_NO_CHECKPOINT; /* nothing configured: cold boot */
    }
    return checkpoint_restore_boot(g_boot_store);
}

int checkpoint_persistence_init(snapshot_store_t* store, fat_block_device_t* dev,
                                uint32_t base_sector, uint32_t slot_sectors,
                                uint64_t interval_ticks) {
    if (!store || !dev) {
        return CHECKPOINT_ERR_PARAM; /* no device: persistence stays disabled */
    }

    if (snapshot_store_init(store, dev, base_sector, slot_sectors) != SNAPSHOT_OK) {
        return CHECKPOINT_ERR_IO;
    }

    /* Format only if the store holds no valid checkpoint, so an existing
     * checkpoint survives a reboot and gets restored. */
    snapshot_reader_t probe;
    if (snapshot_store_load(store, &probe) != SNAPSHOT_OK) {
        if (snapshot_store_format(store) != SNAPSHOT_OK) {
            return CHECKPOINT_ERR_IO;
        }
    }

    checkpoint_set_boot_store(store);
    checkpoint_timer_configure(true, interval_ticks);
    return CHECKPOINT_OK;
}

/* ----- Take ----- */

uint64_t checkpoint_take(void) {
    uint64_t epoch = g_checkpoint.current_epoch + 1;
    uint64_t spaces = 0;
    uint64_t pages = 0;

    /* Enumerate live user processes and mark each one's address space. The
     * kernel space (pid 0) is out of scope for v1 persistence. */
    uint32_t pids[PM_MAX_PROCESSES];
    uint32_t count = 0;
    if (pm_get_process_list(pids, PM_MAX_PROCESSES, &count) != 0) {
        return 0;
    }

    for (uint32_t i = 0; i < count; i++) {
        process_t* proc = pm_get_process(pids[i]);
        if (!proc || !proc->address_space) {
            continue;
        }
        int marked = checkpoint_mark_space(proc->address_space, epoch);
        if (marked < 0) {
            continue;
        }
        spaces++;
        pages += (uint64_t)marked;

        /* Snapshot the process's CPU context now, at the consistent checkpoint
         * point. Unlike pages it is not copy-on-write, so it is captured eagerly
         * (it is tiny); the writeback pass streams it to disk with the pages. */
        checkpoint_capture_context(proc->pid, &proc->context, sizeof(proc->context));
    }

    g_checkpoint.current_epoch = epoch;
    g_checkpoint.spaces_marked = spaces;
    g_checkpoint.pages_marked = pages;
    g_checkpoint.total_takes++;
    g_checkpoint.epoch_open = true;
    return epoch;
}

/* ----- Periodic trigger ----- */

void checkpoint_timer_configure(bool enabled, uint64_t interval_ticks) {
    g_timer_enabled = enabled;
    g_timer_interval = interval_ticks ? interval_ticks : 1;
    g_timer_ticks = 0;
}

bool checkpoint_tick(void) {
    if (!g_timer_enabled) {
        return false;
    }

    g_timer_ticks++;
    if (g_timer_ticks < g_timer_interval) {
        return false;
    }

    /* Interval elapsed. If the previous checkpoint is still being written back,
     * don't overlap it: hold the counter at the threshold and retry next tick
     * (so the checkpoint isn't skipped entirely, just deferred). */
    if (g_checkpoint.epoch_open) {
        return false;
    }

    g_timer_ticks = 0;

    /* Arm the quiescent-point barrier rather than taking the checkpoint inline
     * (#138). A timer tick fires at a safe boundary (about to resume the
     * interrupted context) with no checkpoint lock held, so this CPU can park
     * immediately. On single-CPU IKOS that one park makes the system quiescent
     * and the checkpoint is taken here; under SMP the other CPUs park on their
     * own ticks and the last one to park takes it. */
    checkpoint_barrier_arm(&g_barrier);
    if (checkpoint_barrier_park(&g_barrier, 0 /* cpu */, true /* no lock held */)) {
        uint64_t epoch = checkpoint_take();
        checkpoint_barrier_release(&g_barrier);
        return epoch != 0;
    }
    return false;
}
