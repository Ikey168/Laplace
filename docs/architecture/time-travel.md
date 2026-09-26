# Time-Travel Debugging: Deterministic Replay Core and MCP Interface

This document is the design home for turning IKOS from a system that remembers the last
moment into one that remembers every moment and lets you move through them. It covers two
plans:

1. The **Deterministic Replay Core**: make execution between checkpoints reproducible.
2. The **MCP Interface**: expose record, rewind, and reverse execution as tools an AI
   agent can call.

Both build on the existing checkpoint engine (`kernel/checkpoint.c`,
`kernel/checkpoint_barrier.c`, the scheduler-tick trigger, and the restore and
reconstruct path). See [orthogonal-persistence.md](orthogonal-persistence.md) for that
foundation. Work is tracked in the epic and its sub-issues on the issue tracker.

Everything below runs in the booted kernel (`make run`, `make selftest`,
`tests/qemu/timetravel_e2e.py`). The sections after "Why this is tractable here"
are the original plan, kept for the reasoning behind it.

## How the booted machine records and replays

**The machine.** `build/laplace.elf` is a Multiboot kernel (QEMU `-kernel`, or
GRUB via `make iso`) with a small core in `kernel/core/`: paging with one
address space per process, ring-3 processes loaded from ELF images embedded in
the kernel (`user/laplace/`), `int 0x80` system calls, and a round-robin
scheduler. The kernel runs with interrupts disabled; only user mode takes the
timer. Everything the time-travel modules expect from the kernel (the
`vmm_*`, `pm_*`, and `process_*` calls the checkpoint engine makes) is
implemented over that core.

**Steps.** Every kernel entry a process makes (a system call or a fault) is one
step. A position `(epoch, step)` is the machine at the moment the step-th entry
of that epoch has trapped and not been handled; with an instruction index,
`(epoch, step, i)` for `i >= 1` is the moment entry `step` has been handled and
`i - 1` further user instructions have run (`include/core/machine.h`).

**Determinism.** A timer interrupt never changes user-visible state: it only
marks the time slice expired. Context switches happen only at kernel entries,
through `sched_record_decide()`, which records the step of every preemption on
a live run and forces the same switches on replay. This is what makes the
logical clock execution-derived; the original design counted timer ticks, which
land at a different instruction on every run. The clock (`SYS_TIME`) and
entropy (`SYS_RANDOM`) system calls go through `ktime_read()` and
`kentropy_fill()`. Everything else a program can observe is a function of its
memory and registers, so re-executing from a keyframe with the journal
reproduces the run exactly.

**Recording** (`kernel/core/timetravel.c`). At the first entry, and then
whenever the interval elapses or a record buffer fills, the kernel closes the
epoch (its journal goes to the journal ring) and takes a keyframe at that entry:
`checkpoint_take()` marks every writable user page copy-on-write and captures
registers, and a kernel-state blob captures the process table, each space's
regions, the run order, and the pending entry. Writeback to the keyframe store
runs at a later entry, so programs write in between and the COW hook preserves
the checkpoint-time images.

**Storage.** The first IDE disk (or a RAM disk) holds a label, the keyframe
store (a ring index plus N double-buffered regions), and the journal ring: one
journal per retained keyframe (`kernel/journal_ring.c`). Journal E holds the
inputs from keyframe E to keyframe E+1 (or to the stop point, for the newest
epoch): preemption steps, time reads, entropy, the divergence checksums of the
state at its end, and its length in steps.

**Replay.** `tt_goto()` drives the rewind verb, whose engine the replay driver
binds (`kernel/replay_driver_sync.c`): restore the keyframe (tear down the live
processes, rebuild them from the blob, pages, and registers), load the epoch's
journal into the wrappers in REPLAY mode, and re-run the processes until the
step clock reaches the target. Instruction targets are reached by single-stepping
the target segment with RFLAGS.TF. Console output is suppressed while replaying.

**Verification.** The divergence checksums cover every mapped user page, every
process's registers, the process table, and the scheduler state. `verify`
re-executes each retained epoch from its keyframe to its end and compares.

**Searches.** Reverse breakpoints and watchpoints re-execute each epoch once,
forward, newest epoch first (`reverse_*_scan` in `kernel/revbreak.c`).
Hardware breakpoints and watchpoints (DR0-DR3) are armed only during searches
and continues; a hit's exact instruction is found by single-stepping its
segment. `watch_last_write` compares values at every step, then single-steps
the segment where the value changed.

**Front ends.** When the machine stops (a failed assertion, a fault, the last
exit, or a debugger request) the debug monitor (`kernel/core/monitor.c`) serves
the gdb remote protocol on COM2 and MCP on COM3. Resuming replays to the end of
the recording and continues live, still recording.

### Limits

- One CPU. Deterministic replay of SMP is not attempted.
- Preemption is at kernel-entry granularity: a process that never enters the
  kernel is not preempted while recording.
- The journaled inputs are the ones programs can see today: preemption, time,
  and entropy. There is no device input (keyboard, network) to journal yet.
- Programs may not use x87/SSE state (the context switch saves the general
  registers only); user programs are built with `-mgeneral-regs-only`.
- Recorded history is read-only: gdb memory and register writes are refused.
- The workloads are the embedded programs in `user/laplace/`.

## Modules

| Stage | Modules |
|-------|---------|
| Boot, CPU, memory, processes, system calls | `kernel/core/{boot.asm,isr.asm,cpu.c,mm.c,proc.c,machine.c,main.c}` |
| Recording, keyframes, restore, replay, verify, navigation | `kernel/core/timetravel.c` |
| Input journal (double-buffered store) and live capture | `kernel/checkpoint_journal.c`, `kernel/journal_capture.c`, `kernel/journal_capture_sync.c` |
| Journal ring (one journal per retained keyframe) | `kernel/journal_ring.c` |
| Deterministic preemption, time, entropy | `kernel/sched_record.c`, `kernel/time_record*.c`, `kernel/entropy_record*.c` |
| Checkpoint engine (COW marking, capture, writeback) | `kernel/checkpoint.c`, `kernel/snapshot_store.c` |
| Keyframe retention ring and store | `kernel/keyframe_ring.c`, `kernel/keyframe_store.c`, `kernel/keyframe_store_sync.c` |
| Replay engine and driver | `kernel/replay_engine*.c`, `kernel/replay_driver*.c` |
| Divergence detector and component checksums | `kernel/divergence*.c` |
| Rewind, reverse, reverse breakpoints/watchpoints | `kernel/rewind*.c`, `kernel/reverse*.c`, `kernel/revbreak*.c` |
| gdb remote protocol and target | `kernel/gdbstub.c`, `kernel/gdbstub_sync.c`, `kernel/gdb_serial.c`, `kernel/core/gdb_target.c` |
| MCP protocol and tools | `kernel/mcp.c`, `kernel/mcp_sync.c`, `kernel/mcp_server.c`, `tools/laplace-mcp` |
| Debug monitor | `kernel/core/monitor.c` |

The pure modules have host unit tests under `tests/`. The booted kernel is
tested by `make selftest` and `tests/qemu/timetravel_e2e.py`.

## Why this is tractable here

The checkpoint engine already produces periodic whole-system keyframes. A keyframe is a
consistent snapshot of every user address space plus the process table and scheduler
state. What it does not capture is the path between two keyframes: the exact sequence of
inputs and preemptions that led from one to the next.

If that path is made reproducible, then any past instant can be reconstructed by taking
the nearest keyframe at or before it and replaying forward. Reverse execution then falls
out for free: to step backward, restore the prior keyframe and replay to just before the
current point. A small purpose-built kernel is far easier to make fully deterministic
than a general-purpose OS, which is the core advantage IKOS has here.

## Plan 1: Deterministic Replay Core

Goal: two runs from the same keyframe, given the same recorded inputs, reach a
byte-identical state at every epoch boundary.

### Sources of nondeterminism

This section is the spike deliverable for the catalog. Every source that can make two runs
diverge must be either recorded and replayed, virtualized, or gated to a deterministic
point. Each entry below is grounded in the current code.

#### Spike findings (current code)

A read of the kernel established the facts that shape the plan:

- Preemption is driven by the PIT on IRQ0. `kernel/interrupts.c` programs the PIT
  (`setup_timer_interrupt`, ports 0x43 and 0x40) and routes IRQ0 to INT 32
  (`timer_interrupt_entry`). `scheduler_tick` in `kernel/scheduler.c` is the handler and
  does round-robin preemption when a task's `time_slice` reaches zero. Where each tick
  lands in the instruction stream is the dominant source of divergence.
- The checkpoint cadence is already tick-counted, not wall-clock. `checkpoint_tick`
  (`kernel/checkpoint.c:668`) increments `g_timer_ticks` and fires every
  `g_timer_interval` ticks. Keyframe epochs are therefore already deterministic once the
  tick sequence itself is deterministic; checkpoint timing needs no separate work.
- RDTSC is not yet a live source. `get_rdtsc` in `kernel/numa_allocator.c` is a stub that
  returns 0. It must be recorded or virtualized before any real RDTSC read is added, but
  it does not cause divergence today.
- The random MAC generator `eth_addr_random` in `kernel/net/ethernet.c` is actually
  hardcoded, so it is deterministic despite its name. Networking state is also severed on
  restore by the external-state policy, so net-path randomness does not affect persistence
  correctness.
- Real external entropy enters through `auth_generate_random` in `kernel/auth_core.c`,
  which reads `/dev/urandom`. This is a genuine nondeterministic source.
- The IDE driver polls rather than taking completion interrupts (`ide_wait_ready` and
  `ide_wait_drq` in `kernel/ide_driver.c` spin on the status register). Disk data content
  is deterministic; only the number of poll iterations varies, which perturbs timing.

#### Catalog and strategies

| Source | Where it enters | Strategy | Files touched |
|--------|-----------------|----------|---------------|
| Interrupt and preemption timing | PIT IRQ0 lands at a hardware-timed point in the instruction stream; `scheduler_tick` may preempt | Record the logical point of each context switch (an instruction or event count since the epoch); on replay, deliver the tick and preempt at the same point | `kernel/interrupt_stubs.asm`, `kernel/interrupts.c`, `kernel/scheduler.c` |
| Checkpoint cadence | `checkpoint_tick` fires on a tick count | Already deterministic given a deterministic tick sequence; no change beyond the preemption work | `kernel/checkpoint.c` (no change expected) |
| Keyboard input | Scancodes arrive via IRQ1 and are read and translated in `kernel/input_keyboard.c` | Record each scancode with its epoch and logical clock in the input journal; feed from the journal on replay | `kernel/input_keyboard.c`, `kernel/input_manager.c`, journal |
| External entropy (/dev/urandom) | `auth_generate_random` reads `/dev/urandom` | Record the returned bytes on the live run and return them on replay, or seed a recorded PRNG captured in the checkpoint | `kernel/auth_core.c`, journal |
| RDTSC and cycle reads (latent) | `get_rdtsc` stub today; any future real read branches on cycles | Record on the live run and return recorded values on replay; gate before a real RDTSC is introduced | `kernel/numa_allocator.c`, journal |
| IDE completion timing | Poll-loop iteration counts vary in `ide_wait_ready` and `ide_wait_drq` | Content is deterministic; keep polling from perturbing the logical clock, or drain completions at a fixed logical point | `kernel/ide_driver.c` |
| MAC and TLS randomness (not active) | `eth_addr_random` is hardcoded; net state is severed on restore | No action for persistence; if net replay is wanted later, record like other entropy | `kernel/net/*` (future) |

The input journal referenced above is the deliverable of the next issue and reuses the
double-buffered slot and CRC conventions in `kernel/checkpoint_disk.c`.

#### Outcome

This catalog fixed the scope of the deterministic replay core: the primary work was
deterministic preemption, the input journal for keyboard input and entropy, and a gate for
RDTSC before it goes live; the RDTSC stub, the hardcoded MAC, and severed network
randomness needed no work. Those pieces are now implemented, per the
[Modules](#modules) table above.

### Components

**Input journal.** A ring log written alongside each checkpoint slot that captures every
nondeterministic input, tagged with the epoch and a logical clock. It reuses the
double-buffered slot and CRC conventions in `kernel/checkpoint_disk.c`, and must survive a
crash as cleanly as the checkpoint slots: a crash before commit leaves the last good
journal intact. The journal is the delta that turns discrete keyframes into a continuous,
replayable timeline.

**Deterministic preemption.** Record the tick or instruction-count sequence at which
context switches occur, so replay switches contexts at the identical points a live run
did. This touches `kernel/interrupt_stubs.asm`, the timer and scheduler path, and the
existing tick-based checkpoint trigger. Preemption timing is the single largest divergence
risk; getting it right removes most replay drift.

**Time and entropy capture.** Trap or wrap RDTSC, timer counters, and RNG seeding so that
on replay they return recorded values rather than live hardware.

### Replay engine and verification

**Replay mode.** Boot from the nearest keyframe and apply the input journal forward to
reach an arbitrary target epoch. This reuses the existing restore plus process-table,
scheduler, and context reconstruction path. A target epoch is a checkpoint epoch plus an
offset into the journal.

**Divergence detector.** During replay, checksum system state at each epoch boundary and
compare against what was recorded live. Any mismatch is a nondeterminism leak the catalog
missed. Build this early and keep it on in debug builds; it is the harness that keeps the
whole feature honest.

**CI test.** A `scripts/test/timetravel_demo.sh` in the spirit of
`scripts/test/persistence_demo.sh`: run a session, replay it headless, and assert the
final state is byte-identical. This gates the milestone and guards against regressions.

### Time-travel UX

- **Keyframe retention ring.** Keep the last N checkpoints on disk, not just the
  double-buffered latest, so rewind targets are not limited to the most recent snapshot.
- **rewind-to.** Restore the nearest keyframe at or before a target epoch and replay to
  the exact target point.
- **Reverse execution.** `reverse-step` and `reverse-continue`, implemented as restore
  the prior keyframe and replay forward to just before the current point.
- **Reverse breakpoints and watchpoints.** Replay forward from the nearest keyframe until
  a condition is hit, to answer "where did this value last change?"

## Plan 2: MCP Interface

Goal: expose record, rewind, and reverse execution as Model Context Protocol tools so an
AI agent can drive a time-traveling debugger over a well-defined interface.

This is implemented: `kernel/mcp.c` is a pure JSON-RPC dispatch core that handles
`tools/list` and `tools/call`, mapping the tool names below onto the rewind (#169), reverse
(#170), and reverse-watchpoint (#171) engines; `kernel/mcp_sync.c` wires the operations to
the kernel entry points. `scripts/test/mcp_heisenbug_demo.sh` runs the agent flow below
headlessly: an agent debugs a planted heisenbug by calling `watch_last_write` and
`rewind_to`, with the stdio JSON-RPC server loop the only remaining transport wiring.

The interface changes who the customer is. Instead of a human who must adopt a new OS, the
consumer is an agent that calls tools. Agents are poor at exactly what this engine is good
at: reproducing a non-deterministic bug, stepping backward, and keeping the world stable
between runs. Running a program inside IKOS gives the agent a fully reproducible, rewindable
world it cannot get on a general-purpose OS.

### Tool surface

The MCP server is a thin wrapper over the replay engine. Proposed tools:

- `record_session(program)`: run the program under the input journal and return a session
  handle
- `list_checkpoints()`: list available keyframes and their epochs
- `diff_epochs(a, b)`: report what changed between two moments
- `rewind_to(epoch)`: restore the nearest keyframe and replay to the target
- `reverse_step()` and `reverse_continue()`: step or run backward
- `watch_last_write(addr_or_symbol)`: find the last write to an address or symbol
- `replay(from, to)`: deterministically replay a range

### Agent debugging flow

1. The agent runs suspect code with `record_session`.
2. On a failure, it calls `watch_last_write` or `diff_epochs` to localize the cause.
3. It uses `rewind_to` and `reverse_step` to inspect the exact moment the state went bad.
4. It proposes a fix, re-records, and confirms the failure no longer reproduces.

### Positioning and limits

- MCP is the interface, not the value. It exposes the replay engine; it does not create
  it. The Deterministic Replay Core must land first, or the tools return garbage.
- MCP is not a moat. Anyone can wrap an existing record-replay tool in MCP. The defensible
  asset is the deterministic-sandbox property that off-the-shelf engines cannot match.
- Scope it to code the agent runs inside IKOS: greenfield code, reproducible verification,
  embedded targets, and teaching. It is not a way to rewind an existing production
  deployment.

## Milestones

- **A. Deterministic replay core**: catalog nondeterminism, input journal, deterministic
  preemption, virtualized time and entropy.
- **B. Replay engine and verification**: replay mode plus the divergence detector, gated
  by a byte-identical CI test.
- **C. Time-travel UX**: keyframe retention, rewind-to, reverse execution, reverse
  breakpoints.
- **D. Tooling**: a GDB reverse-execution bridge.
- **E. MCP interface**: the tool surface above, plus a demo in which an agent debugs a
  planted heisenbug by rewinding the machine.

Suggested order: land the A chain first, since it proves the whole thesis before any UX,
GDB, or MCP work. Milestone E depends on C.
