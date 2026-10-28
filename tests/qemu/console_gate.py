#!/usr/bin/env python3
"""Console input, pipes, and poll (#249).

1. Lua's REPL runs on the console. A session is typed into COM1 a character
   at a time: assignments, a loop, clock and random reads, and a line edited
   with backspaces before Enter. The bytes are journaled at the steps they
   are delivered. When Lua exits, the self-test replays every retained epoch
   from the journal, with nothing reading the UART, and it must be byte-exact.
2. A two-process pipeline (pipe, fork, dup2, execve: producer | consumer)
   moves 300000 bytes through a pipe in writes of 1 byte to 20000 bytes, read
   with read, poll, and select. The edges are checked too: EOF, EAGAIN, a
   poll timeout, and EPIPE. Keyframes are taken mid-pipeline, so pipe buffers
   are in them, and the replay must be byte-exact.

Usage: tests/qemu/console_gate.py
"""

import os
import re
import socket
import subprocess
import sys
import tempfile
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
QEMU = os.environ.get("QEMU", "qemu-system-x86_64")
FAILS = 0


def check(good, what):
    global FAILS
    print(("[ok] " if good else "[FAIL] ") + what, flush=True)
    if not good:
        FAILS += 1
    return good


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


def seed_image(adds):
    fd, path = tempfile.mkstemp(suffix=".cpio")
    os.close(fd)
    cmd = [sys.executable, os.path.join(ROOT, "tools", "mkcpio.py"), os.path.join(ROOT, "rootfs"), path]
    for host, inside in adds:
        cmd += ["--add", f"{host}:{inside}"]
    subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL)
    return path


class Console:
    """COM1 on a socket: what the machine printed, and typing into it."""

    def __init__(self, port):
        deadline = time.monotonic() + 30
        while True:
            try:
                self.s = socket.create_connection(("127.0.0.1", port), timeout=1)
                break
            except OSError:
                if time.monotonic() > deadline:
                    raise
                time.sleep(0.1)
        self.s.settimeout(0.2)
        self.text = ""

    def pump(self):
        try:
            data = self.s.recv(65536)
            if not data:
                return False
            self.text += data.decode(errors="replace").replace("\r", "")
        except socket.timeout:
            pass
        return True

    def wait_for(self, pattern, seconds=60, start=0):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            m = re.search(pattern, self.text[start:])
            if m:
                return start + m.end()
            if not self.pump():
                break
        return None

    def type(self, keys, gap=0.01):
        for ch in keys:
            self.s.sendall(ch.encode())
            time.sleep(gap)
            self.pump()


def lua_session():
    subprocess.run(["bash", os.path.join(ROOT, "scripts", "build", "lua.sh")], check=True,
                   stdout=subprocess.DEVNULL)
    lua = os.path.join(ROOT, "build", "ext", "lua")
    seed = seed_image([(lua, "/bin/lua")])
    port = free_port()
    q = subprocess.Popen(
        [QEMU, "-m", "512M", "-display", "none", "-no-reboot",
         "-kernel", os.path.join(ROOT, "build", "laplace.elf"), "-initrd", seed,
         "-append", "run=/bin/lua interval=20 keyframes=64 selftest exit",
         "-device", "isa-debug-exit,iobase=0xf4,iosize=0x04",
         "-serial", f"tcp:127.0.0.1:{port},server=on,wait=on", "-serial", "null", "-serial", "null"],
        stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT, cwd=ROOT)
    try:
        con = Console(port)
        at = con.wait_for(r"\n> ")
        check(at is not None, "lua: the REPL starts on the console and prompts")
        lines = [
            "x = 6 * 7\n",
            "t = {} for i = 1, 1000 do t[i] = i * i end\n",
            "print('answer', x, t[1000])\n",
            "print('clock', os.clock() >= 0, 'random', math.random(1, 9) >= 1)\n",
            "prnt\x7f\x7fint('edited ' .. #t)\n",          # "pr" + "int(...)" after two erases
        ]
        for line in lines:
            con.type(line)
            at = con.wait_for(r"\n> ", 60, at or 0)
        con.type("os.exit(0)\n")
        con.wait_for(r"selftest: (PASSED|FAILED)", 600)
        q.wait(timeout=120)
        text = con.text
        for line in text.splitlines():
            if re.search(r"^(answer|clock|edited)|selftest: (inputs|verify|PASSED|FAILED)", line):
                print("  [console] " + line.strip()[:150], flush=True)
        check(re.search(r"(?m)^answer\t42\t1000000$", text) is not None,
              "lua: the typed statements run (answer 42 1000000)")
        check(re.search(r"(?m)^clock\ttrue\trandom\ttrue$", text) is not None,
              "lua: clock and random reads in the typed session")
        check(re.search(r"(?m)^edited 1000$", text) is not None,
              "lua: a line edited with backspaces runs as corrected")
        check("prnt\b \b\b \bint('edited ' .. #t)" in text,
              "lua: typing is echoed, and erased characters are echoed away")
        m = re.search(r"selftest: verify rc=0: (\d+) epochs replayed, 0 diverged", text)
        check(m is not None and int(m.group(1)) >= 1,
              "lua: the session replays byte-exact from the journal, no input attached"
              + (f" ({m.group(1)} epochs)" if m else ""))
        check("selftest: PASSED" in text, "lua: the self-test passes (perturbed replays are caught)")
    finally:
        q.kill()
        os.unlink(seed)


def pipeline():
    r = subprocess.run(
        [QEMU, "-m", "512M", "-display", "none", "-no-reboot",
         "-kernel", os.path.join(ROOT, "build", "laplace.elf"),
         "-append", "run=musl_pipeline interval=3 keyframes=64 selftest exit",
         "-device", "isa-debug-exit,iobase=0xf4,iosize=0x04",
         "-serial", "stdio", "-serial", "null", "-serial", "null"],
        stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=900, cwd=ROOT)
    text = r.stdout.decode(errors="replace").replace("\r", "")
    for line in text.splitlines():
        if re.search(r"^(consumer|pipeline):|selftest: (window|verify|PASSED|FAILED)", line):
            print("  [pipeline] " + line.strip()[:150], flush=True)
    check(re.search(r"consumer: 300000 bytes, 0 wrong, hash [0-9a-f]+, after \d+ polls and \d+ selects", text)
          is not None, "pipeline: 300000 bytes arrive intact through read, poll, and select")
    check("pipeline: producer exit 0, consumer exit 0" in text, "pipeline: both ends exit 0")
    check("pipeline: EOF, EAGAIN, a poll timeout, and EPIPE as expected" in text,
          "pipeline: EOF, EAGAIN, a poll timeout, and EPIPE")
    w = re.search(r"selftest: window epochs (\d+)\.\.(\d+)", text)
    check(w is not None and int(w.group(1)) == 1 and int(w.group(2)) > 3,
          "pipeline: keyframes are taken mid-pipeline and the window holds the whole run")
    m = re.search(r"selftest: verify rc=0: (\d+) epochs replayed, 0 diverged", text)
    check(m is not None, "pipeline: every epoch replays byte-exact")
    check("selftest: PASSED" in text, "pipeline: the self-test passes")


def main():
    if not os.path.exists(os.path.join(ROOT, "build", "laplace.elf")):
        subprocess.run(["make", "-s"], cwd=ROOT, check=True)
    lua_session()
    pipeline()
    if FAILS:
        print(f"FAILED: {FAILS} console, pipe, or poll checks")
        sys.exit(1)
    print("PASSED: a typed Lua session and a pipeline record and replay byte-exact")


if __name__ == "__main__":
    main()
