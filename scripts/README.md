# Scripts

- `build/`: third-party programs the gates run, fetched and checked by hash or
  commit and built once into `build/ext/`: `lua.sh` (Lua 5.4, static musl),
  `sqlite.sh` (the SQLite shell, static musl), `dynamic.sh` (the dynamically
  linked musl and glibc programs, with their loaders and libraries).
- `test/`: the host harnesses (`persistence_demo.sh`, `timetravel_demo.sh`,
  `scrub_demo.sh`, `mcp_heisenbug_demo.sh`, `timetravel_live_demo.sh`), which
  exercise the time-travel modules outside the kernel, and
  `qemu_persistence_demo.sh`, which cuts the power of a booted machine
  (`LAPLACE_DISK=ahci` for an AHCI disk).
- `bench/overhead.py`: the overhead table (docs/benchmarks.md).

The gates that boot the kernel are under `tests/qemu/`.
