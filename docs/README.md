# Laplace Documentation

Laplace (formerly IKOS) is a **time-traveling debugger built as an operating
system**: an x86/x86_64
microkernel that records every nondeterministic input as it runs, so any past
moment of the whole machine can be reconstructed and stepped backward (from gdb
or from an MCP client). The substrate that makes it possible is **orthogonal
persistence**: the running system is the durable state, checkpointed whole. Both
stacks, and the kernel they run in, are tested end to end in CI.

Start with the root [`README.md`](../README.md) for the project overview and the
"try it" demos. This folder holds the design and reference docs.

## Headline features

- [Orthogonal persistence (design)](architecture/orthogonal-persistence.md) how
  checkpoint/restore works: stop-the-world COW marking, page-fault capture,
  double-buffered on-disk store with a single superblock-flip commit.
- [Orthogonal persistence v2 (design)](architecture/orthogonal-persistence-v2.md)
  the consistency model and the plan for persisting kernel and driver state.
- [Time-travel debugging (design + module map)](architecture/time-travel.md) the
  deterministic replay core and the MCP interface, with a table mapping every
  stage to the module that ships it.
- [Platforms (TCG, KVM, bare metal)](architecture/hardware.md) PMU detection
  at boot, the AHCI disk, and what each platform has been checked to do.
- [Reverse debugging with gdb (how-to)](testing/reverse-debugging.md) drive
  `reverse-stepi` / `reverse-continue` against a running Laplace over the serial
  port.
- [Driving time-travel from an MCP client (how-to)](testing/mcp-server.md) list
  checkpoints and drive rewind / reverse execution as JSON-RPC tools an agent can
  call.

## Architecture

- [Time-travel debugging](architecture/time-travel.md): the deterministic
  replay core, its inputs, the position model, and the module map.
- [Orthogonal persistence](architecture/orthogonal-persistence.md) and
  [persistence v2](architecture/orthogonal-persistence-v2.md).
- [Platforms](architecture/hardware.md): TCG, KVM, PMU detection at boot, the
  AHCI disk, and what bare metal still needs.
- [Overhead](benchmarks.md): recording, keyframes, replay, and rewind, under
  TCG and KVM, with budgets.

The kernel's own subsystems are documented in their headers
(`include/core/*.h`): processes and threads, virtual memory, files, the tty,
pipes and poll, signals, the network stack and sockets, and the drivers (IDE,
AHCI, e1000).

## Testing

- [Reverse debugging with gdb](testing/reverse-debugging.md) and
  [driving time-travel from MCP](testing/mcp-server.md).

The booted kernel is tested in QEMU by `make selftest`, the gates under
`tests/qemu/` (each one names what it shows in its header), and
`scripts/test/qemu_persistence_demo.sh` (power cuts and resume, on IDE and
AHCI). The host harnesses under `scripts/test/` (`persistence_demo.sh`,
`scrub_demo.sh`, `timetravel_live_demo.sh`, ...) exercise the time-travel
modules outside the kernel. All of them run in CI
(`.github/workflows/laplace.yml`). Recordings of the QEMU runs, made with
`tools/record-cast`, are under [`media/`](media/).

## Contributing to docs

- Put cross-cutting design in `architecture/` and how-to / test procedures in
  `testing/`; a subsystem's reference belongs in its header.
- Update this index when you add a doc.
- Prefer living reference docs over point-in-time status reports; describe how a
  subsystem works and where its code lives, not the branch it was merged on.
