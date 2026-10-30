# Changelog

## 0.1.0

The first release: Laplace records and debugs unmodified Linux programs
backward (#235).

### Runs

- Unmodified static Linux x86-64 programs (musl) through the `syscall` ABI with
  TLS (#238), loaded from outside the kernel with argv, envp, and auxv (#240),
  and dynamically linked ones through `ld-musl` or a distribution's glibc
  `ld-linux` (#254).
- Virtual memory for real programs: `brk`, `mmap`, `munmap`, `mprotect`,
  `mremap`, demand paging, up to 4 GiB of RAM (#239).
- Files in a filesystem inside the machine, seeded from a read-only cpio image
  and checkpointed with it (#247).
- Processes (`fork`, `execve`, `wait4`, `kill`, #248), signals (#250), threads
  and futexes for pthreads (#251), blocking calls, idle, and timers (#246).
- Console input, pipes, `poll` and `select` (#249); an e1000 network card with
  IPv4, UDP, TCP, sockets, and epoll (#253).
- Practical targets in CI: a musl hello world, Lua 5.4, the SQLite 3.44 shell,
  and a pthreads lost-update race (#256).

### Records and replays

- Every nondeterministic input is journaled: preemption, clock and entropy
  reads, raw `rdtsc`/`rdtscp`/`cpuid` in user code (#241), bytes typed on the
  console, and received network frames, as one record format with payloads of
  any size (#242).
- Preemption at any user instruction without a hardware counter: a replay
  finds each point again by the process's state (#245). A replay that cannot
  reach its point, because it loops through states it already had, is reported
  as diverged.
- Keyframes hold x87/SSE/AVX state (#237) and every kernel subsystem's state in
  one format that `verify` checksums and names when it differs (#243). They
  are incremental and write only the pages an epoch dirtied (#244).
- The store is an IDE disk (bus-master DMA when the controller has it), an
  AHCI (SATA) disk, or a RAM disk. It survives power cuts.

### Debugs

- Stock gdb with no file argument: the program and its shared libraries are
  read from the target. Every thread of every process, x87/SSE/AVX registers,
  `reverse-stepi`, `reverse-continue` to breakpoints and watchpoints (#252).
- MCP for agents: 13 tools, including `watch_last_write`, `rewind_to`,
  `reverse_stepi`, and `verify_replay`, through `tools/laplace-mcp`.

### Measured

- Recording costs 1.1 to 1.6x under TCG and 1.04 to 1.33x under KVM for the
  benchmark workloads; rewind takes milliseconds (#255,
  [`docs/benchmarks.md`](docs/benchmarks.md)).

### Platforms

- QEMU TCG: every gate. QEMU/KVM on GitHub-hosted runners: the self-test, the
  preemption gate, and the e2e gate (#236).
- Not yet shown: a bare-metal machine, and the branch-counter path on a host
  whose PMU is exposed. No such machine has run Laplace yet
  ([`docs/architecture/hardware.md`](docs/architecture/hardware.md)).

### Removed

- The pre-Laplace subsystems that were outside the kernel build (#233).

### Limits

One CPU; a preempted hot loop replays 100 to 300x slower than it ran under TCG
(no branch counter yet); IPv4 only, with a minimal TCP; no AVX-512, no NVMe, no
UEFI image. The README lists them all.
