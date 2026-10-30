#!/usr/bin/env python3
"""Dynamically linked programs (#254).

1. musl: hello world built by musl-gcc without -static. The kernel loads the
   program and the loader its PT_INTERP names (/lib/ld-musl-x86_64.so.1, from
   the seed image) and starts the loader with AT_BASE set.
2. glibc: the distribution's lua5.4 package (not rebuilt), with its loader
   (/lib64/ld-linux-x86-64.so.2) and the libraries ldd lists (libc, libm,
   libreadline, libtinfo) copied into the seed image. It runs the Lua gate
   script.

Each one records to its end and every retained epoch replays byte-exact, a
sabotaged replay is caught (glibc), and stock gdb, with no file argument,
lists the shared libraries the loader mapped (qXfer:libraries-svr4:read),
loads their symbols, and reverse-continues to a breakpoint in one.

Usage: tests/qemu/dynamic_gate.py
"""

import os
import re
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import targets_gate as tg  # noqa: E402

ROOT = tg.ROOT


def manifest(name):
    adds = []
    with open(os.path.join(ROOT, "build", "ext", "dyn", name)) as f:
        for line in f:
            host, inside = line.strip().split(":", 1)
            adds.append((host if os.path.isabs(host) else os.path.join(ROOT, host), inside))
    return adds


def gate_musl():
    seed = tg.seed_image(manifest("musl.manifest"))
    run = "run=/bin/hello_dyn interval=5 keyframes=64"
    try:
        out = tg.record_and_verify(seed, run)
        tg.show("musl", out, r"^hello|selftest: (window|verify rc)|load:")
        tg.check(re.search(r"hello from /bin/hello_dyn on Linux x86_64 .*tls 42.*clock ok", out) is not None,
                 "musl: a dynamically linked program starts through ld-musl and runs")
        tg.check(re.search(r"selftest: verify rc=0: [0-9]+ epochs replayed, 0 diverged", out) is not None,
                 "musl: every retained epoch replays byte-exact")
        m = tg.Machine(seed, run)
        try:
            g = m.gdb("info sharedlibrary", "break main", "reverse-continue", "bt 1")
            tg.check(re.search(r"Yes\s.*/lib/ld-musl-x86_64\.so\.1", g) is not None,
                     "musl: gdb lists the loader as a shared library, with symbols")
            tg.check(re.search(r"Breakpoint 1, (0x[0-9a-f]+ in )?main ", g) is not None,
                     "musl: gdb reverse-continues to main in the PIE")
        finally:
            m.close()
    finally:
        os.unlink(seed)


def gate_glibc():
    seed = tg.seed_image(manifest("glibc.manifest"))
    run = 'run="/usr/bin/lua5.4 /scripts/gate.lua" interval=10 keyframes=64'
    try:
        out = tg.replay_checks("glibc", seed, run)
        tg.check(re.search(r"lua: acc -?[0-9.]+, box 4242, kept 2500 of 5000, file 5000 bytes, sum 500500", out)
                 is not None, "glibc: the distribution's lua5.4 runs the gate script with the expected results")
        bad = re.findall(r"linux: pid \d+: system call (\d+) is not implemented", out)
        print(f"  [glibc] system calls answered ENOSYS: {sorted(set(bad)) or 'none'}", flush=True)
        m = tg.Machine(seed, run)
        try:
            g = m.gdb("info sharedlibrary", "break write", "reverse-continue", "bt 1")
            for lib in ("libc.so.6", "libm.so.6", "libreadline.so.8"):
                tg.check(re.search(r"Yes\s.*/lib/x86_64-linux-gnu/" + re.escape(lib), g) is not None,
                         f"glibc: gdb loads {lib}'s symbols from the target")
            tg.check(re.search(r"Breakpoint 1, (0x[0-9a-f]+ in )?(__GI___libc_|__libc_)?write \(", g) is not None,
                     "glibc: gdb reverse-continues to the last write() in libc")
        finally:
            m.close()
    finally:
        os.unlink(seed)


def main():
    subprocess.run(["bash", os.path.join(ROOT, "scripts", "build", "dynamic.sh")], check=True)
    if not os.path.exists(os.path.join(ROOT, "build", "laplace.elf")):
        subprocess.run(["make", "-s"], cwd=ROOT, check=True)
    print("--- musl", flush=True)
    gate_musl()
    print("--- glibc", flush=True)
    gate_glibc()
    if tg.FAILS:
        print(f"FAILED: {tg.FAILS} checks of dynamically linked programs")
        sys.exit(1)
    print("PASSED: dynamically linked musl and glibc programs record, replay byte-exact, and debug with "
          "their shared libraries")


if __name__ == "__main__":
    main()
