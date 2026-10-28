# IKOS Orthogonal Persistence v2 - Whole-Machine Persistence

> A design document from the IKOS era. The files it names outside
> `kernel/checkpoint*.c`, `kernel/snapshot_store.c`, and `kernel/ramdisk.c`
> (`kernel/scheduler.c`, `kernel/demand_paging.c`, `kernel/process_manager.c`,
> and others) were never part of the bootable kernel and were removed in
> #233; the booted kernel's persistence is described in the root README and
> `include/core/timetravel.h`.

Tracking issue: #132. Builds on the delivered v1 ([`orthogonal-persistence.md`](orthogonal-persistence.md), epic #121) and the v1.5 follow-ups (#126–#131).

> Write this before any v2 implementation. The consistency model below will
> reshape the work breakdown, so the sub-issue list at the end is a proposal to
> be split/merged once the model is agreed.

## 1. What v1 persists, and the v2 goal

**v1 (shipped):** user address spaces. A checkpoint marks writable user pages
copy-on-write, the page-fault hook preserves pre-checkpoint contents on first
write, a background pass streams captured + clean + read-only pages to a
double-buffered, CRC-protected on-disk store, and on boot the kernel restores
the latest valid checkpoint (reconstructing address spaces, process-table
entries, and CPU context) or cold-boots. **The kernel and all drivers
re-initialize from scratch every boot;** only user state is replayed on top.

**v2 goal:** persist the **whole machine** - kernel-internal state, driver and
device state, and in-flight IPC/DMA - so that after a power cut the system
resumes not just user memory but the complete OS, ideally with no observable
discontinuity beyond severed external connections.

This is the step from "a clever COW trick on user memory" to "the entire OS is
designed around quiescent checkpointing." It touches every subsystem and needs
a global consistency model. v1 proves the thesis and the mechanism; v2 is the
larger effort to make the machine, not just processes, survive the plug.

## 2. The central problem: consistency

You cannot snapshot the kernel at an arbitrary instant and expect a coherent
restore. At a random point the kernel may be mid-syscall, mid-context-switch,
holding a spinlock, halfway through updating a linked list, or with a device
mid-DMA. A checkpoint taken there captures a torn invariant that crashes on
restore.

v1 sidesteps this: it only persists *user* pages (captured lazily via COW) and
cold-inits the kernel, so kernel invariants are never snapshotted. v2 must
persist kernel state, so it needs a **defined point where the kernel is
self-consistent** - a checkpoint barrier.

### 2.1 Quiescent points (the checkpoint barrier)

Define a checkpoint barrier: a point at which a single CPU holds no kernel
locks, no syscall is in progress on any CPU, no context switch is underway, and
device queues are at a known boundary. Candidate strategy for IKOS:

- **Drive checkpoints from `scheduler_tick()`** (as v1's trigger already does,
  `kernel/scheduler.c`), but only *arm* a checkpoint there; do not take it
  inline.
- **Quiesce:** raise a "checkpoint pending" flag. Each CPU, on returning from
  its next interrupt/syscall to a safe boundary (about to resume user mode, or
  in the idle loop), parks at a barrier. When all CPUs are parked and no lock is
  held, the system is quiescent.
- **Take** the kernel snapshot at that barrier, then release the CPUs.

For the current single-CPU IKOS this simplifies to "take the checkpoint from the
idle loop or just before returning to user mode, with interrupts masked and no
lock held." The barrier abstraction keeps the door open for SMP.

Open question to resolve in implementation: bounded barrier latency. A CPU
blocked deep in a driver must reach the barrier promptly; long-running
kernel operations may need explicit yield/checkpoint-safe points.

## 3. Kernel-internal state

Decide, per subsystem, **persist vs. cold-init-and-reconcile**. Default to
cold-init unless a structure carries irreplaceable state.

| Kernel structure | Source | v2 disposition |
|------------------|--------|----------------|
| Process table | `kernel/process_manager.c` (`pm_table_*`) | **Persist** - v1 already reconstructs entries; extend to full table incl. parent/child, zombies, wait queues |
| Scheduler queues | `kernel/scheduler.c` (`ready_queues`, `round_robin_queue`) | **Persist** run/ready state; rebuild queue links on restore from process state |
| CPU context per process | `process_context_t` | **Persist** (done in v1.5, #126) |
| Virtual memory regions | `vm_space_t` region lists | **Persist** (region metadata persisted; pages already persisted) |
| Open-file tables / VFS | `proc->fds[]`, VFS state | **Persist** fd table + offsets; reopen backing files by path (FS is persistent); see §5 |
| IPC channels + queues | `pm_ipc_*`, `kernel/daemon_ipc.c` | **Drain or persist** in-flight messages; see §6 |
| Kernel heap (`kalloc`) | `kernel/kalloc.c` | **Cold-init**; persisted structures are re-allocated/relinked on restore, or the heap is snapshotted wholesale (decision below) |
| Timers / alarms | `proc->alarm_time`, scheduler ticks | **Persist** logical deadlines; re-arm hardware timer on restore |
| Locks / spinlocks | various | **Never persist** - the barrier guarantees none are held at checkpoint time |

### 3.1 Two strategies for kernel memory

- **(A) Wholesale kernel-image snapshot.** Treat kernel data the same as user
  pages: COW-mark and persist kernel writable pages at the barrier, restore the
  kernel image verbatim, then fix up only the non-persistable bits (device
  registers, in-flight DMA). Simplest conceptually; largest checkpoints; couples
  restore to the exact kernel build (a kernel upgrade invalidates checkpoints).
- **(B) Structured serialization.** Serialize specific kernel structures
  (process table, scheduler, IPC, VFS) into versioned records, cold-init the
  kernel on restore, and rebuild structures from the records. More work and a
  schema to maintain, but survives kernel upgrades and keeps checkpoints small.

**Recommendation:** start with (B) for the small set of structures that carry
real state (process table, scheduler, fds, IPC), since v1 already does (B)-style
reconstruction for address spaces and contexts. Reserve (A) as a fallback if the
serialization surface grows unmanageable.

## 4. Driver / device re-attach protocol

A persisted checkpoint that references device-register state is meaningless
after a power cut: the hardware reset. Each driver must re-initialize hardware
on restore and reconcile it with the persisted *logical* state.

Define a driver contract (extend `device_t` / the device manager,
`kernel/device_manager.c`):

```
struct persist_ops {
    int  (*quiesce)(device_t*);   /* reach a safe boundary before checkpoint */
    int  (*reattach)(device_t*);  /* re-init hardware on restore, reconcile */
    bool persistable;             /* false => connections severed (see §6) */
};
```

- **`quiesce`** - called at the checkpoint barrier: finish or abort in-flight
  transfers, stop DMA, leave the device at a known boundary.
- **`reattach`** - called on restore after cold-init: re-probe and re-program
  the hardware, then reconcile with the persisted logical state.

Per-driver responsibilities (most visible in a demo first):

- **Disk (IDE, `kernel/ide_driver.c`):** re-probe drives, re-map the checkpoint
  store region. The store itself is already crash-consistent (v1). The
  `checkpoint_ide` adapter (#130) is the durable backing.
- **Framebuffer (`kernel/framebuffer.c`):** re-init the video mode on restore
  and **repaint from the persisted pixel buffer** - the most visceral "it came
  back" demo.
- **Input (keyboard/mouse):** cold-init; no meaningful persisted state.
- **Network/audio:** non-persistable (see §6).

## 5. VFS and open files

Files are the easy case: their bytes live in the persistent filesystem, so
v2 persists the fd table (number, offset, flags, path/inode ref) and, on
restore, reopens each backing file by path and seeks to the saved offset. This
generalizes v1.5's external-state policy (#118/#128), where regular-file fds
already survive while sockets/devices are severed. Edge cases to resolve:
unlinked-but-open files, pipes (both ends in the checkpoint vs. one end gone),
and FS changes between checkpoint and restore.

## 6. In-flight IPC and DMA

Messages mid-delivery and DMA transfers mid-flight cannot be replayed. Two
options, chosen per resource:

- **Drain to a quiescent point** before checkpointing (preferred for IPC):
  the barrier (§2.1) lets message queues settle; persist the settled queues as
  ordinary kernel state (§3). `pm_ipc_*` queues become serializable.
- **Mark severed** (preferred for DMA and external connections): extend the
  v1.5 external-state policy (`checkpoint_extstate`, #118/#128) *inward* - DMA
  buffers, sockets, and device handles are tagged non-persistable and surface a
  defined error on restore so the owner re-establishes them. The fd-flags
  severing layer already exists; v2 adds DMA/IPC-endpoint kinds and the
  reconnection contract.

## 7. Layering on v1

v2 reuses, not replaces, the v1 machinery:

- The **on-disk store** (`snapshot_store`, double-buffered slots + CRC) gains a
  new record type for kernel-state blobs alongside page and context records -
  exactly how #126 added context records without changing the format.
- The **writeback / restore pipeline** gains a kernel-state pass that runs at
  the barrier (write) and before user restore (read).
- The **boot path** restores kernel state first, re-attaches drivers (§4), then
  restores user spaces/processes on top (v1 order, now with live kernel state).

## 8. Risks / open questions

- **Barrier latency** under long kernel operations (§2.1).
- **Kernel-upgrade compatibility** of checkpoints - strategy (B) mitigates;
  add a kernel-build/version stamp to the superblock and reject mismatches.
- **Partial driver re-attach failure** - define whether a failed `reattach`
  aborts the restore (cold-boot fallback) or proceeds with that device severed.
- **Testing without hardware** - most of v2 needs a booting kernel + QEMU, which
  the current CI environment lacks; the decision cores (barrier state machine,
  serialization, reconcile logic) should be factored as pure, host-testable
  units like the v1 work, with QEMU reserved for integration.

## 9. Proposed sub-issue breakdown

To be split/merged after review of §2–§3:

1. **Checkpoint barrier / quiescent-point state machine** (§2.1) - pure state
   machine + scheduler integration; host-testable.
2. **Kernel-state record type in the snapshot store** (§7) - new record kind +
   superblock kernel-version stamp.
3. **Persist + restore the process table and scheduler queues** (§3).
4. **Persist + restore the VFS open-file tables** (§5).
5. **IPC drain + persist in-flight messages** (§6).
6. **Driver re-attach framework** (`persist_ops` on `device_t`, §4).
7. **Disk driver re-attach** (§4).
8. **Framebuffer re-attach + repaint-from-persisted-buffer** (§4) - the demo.
9. **Extend external-state severing to DMA/IPC endpoints** (§6).
10. **v2 boot ordering: kernel-state restore -> driver re-attach -> user restore**
    (§7).

## 10. Definition of done (design phase)

This document is accepted once it has a reviewed position on (a) the quiescent
barrier, (b) kernel-state scope and the (A) vs (B) strategy, (c) the driver
re-attach contract, and (d) in-flight IPC/DMA handling, with the sub-issue list
above split into tracked issues.
