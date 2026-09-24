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
  happened; timer interrupts only mark the slice expired, so switches happen at
  kernel entries and replay forces the same ones
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
[boot 1] fresh disk: counted 20000 .. 340000, then power cut after 3s
[boot 2] resumed from keyframe 24: counted 360000 .. 820000 (had printed 340000 before the cut), cut after 4s
[boot 3] resumed from keyframe 56: counted 820000 .. 1100000 (had printed 820000 before the cut), cut after 2.5s
[boot 4] resumed from keyframe 75: counted 1120000 .. 1540000 (had printed 1100000 before the cut), cut after 3.5s
PASSED: the booted machine resumed from the IDE disk after every power cut
```

(Recording: `asciinema play docs/media/qemu-resume.cast`.) A checkpoint marks
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
| gdb: registers, memory, threads, stepi/reverse-stepi, continue/reverse-continue, hardware breakpoints and watchpoints | e2e |
| MCP: protocol handshake, 13 tools, stdio bridge | e2e, `tests/test_mcp.c` |
| Resume live after debugging, recording continues | e2e |
| Power cut and resume from the disk | `qemu_persistence_demo.sh` |

Limits, stated plainly:

- One CPU. Preemption is at kernel-entry granularity: a process that never
  enters the kernel is not preempted while recording.
- The journaled inputs are preemption, clock, and entropy. There is no device
  input (keyboard, network) yet, so there is nothing else to journal.
- Programs may not use x87/SSE state; user programs build with
  `-mgeneral-regs-only`.
- Workloads are the programs embedded in the kernel (`user/laplace/`).
- Recorded history is read-only from the debuggers.

## The machine

The system being recorded is Laplace's own small kernel (`kernel/core/`): a
Multiboot entry into long mode, a frame allocator and per-process 4-level page
tables, ring-3 processes loaded from embedded ELF images, `int 0x80` system
calls, and a round-robin scheduler. The kernel runs with interrupts disabled and
never blocks, so a kernel entry is an atomic, deterministic step; the whole
time-travel stack (`kernel/*.c`, host-tested under `tests/`) sits on top of it.

The project was formerly named IKOS, and the tree carries a broad scaffold of a
general-purpose OS from that era (GUI, USB, networking, audio, daemons, a
multi-stage boot sector). That code is **not** in the bootable kernel and does
not compile today; `make legacy` runs its old build. Issue #233 tracks either
repairing or removing it.

## Getting started

```bash
# Ubuntu/Debian
sudo apt-get install -y gcc make nasm qemu-system-x86 gdb python3
# for make iso: grub-pc-bin xorriso mtools

git clone https://github.com/Ikey168/Laplace.git && cd Laplace
make            # build/laplace.elf and build/user/*.elf
make selftest   # boots, records the heisenbug, verifies the replay, exits
make run        # boot it for real and attach gdb / MCP
```

Kernel command line (`make run APPEND="..."`):

| Option | Meaning |
|--------|---------|
| `run=a,b,c` | programs to start on a cold boot: `heisenbug`, `noise`, `counter`, `hello` (default `heisenbug,noise,noise`) |
| `interval=N` | keyframe cadence in timer ticks (1 ms each; default 100) |
| `keyframes=N` | retained keyframes, the rewind horizon (default 16) |
| `fresh` | ignore a recording on the disk and cold-boot |
| `stop_after=N` | stop for the debugger after N keyframes |

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
  tests/              host unit tests; tests/qemu/ the booted end-to-end gate
  tools/              laplace-mcp (MCP stdio bridge), record-cast
  scripts/test/       host harnesses and the QEMU power-cut demo
  docs/               architecture and how-tos
```

## Roadmap

- Journal device input (keyboard, disk completions) so interactive programs
  can be recorded
- A loader for programs not embedded in the kernel
- x87/SSE state in contexts and keyframes
- Retired-instruction counting (where available) for preemption finer than a
  kernel entry
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
