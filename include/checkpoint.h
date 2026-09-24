/* IKOS Orthogonal Persistence - Checkpoint Engine
 *
 * The checkpoint engine implements the "stop the world for ~1ms" snapshot
 * described in docs/architecture/orthogonal-persistence.md.
 *
 * checkpoint_take() walks every live user address space and marks each
 * currently-writable page read-only + PAGE_SNAPSHOT_COW, then bumps the
 * global checkpoint epoch and returns. NO page is copied during this phase;
 * pages are captured lazily by the page-fault hook (#113) on first write, and
 * streamed to disk by the background writeback pass (#115).
 *
 * Issue #112 (epic #121).
 */

#ifndef CHECKPOINT_H
#define CHECKPOINT_H

#include <stdint.h>
#include <stdbool.h>
#include "vmm.h"            /* vm_space_t, pte_t, PAGE_* flags */
#include "snapshot_store.h" /* snapshot_store_t (writeback target) */

/* Error codes (negative); non-negative returns are counts/epochs. */
#define CHECKPOINT_OK             0
#define CHECKPOINT_ERR_PARAM     -1
#define CHECKPOINT_ERR_STATE     -2
#define CHECKPOINT_ERR_IO        -3
#define CHECKPOINT_ERR_NO_CHECKPOINT -4  /* no valid checkpoint on disk: cold boot */

/* Engine state, observable for tests, stats, and the writeback pass. */
typedef struct {
    uint64_t current_epoch;   /* epoch of the most recent checkpoint_take() */
    uint64_t spaces_marked;   /* address spaces marked in the last take */
    uint64_t pages_marked;    /* pages flipped to snapshot-COW in the last take */
    uint64_t total_takes;     /* lifetime count of checkpoints taken */
    bool     epoch_open;      /* an epoch has been taken but not yet written back */
} checkpoint_state_t;

/* Initialize the engine (epoch 0, nothing open). */
void checkpoint_init(void);

/* The epoch of the most recent checkpoint (0 before the first take). */
uint64_t checkpoint_current_epoch(void);

/* Continue numbering from `epoch` with no epoch open and no captures, after
 * the machine was rebuilt from that keyframe (#224, #231). */
void checkpoint_set_epoch(uint64_t epoch);

/* Read-only view of engine state. */
const checkpoint_state_t* checkpoint_get_state(void);

/* Pure marking primitive — the policy core, exposed for unit testing and for
 * reuse by the page-fault hook (#113).
 *
 * If the page is present AND writable, clear PAGE_WRITABLE, set
 * PAGE_SNAPSHOT_COW, and return true (the page now faults on write).
 * Otherwise leave *pte unchanged and return false. Never copies anything. */
bool checkpoint_mark_pte(pte_t* pte);

/* Mark every currently-writable page in one address space as snapshot-COW for
 * the given epoch and record the epoch on the space. Returns the number of
 * pages marked, or a negative CHECKPOINT_ERR_* code. Copies no page. */
int checkpoint_mark_space(vm_space_t* space, uint64_t epoch);

/* Inverse of checkpoint_mark_pte(): clear PAGE_SNAPSHOT_COW and restore
 * PAGE_WRITABLE so the page is writable again. Returns true if the page was a
 * snapshot-COW page (and was thus resolved), false otherwise. Pure; exposed
 * for unit testing. */
bool checkpoint_resolve_pte(pte_t* pte);

/* ----- Captured pages (the in-memory "snapshot log") -----
 *
 * The page-fault hook copies a page's pre-write contents into a capture record
 * before un-protecting it. The background writeback pass (#115) drains these
 * records to the on-disk snapshot store, then calls checkpoint_clear_captures(). */
typedef struct checkpoint_capture {
    uint64_t epoch;       /* checkpoint epoch this page belongs to */
    uint32_t pid;         /* owning process */
    uint32_t flags;       /* reserved / region flags */
    uint64_t virt_addr;   /* page-aligned virtual address */
    void*    data;        /* PAGE_SIZE copy of the pre-write contents */
    struct checkpoint_capture* next;
} checkpoint_capture_t;

/* Preserve a snapshot-COW page: copy page_contents into a new capture record
 * tagged with the current epoch, then resolve *pte back to writable. Returns
 * CHECKPOINT_OK on capture, CHECKPOINT_ERR_STATE if the PTE is not snapshot-COW,
 * or CHECKPOINT_ERR_PARAM / a negative code on bad input or allocation failure.
 * Exposed for unit testing; normally reached via checkpoint_handle_write_fault(). */
int checkpoint_capture_page(uint32_t pid, uint64_t virt_addr,
                            const void* page_contents, pte_t* pte);

/* Capture-record flag (carried in snapshot_page_record_t.flags) marking a
 * record as a process CPU context blob rather than an address-space page. */
#define CHECKPOINT_REC_CONTEXT  0x1
/* Record flag marking a page that was read-only at checkpoint time (e.g. code).
 * Restore maps it read-only (no PAGE_WRITABLE) so it keeps its permissions. */
#define CHECKPOINT_REC_READONLY 0x2
/* Record flag marking a serialized kernel-state blob chunk (v2, #139), not an
 * address-space page. Restore hands these to the kernel-state reassembler
 * (process table, VFS, IPC, ...; #140+) rather than mapping them as memory. */
#define CHECKPOINT_REC_KERNEL   0x4

/* What writeback should do with one page of an address space, given whether its
 * region is writable and its PTE. The decision core of #131, exposed for tests. */
typedef enum {
    CHECKPOINT_PAGE_SKIP       = 0, /* not present, or modified (already captured) */
    CHECKPOINT_PAGE_PERSIST_RW = 1, /* clean writable page: persist, map writable */
    CHECKPOINT_PAGE_PERSIST_RO = 2, /* read-only page (code): persist, map read-only */
} checkpoint_page_action_t;

/* Decide how to persist a page during writeback:
 *  - not present                       -> SKIP
 *  - writable region + snapshot-COW set -> PERSIST_RW (clean, unmodified)
 *  - writable region + tag cleared      -> SKIP (modified: streamed via its capture)
 *  - read-only region                   -> PERSIST_RO (never changes; persist directly)
 * Pure; no side effects. */
checkpoint_page_action_t checkpoint_page_action(bool region_writable, pte_t pte);
/* Sentinel virt_addr stamped on context records (records are distinguished by
 * the flag above; this is for debuggability). */
#define CHECKPOINT_CONTEXT_VADDR 0xFFFFFFFFFFFFF000ULL

/* Persist a process's CPU context (process_context_t) as a checkpoint record:
 * copy ctx_size bytes (<= PAGE_SIZE) into a new, context-flagged capture record
 * tagged with the current epoch. The writeback pass streams it like any other
 * record; restore recognizes it by CHECKPOINT_REC_CONTEXT and reloads it into
 * the reconstructed process (the latter in #127). Returns CHECKPOINT_OK or a
 * negative code. */
int checkpoint_capture_context(uint32_t pid, const void* ctx, uint32_t ctx_size);

/* Persist an arbitrary-size kernel-state blob (v2, #139) as one or more
 * CHECKPOINT_REC_KERNEL records, chunked into pages and tagged with `tag` (which
 * kernel structure it is). Each record encodes the total size and chunk index in
 * its virt_addr field ((size << 32) | index) so the restore side can reassemble
 * it. The writeback pass streams the records like any other. Returns
 * CHECKPOINT_OK, or a negative code on bad input / allocation failure. */
int checkpoint_capture_kernel_blob(uint32_t tag, const void* data, uint32_t size);

/* Page-fault hook entry point. If fault_addr in `space` is a present,
 * snapshot-COW page, capture its current contents (only while an epoch is open,
 * that is, before its writeback commits), restore writability, flush the TLB,
 * and return true (the faulting write may now proceed). Returns false if the
 * page is not a snapshot-COW page (the fault is someone else's). The page is
 * read at fault_addr, so `space` must be the current address space. */
bool checkpoint_handle_write_fault(vm_space_t* space, uint64_t fault_addr);

/* Head of the capture list (most-recent-first), its length, and a routine to
 * free every capture record. Used by the writeback pass (#115). */
checkpoint_capture_t* checkpoint_captures(void);
uint64_t checkpoint_capture_count(void);
void checkpoint_clear_captures(void);

/* ----- Writeback (#115) -----
 *
 * Stream the current (open) checkpoint to the on-disk store, off the
 * stop-the-world path. Persists, into the store's inactive slot:
 *   - every captured page (the pre-checkpoint image preserved by the hook), and
 *   - every still-clean snapshot-COW page in live user spaces (its frame still
 *     holds the checkpoint-time content), read live at writeback time.
 * Modified pages are not double-written: the hook cleared their PAGE_SNAPSHOT_COW
 * tag, so the clean-page walk skips them. snapshot_store_commit() flips the
 * superblock (the single commit point). On success the capture list is freed
 * and the epoch is closed. Returns CHECKPOINT_OK or a negative code. */
int checkpoint_writeback(snapshot_store_t* store);

/* Stream the current epoch's pages (captured images + still-clean pages) into
 * an already-open snapshot writer. Shared by checkpoint_writeback and the
 * keyframe-ring writeback path (#195), which open the writer on a ring-selected
 * region rather than the two-slot store. Returns CHECKPOINT_OK or a negative
 * code; does not commit. */
int checkpoint_stream_pages(snapshot_writer_t* writer);

/* Finish an epoch once its checkpoint has durably committed: run the post-commit
 * journal hook, drop the in-memory capture log, and close the epoch. Called by
 * both writeback paths after their respective commit. */
void checkpoint_after_commit(uint64_t epoch);

/* Post-commit journal hook (#194). After a checkpoint's writeback has durably
 * committed, the engine calls this hook (when set) with the committed epoch, so
 * the deterministic-replay journal for that epoch can be persisted alongside
 * the checkpoint. Injected as a function pointer to keep the checkpoint core
 * free of the journal / record-subsystem dependencies. NULL by default (no
 * journaling). The return value is advisory: the checkpoint is already
 * committed, so a hook failure does not undo or fail the checkpoint. */
typedef int (*checkpoint_journal_hook_fn)(uint64_t epoch);
void checkpoint_set_journal_hook(checkpoint_journal_hook_fn hook);

/* ----- Restore (#116) -----
 *
 * Called once for each page of the loaded checkpoint, in slot order. The
 * record's page_data points at a reusable buffer owned by the restore loop;
 * copy out anything that must outlive the call. Return CHECKPOINT_OK to
 * continue or a negative code to abort the restore. */
typedef int (*checkpoint_apply_fn)(void* ctx, const snapshot_page_record_t* rec);

/* Load the latest valid checkpoint and replay every page through apply().
 * Restores the global epoch to the checkpoint's epoch on success. Returns the
 * number of pages restored (>= 0), CHECKPOINT_ERR_NO_CHECKPOINT if the store
 * holds no valid checkpoint (the caller should cold-boot), or another negative
 * code on error. Pure orchestration: all VMM/process work happens in apply(). */
int checkpoint_restore(snapshot_store_t* store, checkpoint_apply_fn apply, void* ctx);

/* Max bytes of saved CPU context carried per reconstructed process (a
 * process_context_t is ~164 bytes; this leaves headroom). */
#define CHECKPOINT_CONTEXT_MAX 256

/* One process reassembled from a checkpoint: its address space, its saved CPU
 * context (if a context record was present), and its pid. Built up as records
 * are replayed, then handed to a registration callback (#127). */
typedef struct {
    uint32_t    pid;
    vm_space_t* space;                        /* NULL if the process had no pages */
    uint8_t     context[CHECKPOINT_CONTEXT_MAX];
    uint32_t    context_size;                 /* bytes of context actually saved */
    bool        has_context;
} checkpoint_restored_process_t;

/* Called once per reconstructed process to register it (process table +
 * scheduler) and apply its saved context. Return CHECKPOINT_OK to continue. */
typedef int (*checkpoint_register_fn)(void* reg_ctx, const checkpoint_restored_process_t* proc);

/* Invoke `reg` for each reconstructed process. Returns the number registered
 * (>= 0), or a negative code if a callback fails. Pure iteration: the kernel
 * vs. test registration logic lives entirely in `reg`. */
int checkpoint_finalize_restore(const checkpoint_restored_process_t* procs, int count,
                                checkpoint_register_fn reg, void* reg_ctx);

/* Boot adapter: run checkpoint_restore() with the built-in kernel apply (which
 * recreates an address space per pid, maps each restored page, and collects
 * each process's saved context), then checkpoint_finalize_restore() with the
 * built-in kernel registrar (process table + scheduler + context apply).
 * Returns the number of pages restored (>= 0) or CHECKPOINT_ERR_NO_CHECKPOINT. */
int checkpoint_restore_boot(snapshot_store_t* store);

/* Register the store the boot path restores from. Until one is set,
 * checkpoint_boot() reports "no checkpoint" and the kernel cold-boots. */
void checkpoint_set_boot_store(snapshot_store_t* store);

/* Boot-time entry point invoked from kernel_init(): restore from the
 * registered boot store if one is configured, else signal a cold boot.
 * Returns pages restored (>= 0) or CHECKPOINT_ERR_NO_CHECKPOINT. */
int checkpoint_boot(void);

/* v2 whole-machine boot restore (#147): runs the ordered restore (kernel state,
 * driver re-attach, user processes) with a cold-boot fallback via the pure
 * sequencer in checkpoint_restore_seq.c. Returns CHECKPOINT_OK if the machine
 * resumed, or CHECKPOINT_ERR_NO_CHECKPOINT to continue a cold boot. Defined in
 * checkpoint_boot_v2.c. */
int checkpoint_boot_v2(snapshot_store_t* store);

/* ----- Periodic trigger (#117) ----- */

/* Default checkpoint cadence, in timer ticks. The scheduler tick rate
 * (TIMER_FREQUENCY) determines the wall-clock interval; callers can override. */
#define CHECKPOINT_DEFAULT_INTERVAL_TICKS 1000

/* Enable/disable automatic checkpoints and set the interval (in timer ticks).
 * interval_ticks of 0 is treated as 1 (checkpoint every tick). */
void checkpoint_timer_configure(bool enabled, uint64_t interval_ticks);

/* Call once per timer tick (from scheduler_tick()). When enabled and the
 * interval has elapsed, takes a checkpoint, but only if the previous one has
 * finished writeback (epoch_open == false), so at most one checkpoint is ever
 * in flight. Returns true if a checkpoint was taken on this tick. */
bool checkpoint_tick(void);

/* ----- Boot wiring (#119) ----- */

/* Default checkpoint-store geometry on the backing device. */
#define CHECKPOINT_STORE_BASE_SECTOR   0
#define CHECKPOINT_STORE_SLOT_SECTORS  256

/* Wire orthogonal persistence to a block device at boot: bind a snapshot store
 * to `dev`, format it if it holds no valid checkpoint yet, register it as the
 * boot store (so checkpoint_boot() restores from it), and enable the periodic
 * trigger at the given cadence. Passing dev == NULL leaves persistence disabled
 * (the kernel cold-boots), so callers can wire this unconditionally. Returns
 * CHECKPOINT_OK when persistence is armed, or a negative code. */
int checkpoint_persistence_init(snapshot_store_t* store, fat_block_device_t* dev,
                                uint32_t base_sector, uint32_t slot_sectors,
                                uint64_t interval_ticks);

/* Take a checkpoint: bump the epoch, mark every live user address space, and
 * return the new epoch. Returns 0 on failure. Does no disk I/O and copies no
 * page — the bounded pause is O(mapped pages walked), not O(RAM touched). */
uint64_t checkpoint_take(void);

#endif /* CHECKPOINT_H */
