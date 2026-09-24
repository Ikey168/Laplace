# Reverse debugging with gdb

Laplace serves the gdb remote serial protocol from inside the kernel, over its
second serial port (COM2). Stock gdb attaches to it with `target remote` and
debugs the recorded machine backward and forward: registers, memory,
breakpoints, watchpoints, `reverse-stepi`, `reverse-continue`, and the usual
forward commands, all against the reconstructed state.

This is not QEMU's gdb server (`qemu -s`), which debugs the emulated CPU and
knows nothing about Laplace's history. Laplace's stub belongs to the kernel and
answers from its own recording.

## A session

Boot the heisenbug workload (the default) and wait for the machine to stop:

```
$ make run
...
heisenbug: ledger at 0x0000000040001000, batch_limit at 0x0000000040001100
monitor: stopped (break: pid 1 asserted (code bad)) at epoch 14 step 1689; gdb on COM2, MCP on COM3
```

`make run` puts COM2 on `127.0.0.1:1235`. In another terminal:

```
$ gdb build/user/heisenbug.elf
(gdb) set remotetimeout 120
(gdb) target remote :1235
main () at user/laplace/heisenbug.c:73
(gdb) print g.batch_limit
$1 = 10724                                   # should be 48: something clobbered it
(gdb) watch -l g.batch_limit
Hardware watchpoint 1: -location g.batch_limit
(gdb) reverse-continue
Thread 1 hit Hardware watchpoint 1: -location g.batch_limit

Old value = 10724
New value = 48
0x00000000400000ac in produce (payload=payload@entry=10724, burst=burst@entry=1) at user/laplace/heisenbug.c:42
42	    g.ring[slot] = payload;
(gdb) info registers rax
rax            0x40                64        # slot 64 of a 64-slot ring
(gdb) monitor verify
byte-exact: 14 epochs re-executed from their keyframes
```

This transcript is what `tests/qemu/timetravel_e2e.py` runs and checks in CI.

## What gdb sees

- **Threads** are processes (`info threads`); a stop names the current one.
- **Registers and memory** are read from the machine as reconstructed at the
  current position. Recorded history is read-only: writes are refused.
- **`stepi` / `reverse-stepi`** move one instruction. Laplace reaches an
  instruction by restoring the nearest keyframe, replaying to the enclosing
  kernel entry, and single-stepping (RFLAGS.TF) to it; moving back across a
  kernel entry first counts the previous segment's instructions.
- **`continue` / `reverse-continue`** run to the next or previous hit of a
  breakpoint or watchpoint (up to four: they are the CPU's debug registers,
  scoped to the process gdb had selected when they were inserted), or to the end
  or start of the recording, which gdb reports as "No more reverse-execution
  history". A watchpoint found going backward stops before the write, with the
  old value still in memory, which is what gdb expects.
- **`continue` at the end of the recording** leaves history: the machine runs
  live, recording, until it stops again (Ctrl-C in gdb stops it).
- **`monitor where | window | verify | stats | goto E S [I] | help`** are
  Laplace commands. `verify` re-executes every retained epoch and compares
  checksums of memory, registers, process table, and scheduler; it can take a
  few seconds, hence `set remotetimeout 120`.

## Positions

`monitor where` prints the position as `(epoch, step, insn)`. A step is one
kernel entry (a system call or a fault); insn 0 is the moment the entry traps,
insn 1 the moment after it was handled, insn k after k-1 further instructions.
See [`docs/architecture/time-travel.md`](../architecture/time-travel.md).

## Implementation

| Piece | File |
|-------|------|
| RSP framing and packet dispatch (pure, unit-tested) | `kernel/gdbstub.c`, `tests/test_gdbstub.c` |
| Serial packet loop (acks, retransmit) | `kernel/gdb_serial.c`, `tests/test_gdb_serial.c` |
| Target: registers, memory, threads, stepping, breakpoints | `kernel/core/gdb_target.c` |
| Navigation (stepi, continue, searches) | `kernel/core/timetravel.c` |
| Monitor: serves COM2 while the machine is stopped | `kernel/core/monitor.c` |
