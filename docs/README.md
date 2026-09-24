# Laplace Documentation

Laplace (formerly IKOS) is a **time-traveling debugger built as an operating
system**: an x86/x86_64
microkernel that records every nondeterministic input as it runs, so any past
moment of the whole machine can be reconstructed and stepped backward (from gdb
or from an MCP client). The substrate that makes it possible is **orthogonal
persistence**: the running system is the durable state, checkpointed whole. Those
two stacks are the parts with end-to-end tests and CI; the rest of the tree
documents the broader OS scaffolding.

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
- [Reverse debugging with gdb (how-to)](testing/reverse-debugging.md) drive
  `reverse-stepi` / `reverse-continue` against a running Laplace over the serial
  port.
- [Driving time-travel from an MCP client (how-to)](testing/mcp-server.md) list
  checkpoints and drive rewind / reverse execution as JSON-RPC tools an agent can
  call.

## Architecture

Core system design and framework docs (`architecture/`):

- [Virtual Memory Manager](architecture/VMM_README.md) x86_64 paging,
  copy-on-write, the substrate persistence rides on.
- [Device Driver Framework](architecture/DEVICE_DRIVER_FRAMEWORK.md)
- [USB Driver Framework](architecture/USB_DRIVER_FRAMEWORK.md)
- [Network Stack](architecture/NETWORK_STACK.md) and
  [TCP/IP](architecture/TCPIP.md)
- [User Input Handling](architecture/USER_INPUT_HANDLING.md)

## Implementation notes

Per-subsystem reference docs live in `implementation/`. They name the concrete
source files and data structures for a subsystem and describe its APIs. Grouped
by area:

- Core: [interrupts](implementation/INTERRUPT_IMPLEMENTATION.md),
  [IPC](implementation/IPC_IMPLEMENTATION.md),
  [scheduler](implementation/SCHEDULER_IMPLEMENTATION.md),
  [threading](implementation/THREADING_IMPLEMENTATION.md),
  [kernel allocator](implementation/KALLOC_IMPLEMENTATION.md),
  [paging](implementation/PAGING_IMPLEMENTATION.md).
- Memory: [advanced memory management](implementation/ADVANCED_MEMORY_MANAGEMENT.md),
  [user-space memory](implementation/USER_SPACE_MEMORY_MANAGEMENT.md),
  [VMM summary](implementation/VMM_IMPLEMENTATION_SUMMARY.md).
- Processes: [process manager](implementation/PROCESS_MANAGER_IMPLEMENTATION.md),
  [lifecycle](implementation/PROCESS_LIFECYCLE.md),
  [termination](implementation/PROCESS_TERMINATION.md),
  [signals](implementation/ADVANCED_SIGNAL_HANDLING.md),
  [user-space execution](implementation/USER_SPACE_EXECUTION_IMPLEMENTATION.md).
- File systems: [VFS](implementation/VFS_IMPLEMENTATION.md),
  [FAT](implementation/FAT_IMPLEMENTATION.md),
  [file explorer](implementation/FILE_EXPLORER_IMPLEMENTATION.md).
- Graphics and shell: [framebuffer](implementation/FRAMEBUFFER_IMPLEMENTATION.md),
  [GUI](implementation/GUI_IMPLEMENTATION.md),
  [terminal](implementation/TERMINAL_IMPLEMENTATION.md),
  [terminal GUI](implementation/TERMINAL_GUI_IMPLEMENTATION.md),
  [shell](implementation/SHELL_IMPLEMENTATION_COMPLETE.md),
  [CLI](implementation/CLI_IMPLEMENTATION.md).
- Services: [audio](implementation/AUDIO_IMPLEMENTATION.md),
  [network](implementation/NETWORK_IMPLEMENTATION.md),
  [notifications](implementation/NOTIFICATION_IMPLEMENTATION.md),
  [daemon management](implementation/DAEMON_MANAGEMENT_IMPLEMENTATION.md) and
  [system daemons](implementation/SYSTEM_DAEMON_MANAGEMENT.md),
  [kernel logging](implementation/KERNEL_LOGGING_IMPLEMENTATION.md) and the
  [logging/debugging service](LOGGING_DEBUGGING_SERVICE.md).
- Security: [authentication and authorization](implementation/AUTHENTICATION_AUTHORIZATION.md),
  [auth integration](implementation/AUTHENTICATION_INTEGRATION.md).

## Testing

- [Bootloader and system testing guide](TESTING.md) prerequisites, QEMU, real
  hardware, and UEFI.
- [QEMU and real-hardware testing](testing/QEMU_REAL_HARDWARE_TEST.md) and the
  [USB-boot hands-on guide](testing/real_hardware_test.md).
- [Runtime kernel debugger](testing/RUNTIME_KERNEL_DEBUGGER.md).
- [Reverse debugging with gdb](testing/reverse-debugging.md) and
  [driving time-travel from MCP](testing/mcp-server.md).

The booted kernel is tested in QEMU by `make selftest`,
`tests/qemu/timetravel_e2e.py` (a heisenbug debugged backward over MCP and gdb),
and `scripts/test/qemu_persistence_demo.sh` (power cuts and resume). The
host harnesses under `scripts/test/` (`persistence_demo.sh`, `scrub_demo.sh`,
`timetravel_live_demo.sh`, ...) exercise the time-travel modules outside the
kernel. All of them run in CI (`.github/workflows/laplace.yml`). Recordings of
the QEMU runs, made with `tools/record-cast`, are under [`media/`](media/).

## Contributing to docs

- Put design and reference material in the right folder: cross-cutting design in
  `architecture/`, per-subsystem reference in `implementation/`, and how-to /
  test procedures in `testing/`.
- Update this index when you add a doc.
- Prefer living reference docs over point-in-time status reports; describe how a
  subsystem works and where its code lives, not the branch it was merged on.
