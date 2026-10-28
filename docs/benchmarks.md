# Overhead: recording, keyframes, replay, rewind (#255)

How much recording costs, and how fast a recording can be navigated. The
numbers come from `scripts/bench/overhead.py`. CI runs it under TCG and KVM
on every push to main and keeps the tables as the `overhead` artifact. It is
not a gate.

## How it is measured

Each workload boots twice with the `bench` kernel option:

- once with `norecord`, which turns off keyframes, journals, and preemption
  points. Inputs are still read through the kernel and the user `rdtsc` trap
  stays, so this is Laplace without recording, not Linux.
- once recording, with the default cadence of a keyframe every 100 ms and 64
  keyframes retained, so the window holds the whole run.

After the run the kernel prints `bench:` lines. Times are the guest's TSC,
from the start of the first program, so boot is not counted. Run times are
the fastest of three boots, because QEMU on a shared host is noisy. The
navigation columns come from one recording.

| Column | Meaning |
|--------|---------|
| run | the baseline run time, without recording |
| recorded | the run time while recording, and its ratio to the baseline |
| keyframes | keyframes taken, and keyframe page bytes written per second of the recorded run |
| other disk | everything else the store wrote: journals, input payloads, metadata |
| verify | re-executing every retained epoch and comparing it byte for byte |
| replay | re-executing every retained epoch without the comparisons, as a multiple of the recorded run |
| rewind | restoring the oldest keyframe (MCP `rewind_to`) |
| rstepi | `reverse-stepi` from the end of the recording: mean and worst of 10 |

The store is a RAM disk, so the disk columns measure what the recording
writes, not a disk's speed.

## Workloads

- **hello**: `musl_hello`, a static musl hello world.
- **lua**: Lua 5.4 running `rootfs/scripts/gate.lua` (floats, tables, the
  collector, file I/O).
- **sqlite**: the SQLite 3.44 shell creating, filling (20000 rows), indexing,
  and querying a database file.
- **threads**: two pthreads making 300000 racy increments (`musl_threads race`).
- **cpu**: `user/linux/benchcpu.c`, a sieve with no system calls, 4 rounds.
- **alloc**: `user/linux/benchalloc.c`, 100000 malloc/free of mixed sizes,
  large ones from mmap, each block written.

## TCG

QEMU 8.2 TCG, `-m 512M`, on a 2.1 GHz Xeon development host (not a CI runner):

| workload | run ms | recorded ms | keyframes | other disk | verify ms | replay ms | rewind ms | rstepi ms |
|---|---|---|---|---|---|---|---|---|
| hello: hello world (musl) | 5.8 | 15.2 (2.61x) | 1, 2.6 MiB/s | 6.5 MiB/s | 12.2 (1 epochs, 0 diverged) | 0.8 (0.1x) | 0.7 | 2.0 / 8.7 |
| lua: Lua 5.4: the gate script | 207 | 247 (1.20x) | 3, 6.1 MiB/s | 637 KiB/s | 342 (3 epochs, 0 diverged) | 314 (1.3x) | 1.1 | 25.9 / 44.7 |
| sqlite: SQLite shell: 20000 rows, an index, queries | 589 | 663 (1.13x) | 6, 10.2 MiB/s | 370 KiB/s | 2202 (7 epochs, 0 diverged) | 2157 (3.3x) | 1.3 | 45.4 / 78.4 |
| threads: pthreads: 2 threads, 300000 racy increments | 21.0 | 30.7 (1.46x) | 1, 1.5 MiB/s | 3.3 MiB/s | 4017 (1 epochs, 0 diverged) | 4168 (135.9x) | 1.2 | 4569 / 8136 |
| cpu: compute-bound: a sieve, 4 rounds | 128 | 153 (1.19x) | 2, 3.6 MiB/s | 837 KiB/s | 44077 (2 epochs, 0 diverged) | 44122 (288.7x) | 2.3 | 41.4 / 72.7 |
| alloc: allocation-heavy: 100000 malloc/free | 746 | 1172 (1.57x) | 7, 44.1 MiB/s | 374 KiB/s | 2927 (9 epochs, 0 diverged) | 1424 (1.2x) | 1.6 | 116 / 198 |

## KVM

Not measured yet: this sandbox has no `/dev/kvm`. The table will come from a
GitHub-hosted runner (AMD EPYC, no PMU) with `QEMU_ACCEL=kvm`.

## What the numbers say

- **Recording is affordable.** Real programs (Lua, SQLite) run 1.1 to 1.2x
  slower while recording under TCG. hello world pays a fixed cost: its first
  keyframe is most of its 15 ms. The allocation-heavy run is 1.6x slower: it
  dirties about 40 MiB/s of pages, and each keyframe copies and checksums them.
- **Keyframe checksums were the bottleneck.** Before this issue, every
  keyframe page was checksummed one bit at a time, about 40 instructions a
  byte. The allocation-heavy run then recorded 4x slower than the baseline
  (5.7 s against 1.4 s). `include/crc32_ieee.h` gives the same CRC32 eight
  bytes at a time.
- **Rewind is cheap**: restoring the oldest keyframe takes 1 to 3 ms.
- **Replaying a hot loop that was preempted is slow.** Replay finds a
  preemption point without a branch counter (#245). It traps on the point's
  instruction every time that instruction runs, from the previous kernel entry
  until the registers and memory match. For a loop that makes no system calls,
  that is one trap per iteration. Replay is then 136x (threads) to 290x (cpu)
  slower than the recorded run, and a `reverse-stepi` that has to replay such
  an epoch takes seconds. Programs that make system calls (Lua, SQLite, alloc)
  replay at 1.2 to 3.3x.
  - Under TCG each such check costs two traps (RF, plus a single step,
    because TCG applies RF to a whole translated block). Elsewhere RF alone
    is used, one trap per check.
  - A branch counter, on bare metal or a host with a vPMU (#236), would
    remove the cost. No CI machine has one.

## Budgets

The first numbers set these budgets. A regression past one should be
explained:

- Recording overhead for lua and sqlite: at most 1.5x under TCG.
- Recording overhead for alloc: at most 2x under TCG.
- Rewind to the oldest keyframe: at most 10 ms.
- Replay of programs that make system calls (lua, sqlite, alloc): at most 5x
  the recorded run.
- Hot-loop replay (threads, cpu) has no budget until a branch counter exists.
  These rows are tracked so an improvement shows.

Reproduce:

```bash
python3 scripts/bench/overhead.py                 # TCG, every workload
QEMU_ACCEL=kvm python3 scripts/bench/overhead.py  # KVM
python3 scripts/bench/overhead.py cpu --repeat 1  # one workload
```
