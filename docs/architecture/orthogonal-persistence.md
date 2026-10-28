# IKOS Orthogonal Persistence - Design Doc

> A design document from the IKOS era. The files it names outside
> `kernel/checkpoint*.c`, `kernel/snapshot_store.c`, and `kernel/ramdisk.c`
> (`kernel/scheduler.c`, `kernel/demand_paging.c`, `kernel/process_manager.c`,
> and others) were never part of the bootable kernel and were removed in
> #233; the booted kernel's persistence is described in the root README and
> `include/core/timetravel.h`.

Tracking issue: #110 (epic: #121)

## Overview

Orthogonal persistence means the running state of the system - every process,
every open region of memory, mid-instruction - is the durable state. There is
no explicit save and no explicit load. A power cut followed by a reboot
resumes execution from the last completed checkpoint instead of starting
cold.

This is not a new idea (KeyKOS, EROS/CapROS, Phantom OS, IBM i's
single-level store), but no hobby-tier microkernel implements it. IKOS's
existing virtual memory manager already provides the two primitives a
checkpoint scheme needs - per-process address spaces and a working
copy-on-write engine - so this design adds a snapshot *policy* on top of the
VMM rather than replacing any of it.

## Mechanism

A checkpoint has two phases:

1. **`checkpoint_take()` - stop-the-world, O(address spaces) not O(memory).**
   Walk every live `vm_space_t` and mark every currently-writable page
   read-only, tagged `PAGE_SNAPSHOT_COW` (a new software PTE flag, distinct
   from the existing fork `VMM_FLAG_COW`). Record the new checkpoint epoch.
   Return. No page is copied during this phase, so the pause is bounded by
   the number of page tables, not the amount of RAM in use.

2. **`checkpoint_writeback()` - background, off the critical path.**
   A kernel-side pass streams the pages belonging to the just-closed epoch
   into the on-disk snapshot store. Pages that processes write to *after*
   `checkpoint_take()` are captured by the page-fault hook (below) before
   the write is allowed to proceed, so the writeback pass and ongoing
   process execution never race on the same page's pre-checkpoint contents.

This reuses the existing COW fault path: `vmm_handle_cow_fault()`
(`kernel/vmm_cow.c`) and the refcounted page tracker in
`kernel/copy_on_write.c` already implement "first write to a shared page
copies it first." Snapshot-COW adds one more consumer of that mechanism:
instead of (or in addition to) giving the writer a private copy for fork
semantics, the *original* contents are first persisted to the snapshot log.

The page-fault hook lives in `handle_page_fault()`
(`kernel/demand_paging.c:711`): if the faulting page is tagged
`PAGE_SNAPSHOT_COW` for the active epoch, copy its current contents into the
snapshot log, then fall through to normal COW resolution.

## On-disk snapshot format

Reserve a region of the block device (`kernel/ide_driver.c` for real disk,
`kernel/ramdisk.c` for the no-hardware demo) as the **checkpoint store**:

```
+----------------+----------------+----------------+
|   Superblock   |   Slot A       |   Slot B        |
+----------------+----------------+----------------+
```

- **Superblock**: a single sector holding `{ active_slot, epoch, crc32 }`.
  This is the one piece of state that must be updated atomically.
- **Slot A / Slot B**: each holds one complete checkpoint as a log of
  `(epoch, pid, virt_addr) -> page` records, terminated by a slot-level CRC.

### Crash consistency

Checkpoints are double-buffered: `checkpoint_writeback()` always writes to
the *inactive* slot, never the one the superblock currently points at.
Finalizing a checkpoint is:

1. Write all page records for this epoch into the inactive slot.
2. Compute and write the slot CRC.
3. Overwrite the superblock with `{ that slot, this epoch, crc32(superblock) }`.

A crash at any point before step 3 completes leaves the superblock pointing
at the previous, fully-written slot - so the system always boots into the
last *complete* checkpoint, never a half-written one. A CRC mismatch on the
slot the superblock names is treated as "no valid checkpoint" and the system
falls back to a cold boot.

## Restore path

On boot (`kernel/kernel_main` / `boot/`), before scheduling starts:

1. Read the superblock. If its own CRC is invalid, cold-boot as today.
2. Validate the named slot's CRC. If invalid, cold-boot as today.
3. Otherwise, call `checkpoint_restore()`: recreate each saved `vm_space_t`,
   load its pages into freshly allocated frames, rebuild page tables, and
   restore the corresponding process-table / scheduler entries.
4. Apply the external-state policy (below) to anything that doesn't survive
   a checkpoint, then resume scheduling.

## External / non-persistable state

Some state cannot be meaningfully replayed across a power cycle: open
network sockets, in-flight DMA buffers, device register state. The policy:

- Resources that hold this kind of state are tagged
  "does-not-survive-checkpoint" at the type level (e.g. socket descriptors,
  DMA-mapped buffers).
- At checkpoint time these resources are **not** specially serialized - only
  the fact that a process held a handle to one is preserved as part of its
  normal memory image (the handle/descriptor still exists as a value in the
  process's address space).
- At restore time, any such handle is marked **severed**: subsequent use
  returns a clean, well-defined error (e.g. `ECONNRESET`-equivalent for a
  socket) instead of touching now-invalid hardware/network state. The
  application is responsible for detecting the error and re-establishing
  the resource (reconnect, remap DMA, etc.) - this is the same contract
  applications already need for ordinary connection loss.
- Device drivers themselves are **not** persisted in v1 (see Scope below);
  they re-initialize from cold state on every boot, restored or not.

## Scope for v1

To keep the first implementation tractable:

- **In scope:** user-space `vm_space_t` address spaces and the process
  table entries needed to resume those processes (registers, scheduler
  state, open-handle values as opaque data).
- **Out of scope (v2+):** persisting kernel-internal state, persisting
  driver/device state, persisting in-flight IPC messages. On every boot
  - restored or cold - the kernel and its drivers re-initialize fresh, then
  user processes are restored on top.

This means the v1 guarantee is precisely: *user processes and their memory
survive a power cycle; kernel and devices do not need to be replayed because
they're stateless from the process's point of view (modulo the severed-handle
contract above).*

## New/changed interfaces (summary)

- `include/vmm.h`: new `PAGE_SNAPSHOT_COW` PTE flag bit; `vm_space_t` gains
  `checkpoint_epoch` and a reference into the snapshot map.
- New `kernel/checkpoint.c` / `include/checkpoint.h`: `checkpoint_take()`,
  `checkpoint_writeback()`, `checkpoint_restore()`, snapshot-store read/write
  helpers.
- `kernel/demand_paging.c`: one new branch in `handle_page_fault()`.
- `kernel/scheduler.c`: a periodic timer/idle hook calling
  `checkpoint_take()`.
- Boot path: a check-and-restore step before normal cold init.

## Definition of done for the design phase

This document is accepted once it covers (a) the snapshot format, (b) the
crash-consistency argument, and (c) the external-state policy, ahead of any
implementation issue (#111 onward) landing code that depends on it.
