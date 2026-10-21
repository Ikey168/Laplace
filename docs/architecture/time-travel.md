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

**Determinism.** Context switches happen only at kernel entries, through
`sched_record_decide()`, which records the step of every preemption on a live
run and forces the same switches on replay. This is what makes the logical
clock execution-derived; the original design counted timer ticks, which land
at a different instruction on every run. A timer interrupt that lands in user
mode becomes an entry of its own when the machine wants one there (#245,
below); otherwise it only marks the time slice expired. The clock (`SYS_TIME`) and
entropy (`SYS_RANDOM`) system calls go through `ktime_read()` and
`kentropy_fill()`. User-mode `rdtsc` and `rdtscp` raise #GP (CR4.TSD) and are
emulated from `ktime_read()`; with CPUID faulting, user `cpuid` raises #GP too
and is answered from a masked table captured in every keyframe's kernel state
(#241). Each emulated instruction is a kernel entry, so a step. Everything else
a program observes is a function of its memory and registers, so re-executing
from a keyframe with the journal reproduces the run exactly, unless it
executes an instruction nothing can trap (`rdrand`, `rdseed`, `rdpid`), which
the table hides and verification reports.

**Extended state** (#237). The kernel never touches x87/XMM/YMM registers
(`-mgeneral-regs-only`), so between entries the register file holds the state
of the last process that ran. Switching to another process saves that state
and restores the next one's (`kernel/core/fpu.c`); nothing traps, so the steps
are the same in recording and replay. Saved areas are made canonical
(components the CPU left in their initial state are written out as such), live
in a keyframe's context record next to the general registers, and are part of
the register checksum (control words, MXCSR, ST0-7, XMM0-15, the upper YMM
halves; the last-instruction pointers are not, as CPUs differ in storing
them).

**Linux programs** (#238). The kernel runs in the top 2 GiB and reaches RAM
through a direct map, so the lower half of every address space is free for
programs linked at 0x400000. `syscall` enters through a stub that builds the
same trap frame `int 0x80` does (vector 129, `VEC_LINUX_SYSCALL`), so a Linux
system call is a step like any other. `kernel/core/linux.c` implements the
calls; FS/GS base (TLS) is per process, loaded on switch, kept in keyframes,
and checksummed. Times are a journaled TSC read converted through a clock base
(TSC frequency, wall clock at boot) that is part of every keyframe's kernel
state; resuming after a power cut re-anchors it to the new boot's TSC and takes
a keyframe at once. There is no vDSO, so libc reads the time through a system
call.

**Process memory** (#239, `kernel/core/vm.c`). An address space is a sorted
list of regions and the pages present in them, at fixed places (no
randomization): ELF segments, the `brk` heap after the highest one, `mmap`
top-down from 0x7FFF00000000, and for Linux programs an 8 MiB stack below
0x7FFFFFFF0000. Pages appear on first touch: a not-present fault inside a
region that allows the access maps a zeroed page and resumes the instruction.
Which pages are present is part of every keyframe, so a replay takes the same
faults at the same instructions, and like snapshot-COW captures they are not
steps. A system call reading an untouched page sees zeros without mapping it,
and so does a debugger, so inspecting memory never changes the machine. Before
any change to regions or pages while recording, a pending keyframe writeback
is finished, since it reads them. The kernel-state blob holds every region
(start, end, protection, kind) in one flat list; restore re-creates them and
maps each saved page with its region's protection (no user bit for
`PROT_NONE`). The process-table checksum covers the regions and the break, the
user-page checksum each page's user bit.

**Files** (#247, `include/memfs.h`, `include/core/files.h`). The filesystem
is inside the recording. It starts from a read-only seed image, a cpio
"newc" archive passed as a Multiboot module (`tools/mkcpio.py` packs
`rootfs/` deterministically), which the recording pins by CRC: seed files are
read straight from the image and need no journal, and a recording refuses to
resume with a different image. Whatever programs create or change is kernel
state: the tree (inodes, directories, sizes, times) is the `fs` section of
the keyframe, file pages go in the page pool next to user memory (as a page
provider with pseudo pids, tracked unchanged-or-not the same way), and open
file descriptions, descriptor tables, working directories, and umasks are the
`files` section. Both are divergence components of their own. A seed file is
copied into pages at its first change. File times follow the last wall-clock
time a program read through a journaled call, so they replay too. Programs
can also be started from the image (`run=/bin/lua`).

**Inputs** (#242, `include/iorec.h`, `include/core/kio.h`). Everything that
enters the machine from outside is one kind of record: (step, kind, call
number, argument digest, return value, payload). Clock reads (`ktime_read`)
and entropy (`kentropy_fill`) go through it now, and console and network
input will. While recording, the live source runs and its result is
appended; payload bytes go into one byte stream per epoch whose pages are
written to the page pool as they fill (owned by the epoch's keyframe and
freed with it), so a large input costs pool pages and a few journal events
(two per record, one per run of pages), not a fixed-size journal stream. While
replaying, the source never runs: the call must be the one recorded at this
step with the same kind, call number, and argument digest, and it gets the
recorded result and payload. A call that does not match stops the replay at
that step, and `verify` reports the epoch as diverged in the `inputs`
component (15): the leak is caught at its first visible effect. Journal slots
are sized from the disk (640 to 2560 sectors) and the label records the size.
Programs reach devices only through `machine_device_write`, which does
nothing while replaying, so a replay never repeats an external side effect.

**Blocking and idle** (#246, `include/core/machine.h`). A process that
waits leaves the run queue with a reason, the object it waits on, and an
optional deadline; the wait is part of the `procs` section and of the
process-table checksum. A wake caused by another process's call is
deterministic. A deadline passing is an input: while recording, the kernel
compares deadlines with the clock at every entry and while idle, wakes the
process, and journals the wake (a `kio_event`) with the step it was delivered
at; a replay delivers it at that step and never waits. When nothing can run,
the kernel halts with interrupts enabled until a wake is due or a debugger
asks to stop: idle time adds no steps. `nanosleep` and `clock_nanosleep` use
this; futexes, `wait4`, `poll`, and console input reuse it.

**Processes** (#248, `include/core/pctl.h`). `fork` (and `vfork`, `clone`
without `CLONE_VM`) shares every frame read-only between parent and child
with `PAGE_FORK_COW`; a table of shared frames counts their mappings, and
the first write copies the frame. Snapshot COW and fork COW meet in one
write-fault path (`vmm_user_write_fault`): the keyframe image is captured
first, then the frame is unshared; a keyframe stores a shared frame in one
pool slot. `execve` loads through the loader into a fresh address space,
then closes close-on-exec descriptors. An exiting process with a living
parent stays a zombie (pid and wait status) until `wait4` reaps it, and a
parent already waiting is woken at once; orphans go to pid 1. The pid
counter, the parent links, the zombies' statuses, and the wait4 operands are
kernel state.

**Signals** (#250, `include/core/ksig.h`). Each process has 64 actions, a
blocked and a pending set (with each pending signal's siginfo), an alternate
stack, and up to eight timers (`setitimer`/`alarm` and `timer_create`); all
of it is the `signals` keyframe section and divergence component. Delivery
happens only when a process is about to run after a kernel entry, so it is a
function of the step. A handler gets a Linux x86-64 `rt_sigframe`
(`sa_restorer` as its return address, a ucontext with the sigcontext and
signal mask, the siginfo, and the x87/SSE/AVX state with the XSAVE markers),
and `rt_sigreturn` restores from it. A process blocked in a wait that a
signal interrupts returns `-EINTR`. Faults become signals at the faulting
instruction (#PF/#GP -> SIGSEGV with `si_addr`, #DE/#MF/#XM -> SIGFPE, #UD
-> SIGILL, int3 -> SIGTRAP, #AC -> SIGBUS). With no handler, a fault stops
the machine for the debugger (gdb sees the signal and the faulting line), as
does any other core-dumping signal (`abort()`); other fatal signals end the
process with the signal as its wait status, and an exiting child sends its
parent SIGCHLD. Timer expiries are inputs: noticed against the clock while
recording, journaled with the step they are delivered at, and replayed there.

**Preemption at any instruction** (#245, `include/core/preempt.h`,
`include/pmatch.h`). While recording, a timer interrupt in user mode becomes a
kernel entry (vector 32, a step like any other) when the slice expired and
another process is ready, a timer deadline passed, a keyframe is due, or a
debugger asked to stop. So a program that never makes a system call is
preempted, signalled, keyframed, and stoppable. No counter says how far it
got, so the entry's journal record describes the point by state: the
registers, the FS/GS bases, a checksum of the x87/SSE/AVX state, and a hash
of every page the CPU marked dirty since the previous such record (which
clears the marks). Pages outside that set are the same at every point of the
segment, because only that process ran. A replay arms a hardware breakpoint
at the recorded rip, for the process that runs the segment, in a slot the
debugger leaves free; each hit compares the registers, then the extended
state, then the pages (the one that differed last time first), and the first
match becomes the entry. A page the record saw present must be present: an
untouched page is not one that reads as zeros. Since a recording can be
preempted between a demand-paging fault and the retry of its instruction,
the point is checked again after every such fault. When the same state
occurs more than once (a spin),
every occurrence has the same future, so the first is as good as the
recorded one. A point inside `rep movs`/`rep stos` (rip on the instruction,
rcx/rsi/rdi partly advanced) is reached by running the remaining iterations
in the kernel, exactly as the CPU would; other string instructions are
single-stepped; one that cannot reach the point runs to its end with the
breakpoint moved past it. A replay that reaches the segment's next real entry
first has diverged, and is reported in the "inputs" component. Stepping over
a breakpoint uses RF together with a single step: QEMU's TCG applies RF to a
whole translated block, so RF alone would let a loop run past every later
hit. The cost is a trap or two per execution of the recorded instruction
before the point; a branch counter, where a CPU has one, can skip most of
them (#236).

**Diagnosing a divergence** (#241). Every 16 entries the recorder journals a
rolling hash of the registers at each entry (`KJ_EV_TRACE` in the extended
journal, `include/core/kjournal.h`). When `verify` finds a diverged epoch, the
first trace sample that differs bounds the divergence to 16 entries (or the
whole epoch, when only memory differs); the window is then single-stepped and
the first untrappable instruction in it is reported with its pid, address, and
position.

**Recording** (`kernel/core/timetravel.c`). At the first entry, and then
whenever the interval elapses or a record buffer fills, the kernel closes the
epoch (its journal goes to the journal ring) and takes a keyframe at that entry:
`checkpoint_take()` marks every writable user page copy-on-write and captures
registers, and a kernel-state blob captures the kernel's own state. Writeback to the keyframe store
runs at a later entry, so programs write in between and the COW hook preserves
the checkpoint-time images.

**Kernel state** (#243, `include/kstate_format.h`, `kernel/core/kstate.c`).
The blob is a set of sections, one per kernel subsystem, each with its own
version and CRC; there is no fixed cap on processes, regions, or other
objects. A subsystem registers a save, a restore, and optionally a checksum:
`procs` (pid, parent, state, name, executable, and every region), `cpuid`,
`clock`, `machine` (epoch, current process, next pid, pending entry), and
`probe`, a one-word test subsystem. Restore runs them in registration order,
so a subsystem whose objects name another's (a file descriptor its process)
registers after it and resolves the reference by id; a blob with an unknown
section, or without a registered one, is refused. Registers and pages stay
keyframe records of their own. Each subsystem with a checksum is its own
divergence component (6 and up), and every report names the component:
`verify` says `component 6 (probe)`, the self-test perturbs the probe's
restore and requires that name.

**Storage.** The first IDE disk (or a RAM disk) holds a label, the keyframe
store (a ring index plus N double-buffered regions), the journal ring (one
journal per retained keyframe, `kernel/journal_ring.c`), and the page pool.
Region sizes follow the disk size at format time: a keyframe region gets
1/16 of the disk's share per keyframe (512 to 4096 sectors), the pool the
rest.

**Incremental keyframes** (#244, `include/page_store.h`,
`include/core/kfpages.h`). A keyframe region holds the kernel state, the
registers, and a page manifest: runs of (pid, address, count, first slot)
into the pool, where each slot holds one page. The kernel knows which pages
are unchanged since a reference keyframe (the newest one taken, or the one
last restored) from a software PTE bit, `PAGE_KF_KNOWN`: taking or restoring
a keyframe sets it on every page, and the snapshot-COW write fault (or an
mremap move) clears it. A keyframe reuses the slots of the pages still KNOWN
and writes only the others, so its cost follows the pages its epoch dirtied,
not resident memory: with 256 MiB resident and a 64-page working set, about
90 pages per keyframe (`tests/qemu/scale_gate.sh`). Slots are
reference-counted by the retained manifests; evicting a keyframe frees only
slots no other keyframe names, so the full keyframe written while memory
filled can go while the ones after it still restore. Counts live in memory
and are rebuilt at boot from the manifests on the disk. A keyframe is also
taken early when its epoch has dirtied `dirty=` pages (default 8192).

The same bit makes the epoch-boundary checksum of user memory cheap: a KNOWN
page's CRC is its slot's, computed once when the slot was written. And it
makes restore cheap: the frames of KNOWN pages whose slot the target
keyframe also names are kept, replay saves a pristine copy of a page at its
first write, and only the rest is read from the pool. Journal E holds the
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
- A recorded preemption point is found again by trapping on its instruction:
  replaying a preempted hot loop costs a trap or two per iteration before the
  point (no branch counter is used yet).
- The journaled inputs are the ones programs can see today: preemption, time,
  and entropy. There is no device input (keyboard, network) to journal yet.
- `rdrand`, `rdseed`, and `rdpid` cannot be trapped: the CPUID table hides
  them, and executing them anyway is a divergence that `verify` names. Without
  CPUID faulting (QEMU TCG), user `cpuid` runs natively: deterministic on one
  host, but a recording moved to another CPU may diverge (#241).
- The page pool must hold every distinct page the retained keyframes need:
  resident memory plus what the window's epochs dirtied. A full pool stops the
  recording with a message naming its size; give it a larger disk (or `pool=`
  for the RAM disk).
- An epoch that maps a lot of new memory (a program filling its heap) has a
  keyframe at each end that differ in most pages, so replaying across it
  rereads those pages from the pool.
- x87, SSE, and AVX are saved (XSAVE with XCR0 = x87|SSE|AVX, or FXSAVE);
  AVX-512 and the XSAVE optimizations (XSAVEOPT, XSAVEC, XGETBV1) are not
  offered to programs.
- Recorded history is read-only: gdb memory and register writes are refused.
- The workloads are the embedded programs in `user/laplace/` (Laplace ABI) and
  `user/linux/` (unmodified static musl programs, Linux ABI).
- KVM works (`make selftest` passes under `-enable-kvm -cpu host`, checked on a
  GitHub-hosted runner on 2026-10-21), but those runners expose no PMU, so
  instruction-precise preemption does not rely on a hardware branch counter
  (#245).

## Modules

| Stage | Modules |
|-------|---------|
| Boot, CPU, memory, processes, system calls | `kernel/core/{boot.asm,isr.asm,cpu.c,mm.c,proc.c,machine.c,main.c}` |
| Recording, keyframes, restore, replay, verify, navigation | `kernel/core/timetravel.c` |
| Kernel state in keyframes: sectioned format, subsystems | `kernel/kstate_format.c`, `kernel/core/kstate.c` |
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
