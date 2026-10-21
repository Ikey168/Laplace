# Laplace

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
[![CI](https://github.com/Ikey168/Laplace/actions/workflows/laplace.yml/badge.svg)](https://github.com/Ikey168/Laplace/actions/workflows/laplace.yml)
[![Architecture](https://img.shields.io/badge/Architecture-x86__64-blue.svg)]()
[![Boot](https://img.shields.io/badge/Boot-Multiboot-orange.svg)]()

> *"An intellect which at a certain moment would know all forces that set nature
> in motion, and all positions of all items of which nature is composed... for
> such an intellect nothing would be uncertain, and the future just like the
> past would be present before its eyes."*
>
> Pierre-Simon Laplace, *A Philosophical Essay on Probabilities* (1814)

**Laplace is a time-traveling debugger built as an operating system.** Record a
session, then scrub the whole machine backward: step back from the crash, run
backward to a breakpoint, or ask where a value was last written and land there.

Heisenbugs die on the operating table: rerun the program and the schedule
shifts, the timer reads change, the entropy differs, and the crash is gone.
Laplace attacks that at the root. The OS records every nondeterministic input as
it runs, so any past moment of the whole machine can be reconstructed exactly,
as many times as you need. The bug cannot escape into a different interleaving,
because the interleaving is part of the recording.

The name is the thesis. Laplace's demon, the intellect in the epigraph, is
fiction in physics; here it is an implementation: a recorded state of the
machine plus the journal of everything nondeterministic that followed
determines every later moment, and the kernel recomputes any of them on demand.

Two front ends drive it, both served from inside the kernel while the machine is
stopped:

- **gdb**: stock gdb attaches with `target remote` and debugs the recorded
  machine in both directions: `reverse-stepi`, `reverse-continue`, hardware
  watchpoints and breakpoints, registers, memory, one thread per process.
- **MCP**: an MCP client (an AI agent) connects through `tools/laplace-mcp` and
  gets tools like `watch_last_write`, `rewind_to`, `reverse_stepi`,
  `read_memory`, and `verify_replay`. Agents are bad at exactly what this is
  good at: reproducing a flaky bug and keeping the world stable between
  attempts.

## See it

A planted heisenbug (`user/laplace/heisenbug.c`): an off-by-one ring write that
fires only when preemption, the clock, and a random payload line up. Laplace
records the machine until the program's assertion stops it, then an agent asks
who clobbered the value, over MCP, and gets the exact instruction:

```
[run ] break: pid 1 asserted (code bad) at epoch 14, step 1689
  mcp> read_memory(pid=1, addr=0x40001100, len=4)
       pid=1 addr=0x40001100 len=4 bytes=e4290000 u32=10724       # should be 48
  mcp> watch_last_write(pid=1, addr=0x40001100, len=4)
       last write at epoch=14 offset=1616 insn=26 pid=1 rip=0x400000b3
  mcp> get_registers(pid=1)
       pid=1 rax=0x40 ...                                            # slot 64 of a 64-slot ring
  mcp> reverse_stepi()
       now at epoch=14 offset=1616 insn=25 pid=1 rip=0x400000ac     # the store itself
  mcp> read_memory(pid=1, addr=0x40001100, len=4)
       pid=1 addr=0x40001100 len=4 bytes=30000000 u32=48
  mcp> verify_replay()
       byte-exact: epochs_checked=14 epochs_diverged=0
```

and gdb, attached to the same stopped machine:

```
(gdb) watch -l g.batch_limit
(gdb) reverse-continue
Thread 1 hit Hardware watchpoint 1: -location g.batch_limit
Old value = 10724
New value = 48
0x00000000400000ac in produce (payload=payload@entry=10724, burst=burst@entry=1) at user/laplace/heisenbug.c:42
42	    g.ring[slot] = payload;
```

Every past moment in that session is reconstructed by restoring a keyframe
from the disk and re-executing the recorded processes. `verify_replay`
re-executes every retained epoch and compares checksums of all memory, all
registers, the process table, and the scheduler with the ones recorded live.

Recording of the full session (`asciinema play docs/media/laplace-heisenbug.cast`)
is `tests/qemu/timetravel_e2e.py`, which CI runs on every push:

```bash
make                                  # kernel + user programs (needs gcc, nasm)
python3 tests/qemu/timetravel_e2e.py  # boot, record, debug backward over MCP and gdb
make selftest                         # in-kernel: verify every epoch, catch a perturbed replay
bash scripts/test/qemu_persistence_demo.sh   # pull the power, boot, resume
```

## Using the debugger

```bash
make run    # QEMU: console on stdio, gdb on :1235 (COM2), MCP on :1236 (COM3),
            # recording to build/disk.img. APPEND="run=counter" picks programs.
```

When the machine stops (an assertion, a fault, the last process exiting, or a
debugger connecting while it runs) the kernel serves both ports:

```
$ gdb build/user/heisenbug.elf -ex 'set remotetimeout 120' -ex 'target remote :1235'
(gdb) reverse-stepi
(gdb) monitor where
(gdb) monitor verify

$ claude mcp add laplace -- /path/to/Laplace/tools/laplace-mcp --port 1236
```

`continue` (gdb) or `resume` (MCP) at the end of the recording runs the machine
live again, still recording, until the next stop. How-tos:
[gdb reverse debugging](docs/testing/reverse-debugging.md) and
[driving time travel from MCP](docs/testing/mcp-server.md).

## Why the debugger is an operating system

Record/replay debuggers exist: rr records single Linux processes from the
outside, and emulator-level record/replay (QEMU, Simics) records a virtual
machine from underneath. Laplace moves the recorder inside: time travel is an OS
service. The kernel owns its keyframes, its input journal, its replay, and a
divergence detector that checksums the whole machine at every epoch boundary on
both runs. The unit of replay is every process on the machine and the schedule
between them.

It works like film: a **keyframe** (a whole-system checkpoint) every interval,
and between keyframes a CRC-protected **journal** of every nondeterministic
input the programs can observe:

- preemption: the step (kernel entry) at which every involuntary context switch
  happened, and replay forces the same ones; a timer interrupt in user code
  becomes an entry of its own, journaled by the program's state at that
  instruction (registers, extended state, the pages it dirtied), which a replay
  finds again with a hardware breakpoint
- clock reads (`SYS_TIME`)
- entropy (`SYS_RANDOM`)

Restoring the nearest keyframe and re-executing with the journaled inputs
reconstructs any `(epoch, step, instruction)` in the retained window.
Instruction precision comes from single-stepping the last segment with the trap
flag; breakpoints and watchpoints use the CPU's debug registers while replaying.
The keyframe ring and one journal per keyframe live on the disk and survive a
reboot.

Full design, module map, and limits:
[`docs/architecture/time-travel.md`](docs/architecture/time-travel.md).

## The substrate: orthogonal persistence

The recorder falls out of a stranger property: Laplace is a persistent OS. The
periodic whole-system checkpoints that make power cuts harmless are exactly the
keyframes time travel needs. A program does **nothing special** to be durable:

```c
/* user/laplace/counter.c: the entire persistence "logic" */
static uint64_t counter;
for (;;) { counter++; if (counter % 20000 == 0) print(counter); sys_yield(); }
```

`scripts/test/qemu_persistence_demo.sh` boots it on an IDE disk, kills QEMU
with SIGKILL, and boots again, four times:

```
[boot 1] fresh disk: counted 20000 .. 420000, then power cut after 3s
[boot 2] resumed from keyframe 24: counted 420000 .. 1000000 (had printed 420000 before the cut), cut after 4s
         lost at most 20000 counts to the cut (work since the newest keyframe)
[boot 3] resumed from keyframe 56: counted 1000000 .. 1320000 (had printed 1000000 before the cut), cut after 2.5s
         lost at most 20000 counts to the cut (work since the newest keyframe)
[boot 4] resumed from keyframe 75: counted 1340000 .. 1840000 (had printed 1320000 before the cut), cut after 3.5s
         lost at most 0 counts to the cut (work since the newest keyframe)
PASSED: the booted machine resumed from the IDE disk after every power cut
```

A power cut loses only the work since the newest keyframe on the disk (at most
one keyframe interval, 100 ms by default); the demo checks that bound. (Recording:
`asciinema play docs/media/qemu-resume.cast`.) A checkpoint marks
every writable page read-only and copy-on-write; the first write to a marked
page makes the kernel preserve its pre-checkpoint image, and the keyframe is
written to the disk at a later kernel entry. Each keyframe region is
double-buffered with a superblock flip as its commit point, CRC-checked, and the
boot path resumes from the newest valid one. Design:
[`docs/architecture/orthogonal-persistence.md`](docs/architecture/orthogonal-persistence.md).

## What works

Everything in this table runs in the booted kernel and is exercised in CI
(`.github/workflows/laplace.yml`):

| Capability | Tested by |
|------------|-----------|
| Multiboot boot (QEMU `-kernel`, GRUB ISO), long mode, paging, ring-3 processes, system calls | every QEMU job; `make iso` |
| Recording: keyframes with deferred copy-on-write writeback, one journal per keyframe, on the IDE disk | `make selftest`, e2e |
| Replay by re-executing the processes to any (epoch, step, instruction) | e2e, `make selftest` |
| Byte-exact verification of every retained epoch; a perturbed replay is caught | `make selftest`, `verify_replay`, `monitor verify` |
| Raw `rdtsc`/`rdtscp`/`cpuid` in user code replay exactly; a divergence names the instruction that caused it | `tests/qemu/determinism_gate.sh` |
| x87/SSE/AVX state survives preemption, is in every keyframe, and is part of what replay verifies | `tests/qemu/fpu_gate.sh`, `make selftest` |
| Unmodified static Linux programs (musl) run; their clock and random reads replay exactly | `tests/qemu/linux_gate.sh` |
| Programs load from outside the kernel (Multiboot modules, static and static-PIE) with argv and envp; a recording refuses to resume with changed binaries | `tests/qemu/loader_gate.sh` |
| malloc, `mmap`/`munmap`/`mprotect`/`mremap`, `brk`, and a demand-paged stack replay exactly with keyframes between the calls, at 512 MiB and 4 GiB of RAM | `tests/qemu/vm_gate.sh` |
| Files: a seed image, then a program's own files, in keyframes and checksums; unmodified Lua 5.4 runs a script from the image, writes a file and reads it back, and replays byte-exact; a recording refuses another seed | `tests/qemu/files_gate.sh` |
| Inputs (clock reads, entropy, and later console and network) are one journaled record with payloads of any size: 10 MiB read in one epoch replays byte-exact; a replay whose input call differs from the recording stops at that step and is reported | `tests/qemu/input_gate.sh` |
| Processes: 50 children fork (copy-on-write), exec, exit, and are reaped with their statuses; kill ends a child; rewinding between a fork and its exec shows both processes in gdb and MCP | `tests/qemu/proc_gate.sh` |
| Preemption at any instruction without a counter: two loops with no system call interleave, a loop where only memory changes and `rep movsb` copies are preempted, a spin gets its timer's signals, and each replays byte-exact | `tests/qemu/preempt_gate.sh` |
| Signals: SIGALRM from a timer interrupts a loop at journaled points with FPU state saved in the frame; a SIGSEGV handler recovers; masks, pending, sigsuspend, sigaltstack; every run replays byte-exact; an unhandled fault or abort() stops the machine and gdb shows the signal and the faulting line | `tests/qemu/signal_gate.sh` |
| Blocking: a sleeping process and a working one replay byte-exact with each timer wake at its journaled step; a 10 s idle adds no steps and replays without waiting; a debugger stops the machine while everything waits | `tests/qemu/sleep_gate.sh` |
| Incremental keyframes: 256 MiB resident records at about 90 pages per keyframe and replays without the evicted base keyframe; 64 MiB of malloc/free replays exactly | `tests/qemu/scale_gate.sh` |
| gdb: registers, memory, threads, stepi/reverse-stepi, continue/reverse-continue, hardware breakpoints and watchpoints | e2e |
| MCP: protocol handshake, 13 tools, stdio bridge | e2e, `tests/test_mcp.c` |
| Resume live after debugging, recording continues | e2e |
| Power cut and resume from the disk | `qemu_persistence_demo.sh` |

Limits, stated plainly:

- One CPU. A process is preempted at any instruction, but a replay finds each
  such point by trapping on its instruction: a preempted hot loop replays a
  trap or two per iteration slower (no branch counter is used yet).
- The journaled inputs are preemption, clock, and entropy. There is no device
  input (keyboard, network) yet, so there is nothing else to journal.
- User `rdtsc`/`rdtscp` are trapped and journaled, and user `cpuid` is answered
  from a recorded, masked table where the CPU has CPUID faulting (KVM, recent
  bare metal; QEMU's emulator has none, so there `cpuid` runs natively).
  `rdrand`, `rdseed`, and `rdpid` cannot be trapped: they are hidden from the
  table, and a program that executes them anyway diverges; `verify` then names
  the instruction and its address (#241).
- Keyframes are incremental (#244): each writes only the pages dirtied since
  the previous one into a reference-counted page pool on the disk, so memory
  is limited by the pool (the disk's size, or `pool=` for the RAM disk).
- x87, SSE, and AVX state is saved and verified (#237); AVX-512 is not enabled
  in XCR0, so programs see no AVX-512.
- Workloads are Laplace's own programs (`user/laplace/`, `int 0x80`) and
  unmodified static Linux programs (`musl-gcc -static` or `-static-pie`, the
  Linux `syscall` ABI, #238), embedded in the kernel (`user/linux/`) or passed
  as Multiboot modules (`-initrd`, #240). The
  Linux interface so far covers startup, TLS, stdio to the console, the clocks,
  `getrandom`, memory (`brk`, `mmap`, `munmap`, `mprotect`, `mremap`,
  `madvise`, with pages mapped on first touch, #239), and files (#247): a
  filesystem inside the machine, seeded from a read-only cpio image
  (`rootfs/`, passed with `-initrd`) and checkpointed with it, with `openat`,
  `read`/`write` and their `p`/`v` forms, `lseek`, `stat` and `statx`,
  `getdents64`, `mkdir`, `unlink`, `rename`, links, `ftruncate`, `fcntl`,
  `dup`, `getcwd`/`chdir`, private file `mmap`, and `/dev/null`, `/dev/zero`,
  `/dev/urandom`; and processes (#248): `fork`, `vfork`, `clone` without
  `CLONE_VM`, `execve`, `wait4`, `kill`, zombies and orphans. Unimplemented calls return ENOSYS and are logged once.
- Recorded history is read-only from the debuggers.

## The machine

The system being recorded is Laplace's own small kernel (`kernel/core/`): a
Multiboot entry into long mode, a higher-half kernel with a direct map of all
RAM, per-process 4-level page tables over the whole lower half, ring-3
processes loaded from embedded ELF images, Laplace (`int 0x80`) and Linux
(`syscall`) system calls, and a round-robin scheduler. The kernel runs with interrupts disabled, so a kernel entry is an atomic, deterministic step; a process that waits (`nanosleep`, and later children, futexes, input) blocks, and when nothing can run the kernel idles with `hlt` (#246); the whole
time-travel stack (`kernel/*.c`, host-tested under `tests/`) sits on top of it.

The project was formerly named IKOS, and the tree carries a broad scaffold of a
general-purpose OS from that era (GUI, USB, networking, audio, daemons, a
multi-stage boot sector). That code is **not** in the bootable kernel and does
not compile today; `make legacy` runs its old build. Issue #233 tracks either
repairing or removing it.

## Getting started

```bash
# Ubuntu/Debian
sudo apt-get install -y gcc make nasm musl-tools qemu-system-x86 gdb python3
# for make iso: grub-pc-bin xorriso mtools

git clone https://github.com/Ikey168/Laplace.git && cd Laplace
make            # build/laplace.elf and build/user/*.elf
make selftest   # boots, records the heisenbug, verifies the replay, exits
make run        # boot it for real and attach gdb / MCP
```

Kernel command line (`make run APPEND="..."`):

| Option | Meaning |
|--------|---------|
| `run=a,b,c` | programs to start on a cold boot, each with arguments: an embedded name (`heisenbug`, `noise`, `counter`, `hello`, `musl_hello`, ...) or the path of a module passed with `-initrd`, e.g. `run="/args.elf one two",noise` (default `heisenbug,noise,noise`) |
| `root=PATH` | the filesystem's seed image: a cpio module (default: the first cpio module; `make run` passes `rootfs/`) |
| `env="A=1 B=2"` | the environment of every Linux program |
| `interval=N` | keyframe cadence in timer ticks (1 ms each; default 100) |
| `keyframes=N` | retained keyframes, the rewind horizon (default 16) |
| `fresh` | ignore a recording on the disk and cold-boot |
| `stop_after=N` | stop for the debugger after N keyframes |
| `dirty=N` | also take a keyframe once an epoch dirties N pages (default 8192) |
| `pool=N` | without an IDE disk: the RAM disk's page pool in MiB (default 64) |

| Target | Description |
|--------|-------------|
| `make` | kernel and user programs |
| `make run` | boot in QEMU with gdb and MCP ports and a disk |
| `make selftest` | in-kernel verification, exits QEMU with the result |
| `make test` | `selftest` plus the in-QEMU end-to-end gates |
| `make iso` | GRUB rescue ISO for BIOS machines |
| `make legacy` | the old IKOS wildcard build (does not compile; #233) |

## Testing

CI (`.github/workflows/laplace.yml`) builds the kernel with `-Werror`, runs the
host unit tests of every time-travel and persistence module, runs the host
harnesses (`scripts/test/{persistence,timetravel,scrub,mcp_heisenbug,timetravel_live}_demo.sh`,
which exercise the modules outside the kernel), then boots the kernel in QEMU
for `make selftest`, `tests/qemu/timetravel_e2e.py`, the power-cut demo, and
`make iso`.

## Project structure

```
Laplace/
  kernel/core/        the bootable kernel: boot.asm, isr.asm, cpu, mm, proc,
                      machine (steps, scheduling, traps), timetravel (record,
                      restore, replay, navigation), monitor, gdb_target, main
  kernel/*.c          the time-travel and persistence modules (checkpoint,
                      snapshot/keyframe/journal stores, record wrappers, replay,
                      divergence, rewind/reverse/revbreak, gdbstub, mcp), plus
                      the legacy IKOS subsystems (not built)
  include/            headers (include/core/ for the kernel core)
  user/laplace/       the recorded programs: heisenbug, noise, counter, hello
  user/linux/         unmodified Linux programs built with musl (probes, bigmem)
  rootfs/             the machine filesystem's seed image (packed to build/rootfs.cpio)
  tests/              host unit tests; tests/qemu/ the booted end-to-end gates
  tools/              laplace-mcp (MCP stdio bridge), record-cast, mkcpio
  scripts/test/       host harnesses and the QEMU power-cut demo
  scripts/build/      third-party programs for the gates (Lua)
  docs/               architecture and how-tos
```

## Roadmap

The plan for running real programs (unmodified static Linux binaries, threads
included) is tracked in #235. Its main steps:

- Incremental keyframes that write only dirtied pages, so keyframes scale to
  real memory sizes (#244)
- Journal device input (keyboard, disk completions) so interactive programs
  can be recorded (#242, #249)
- Preemption at any instruction, recorded as a register-and-memory state that
  replay finds with a breakpoint, so it works without a hardware branch counter
  (QEMU TCG, and CI runners without a PMU), with the counter as a speed-up where
  present (#245)
- SMP-safe deterministic replay
- Repair or remove the legacy subsystems (#233)

## Documentation

Full index: [`docs/README.md`](docs/README.md).

- [Time-travel debugging: design, module map, limits](docs/architecture/time-travel.md)
- [Reverse debugging with gdb](docs/testing/reverse-debugging.md)
- [Driving time travel from MCP](docs/testing/mcp-server.md)
- [Orthogonal persistence](docs/architecture/orthogonal-persistence.md) and
  [persistence v2](docs/architecture/orthogonal-persistence-v2.md)

## License

This project is licensed under the **MIT License**. See the [LICENSE](LICENSE) file for
the full text.

## Acknowledgments

- **Namesake**: Pierre-Simon Laplace, whose demon saw the past as clearly as the
  present given the state of the world and the forces acting on it
- **Contributors**: thanks to everyone who has contributed to Laplace
- **Community**: thanks to the open-source OS development community
- **Tools**: built with GCC, NASM, QEMU, and Git
- **Inspiration**: the orthogonal-persistence lineage of KeyKOS, EROS, Phantom OS, and
  IBM i's single-level store

For questions, suggestions, or support, please open an issue or start a discussion.
