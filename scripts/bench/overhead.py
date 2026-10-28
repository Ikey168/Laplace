#!/usr/bin/env python3
"""Recording overhead, per workload (#255).

Each workload boots twice: once with `norecord` (no keyframes, journals, or
preemption points: the baseline) and once recording, both with `bench`,
which makes the kernel print what the run cost and how fast the recording is
navigated afterwards. All times are the guest's, from the start of the first
program, so boot and QEMU start-up are not counted.

Columns:
  run          baseline run time, without recording
  recorded     run time while recording, and its ratio to the baseline
  keyframes    keyframes taken, and keyframe page bytes written per second
  other disk   the rest the store wrote (journals, input payloads, metadata)
  verify       re-executing every retained epoch and comparing it
  replay       re-executing every retained epoch, without the comparisons,
               as a multiple of the recorded run time
  rewind       restoring the oldest retained keyframe (rewind_to)
  rstepi       reverse-stepi from the end: mean and worst of 10

Run times are the fastest of --repeat boots (default 3).

Usage: scripts/bench/overhead.py [workload ...] [--repeat N] [--out FILE]
       QEMU_ACCEL=kvm scripts/bench/overhead.py     (under KVM)
Workloads: hello lua sqlite threads cpu alloc (default: all)
"""

import os
import re
import subprocess
import sys
import tempfile
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
QEMU = os.environ.get("QEMU", "qemu-system-x86_64")
ACCEL = ["-enable-kvm", "-cpu", "host"] if os.environ.get("QEMU_ACCEL") == "kvm" else []


def seed_image(adds):
    fd, path = tempfile.mkstemp(suffix=".cpio")
    os.close(fd)
    cmd = [sys.executable, os.path.join(ROOT, "tools", "mkcpio.py"), os.path.join(ROOT, "rootfs"), path]
    for host, inside in adds:
        cmd += ["--add", f"{host}:{inside}"]
    subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL)
    return path


def ext(name):
    subprocess.run(["bash", os.path.join(ROOT, "scripts", "build", f"{name}.sh")], check=True,
                   stdout=subprocess.DEVNULL)
    return os.path.join(ROOT, "build", "ext", name if name != "sqlite" else "sqlite3")


# name -> (description, seed image adds or None, run= value)
WORKLOADS = {
    "hello": ("hello world (musl)", None, "musl_hello"),
    "lua": ("Lua 5.4: the gate script", [("lua", "/bin/lua")], '"/bin/lua /scripts/gate.lua"'),
    "sqlite": ("SQLite shell: 20000 rows, an index, queries", [("sqlite", "/bin/sqlite3")],
               '"/bin/sqlite3 -batch -init /scripts/sqlite.sql /tmp/gate.db .quit"'),
    "threads": ("pthreads: 2 threads, 300000 racy increments", None, '"musl_threads race 300000"'),
    "cpu": ("compute-bound: a sieve, 4 rounds", None, '"musl_benchcpu 4"'),
    "alloc": ("allocation-heavy: 100000 malloc/free", None, '"musl_benchalloc 100000"'),
}


def boot(initrd, append, nav=True):
    args = [QEMU, *ACCEL, "-m", "512M", "-display", "none", "-no-reboot",
            "-kernel", os.path.join(ROOT, "build", "laplace.elf"),
            "-device", "isa-debug-exit,iobase=0xf4,iosize=0x04",
            "-serial", "stdio", "-serial", "null", "-serial", "null"]
    if initrd:
        args += ["-initrd", initrd]
    args += ["-append", append + (" bench exit" if nav else " bench=run exit")]
    r = subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=3600, cwd=ROOT)
    text = r.stdout.decode(errors="replace").replace("\r", "")
    vals = {}
    for line in text.splitlines():
        words = line.split()
        if len(words) < 2 or words[0] != "bench:":
            continue
        kind = words[1]
        for w in words[2:]:
            k, _, v = w.partition("=")
            if v.lstrip("-").isdigit():
                vals[k if kind in ("norecord", "record") else f"{kind}.{k}"] = int(v)
    if "run_us" not in vals:
        sys.exit(f"bench: no result from: {append}\n{text[-2000:]}")
    return vals


def ms(us):
    return f"{us / 1000:.1f}" if us < 100000 else f"{us / 1000:.0f}"


def rate(nbytes, us):
    if us <= 0:
        return "-"
    per_s = nbytes * 1e6 / us
    return f"{per_s / 1048576:.1f} MiB/s" if per_s >= 1048576 else f"{per_s / 1024:.0f} KiB/s"


def measure(name, repeat):
    """Run times are the fastest of `repeat` boots (QEMU on a shared host is
    noisy); the navigation numbers come from the first recording."""
    desc, adds, run = WORKLOADS[name]
    seed = seed_image([(ext(h), inside) for h, inside in adds]) if adds else None
    try:
        append = f"run={run} keyframes=64"
        base = min((boot(seed, append + " norecord", False) for _ in range(repeat)),
                   key=lambda v: v["run_us"])
        rec = boot(seed, append)
        for _ in range(repeat - 1):
            again = boot(seed, append, False)
            if again["run_us"] < rec["run_us"]:
                rec.update({k: v for k, v in again.items() if "." not in k})
    finally:
        if seed:
            os.unlink(seed)
    return desc, base, rec


def row(name, desc, base, rec):
    run, rrun = base["run_us"], rec["run_us"]
    kf = rec.get("keyframe_bytes", 0)
    other = max(rec.get("disk_bytes", 0) - kf, 0)
    replay = rec.get("replay.us", 0)
    return [
        f"{name}: {desc}",
        ms(run),
        f"{ms(rrun)} ({rrun / max(run, 1):.2f}x)",
        f"{rec.get('keyframes', 0)}, {rate(kf, rrun)}",
        rate(other, rrun),
        f"{ms(rec.get('verify.us', 0))} ({rec.get('verify.epochs', 0)} epochs, "
        f"{rec.get('verify.diverged', 0)} diverged)",
        f"{ms(replay)} ({replay / max(rrun, 1):.1f}x)",
        ms(rec.get("rewind.us", 0)),
        f"{ms(rec.get('rstepi.avg_us', 0))} / {ms(rec.get('rstepi.max_us', 0))}",
    ]


HEAD = ["workload", "run ms", "recorded ms", "keyframes", "other disk", "verify ms", "replay ms",
        "rewind ms", "rstepi ms"]


def main():
    argv = sys.argv[1:]
    out = None
    repeat = 3
    if "--repeat" in argv:
        i = argv.index("--repeat")
        repeat = max(1, int(argv[i + 1]))
        del argv[i:i + 2]
    if "--out" in argv:
        i = argv.index("--out")
        out = argv[i + 1]
        del argv[i:i + 2]
    names = argv or list(WORKLOADS)
    for n in names:
        if n not in WORKLOADS:
            sys.exit(f"unknown workload {n}; one of {' '.join(WORKLOADS)}")
    subprocess.run(["make", "-s"], cwd=ROOT, check=True)
    accel = "KVM (-enable-kvm -cpu host)" if ACCEL else "TCG"
    cpu = ""
    try:
        with open("/proc/cpuinfo") as f:
            m = re.search(r"model name\s*:\s*(.*)", f.read())
            cpu = m.group(1).strip() if m else ""
    except OSError:
        pass
    rows = []
    for n in names:
        t0 = time.time()
        desc, base, rec = measure(n, repeat)
        rows.append(row(n, desc, base, rec))
        print(f"[{n}] measured in {time.time() - t0:.0f}s: " + " | ".join(rows[-1][1:]), flush=True)
    lines = [f"Accelerator: {accel}; run times: fastest of {repeat}; host CPU: {cpu or 'unknown'}; QEMU: "
             + subprocess.run([QEMU, "--version"], stdout=subprocess.PIPE, text=True).stdout.splitlines()[0],
             "",
             "| " + " | ".join(HEAD) + " |",
             "|" + "|".join("---" for _ in HEAD) + "|"]
    lines += ["| " + " | ".join(r) + " |" for r in rows]
    table = "\n".join(lines)
    print()
    print(table)
    if out:
        with open(out, "w") as f:
            f.write(table + "\n")


if __name__ == "__main__":
    main()
