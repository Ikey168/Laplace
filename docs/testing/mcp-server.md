# Driving time travel from MCP

Laplace's kernel is an MCP server. It speaks newline-delimited JSON-RPC on its
third serial port (COM3), implementing the protocol itself: `initialize`,
notifications, `ping`, `tools/list` (with input schemas), and `tools/call`.
`tools/laplace-mcp` is a stdio bridge to that port, so any MCP client can
launch it like a local server.

## Connect a client

```
$ make run                    # QEMU: MCP (COM3) on 127.0.0.1:1236
$ claude mcp add laplace -- /path/to/Laplace/tools/laplace-mcp --port 1236
```

Or in any client's JSON config:

```json
{ "mcpServers": { "laplace": { "command": "/path/to/Laplace/tools/laplace-mcp",
                               "args": ["--port", "1236"] } } }
```

A request that arrives while the machine is running live stops it at its next
kernel entry, then gets answered from the stopped machine.

## Tools

Positions are `(epoch, offset, insn)`: `offset` counts kernel entries (system
calls and faults) into the epoch; `insn` 0 is the moment an entry traps, 1 the
moment after it was handled, k after k-1 further instructions.

| Tool | Arguments | What it does |
|------|-----------|--------------|
| `where` | | Position, current process, rip, and why the machine stopped |
| `list_checkpoints` | | The retained keyframe window (rewind horizon) |
| `list_processes` | | Processes alive at the current position |
| `rewind_to` | `epoch`, `offset`, `insn`? | Reconstruct that moment by re-executing |
| `reverse_step` / `step_forward` | | One kernel entry back / forward |
| `reverse_stepi` / `stepi` | | One instruction back / forward |
| `read_memory` | `pid`, `addr` (number or `"0x..."`), `len` (≤256) | Bytes at the current position |
| `get_registers` | `pid` | Registers at the current position |
| `watch_last_write` | `pid`, `addr`, `len` (1-8) | Land where those bytes last changed, at or before now: the instruction after the store, or the system call that wrote them |
| `verify_replay` | | Re-execute every retained epoch and compare checksums of memory, registers, process table, and scheduler |
| `resume` | | Leave history and run live (recording) until the next stop |

## An agent's session

From `tests/qemu/timetravel_e2e.py`, which runs this in CI:

```
mcp> where()
     position epoch=14 offset=1689 insn=0 pid=1 rip=0x400001e1 stop=break: pid 1 asserted (code bad) (end of the recording)
mcp> read_memory(pid=1, addr=0x40001100, len=4)
     pid=1 addr=0x40001100 len=4 bytes=e4290000 u32=10724
mcp> watch_last_write(pid=1, addr=0x40001100, len=4)
     last write at epoch=14 offset=1616 insn=26 pid=1 rip=0x400000b3
mcp> get_registers(pid=1)
     pid=1 rax=0x40 ... rip=0x400000b3 rflags=0x200
mcp> reverse_stepi()
     now at epoch=14 offset=1616 insn=25 pid=1 rip=0x400000ac
mcp> read_memory(pid=1, addr=0x40001100, len=4)
     pid=1 addr=0x40001100 len=4 bytes=30000000 u32=48
mcp> verify_replay()
     byte-exact: epochs_checked=14 epochs_diverged=0
```

## Implementation

| Piece | File |
|-------|------|
| Protocol and tools (pure, unit-tested) | `kernel/mcp.c`, `tests/test_mcp.c` |
| Line loop | `kernel/mcp_server.c`, `tests/test_mcp_server.c` |
| Tools bound to the machine | `kernel/mcp_sync.c` |
| Monitor: serves COM3 while the machine is stopped | `kernel/core/monitor.c` |
| stdio bridge | `tools/laplace-mcp` |
