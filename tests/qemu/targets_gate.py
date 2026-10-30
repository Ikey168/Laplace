#!/usr/bin/env python3
"""Gates for practical targets (#256): hello world, Lua, and SQLite.

Each target is an unmodified static musl program. For each one:

  1. It runs under recording to its end, and every retained epoch replays
     byte-exact (the in-kernel self-test's verify).
  2. Replay fails when it is sabotaged: with `sabotage=clock` every replayed
     clock read is one tick (1 ms) late, with `sabotage=entropy` one bit of
     every replayed random payload is flipped, and verify reports divergence.
  3. It is debugged backward over MCP (watch_last_write lands on the store
     that last changed a value) and over stock gdb with no file argument
     (gdb reads the program from the target; reverse-continue runs back to a
     breakpoint in the program).

The pthreads race, the fourth target, is tests/qemu/threads_gate.sh.

Usage: tests/qemu/targets_gate.py [hello|lua|sqlite ...]   (default: all)
"""

import json
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


def show(tag, text, pattern):
    for line in text.splitlines():
        if re.search(pattern, line):
            print(f"  [{tag}] {line.strip()[:150]}", flush=True)


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


def seed_image(adds):
    """The rootfs/ seed image with programs added (host path -> path in it)."""
    fd, path = tempfile.mkstemp(suffix=".cpio")
    os.close(fd)
    cmd = [sys.executable, os.path.join(ROOT, "tools", "mkcpio.py"), os.path.join(ROOT, "rootfs"), path]
    for host, inside in adds:
        cmd += ["--add", f"{host}:{inside}"]
    subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL)
    return path


def qemu_args(initrd, append, mem):
    a = [QEMU, "-m", mem, "-display", "none", "-no-reboot", "-kernel", os.path.join(ROOT, "build", "laplace.elf")]
    if initrd:
        a += ["-initrd", initrd]
    return a + ["-append", append]


def record_and_verify(initrd, append, mem="512M"):
    """Boot, record to the end, verify every epoch; the console text."""
    args = qemu_args(initrd, append + " selftest exit", mem)
    args += ["-device", "isa-debug-exit,iobase=0xf4,iosize=0x04",
             "-serial", "stdio", "-serial", "null", "-serial", "null"]
    r = subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=1800, cwd=ROOT)
    return r.stdout.decode(errors="replace").replace("\r", "")


class Mcp:
    def __init__(self, port):
        self.proc = subprocess.Popen([sys.executable, os.path.join(ROOT, "tools", "laplace-mcp"), "--port", str(port)],
                                     stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, bufsize=1)
        self.n = 0
        self.call("initialize", {"protocolVersion": "2024-11-05", "capabilities": {},
                                 "clientInfo": {"name": "targets_gate", "version": "1"}})

    def call(self, method, params=None):
        self.n += 1
        msg = {"jsonrpc": "2.0", "id": self.n, "method": method}
        if params is not None:
            msg["params"] = params
        self.proc.stdin.write(json.dumps(msg) + "\n")
        self.proc.stdin.flush()
        return json.loads(self.proc.stdout.readline())

    def tool(self, name, **args):
        r = self.call("tools/call", {"name": name, "arguments": args})
        text = r["result"]["content"][0]["text"]
        print(f"  [mcp] {name}({', '.join(f'{k}={v:#x}' if isinstance(v, int) and v > 9999 else f'{k}={v}' for k, v in args.items())}): {text.strip()[:130]}", flush=True)
        return text

    def close(self):
        self.proc.kill()


def field(text, key):
    m = re.search(r"\b%s=(0x[0-9a-f]+|\d+)" % key, text)
    return int(m.group(1), 0) if m else None


class Machine:
    """A recording stopped at its end, with gdb on COM2 and MCP on COM3."""

    def __init__(self, initrd, append, mem="512M"):
        self.gdb_port, self.mcp_port = free_port(), free_port()
        fd, self.console = tempfile.mkstemp(suffix=".log")
        os.close(fd)
        args = qemu_args(initrd, append, mem) + [
            "-serial", f"file:{self.console}",
            "-serial", f"tcp:127.0.0.1:{self.gdb_port},server,nowait",
            "-serial", f"tcp:127.0.0.1:{self.mcp_port},server,nowait"]
        self.q = subprocess.Popen(args, cwd=ROOT)
        for _ in range(3600):
            time.sleep(0.5)
            if "monitor: stopped" in self.text():
                break

    def text(self):
        return open(self.console, errors="replace").read()

    def gdb(self, *cmds):
        with tempfile.TemporaryDirectory() as d:     # no program file at hand
            args = ["gdb", "-batch", "-nx", "-ex", "set remotetimeout 600",
                    "-ex", f"target remote 127.0.0.1:{self.gdb_port}"]
            for c in cmds:
                args += ["-ex", c]
            r = subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=1800, cwd=d)
        out = r.stdout.decode(errors="replace")
        for line in out.splitlines():
            if re.search(r"Reading symbols|Breakpoint|hit|^#0|^\$", line):
                print(f"  [gdb] {line.strip()[:140]}", flush=True)
        return out

    def close(self):
        self.q.kill()
        self.q.wait()
        os.unlink(self.console)


def symbols(elf):
    """name -> (address, size) from the program's symbol table."""
    out = subprocess.run(["nm", "-S", elf], stdout=subprocess.PIPE, text=True).stdout
    syms = {}
    for line in out.splitlines():
        p = line.split()
        if len(p) == 4:
            syms[p[3]] = (int(p[0], 16), int(p[1], 16))
        elif len(p) == 3:
            syms[p[2]] = (int(p[0], 16), 0)
    return syms


def replay_checks(tag, initrd, append, mem="512M"):
    out = record_and_verify(initrd, append, mem)
    show(tag, out, r"^(hello|lua:|rows|first|after|random|pages|ok$)|selftest: (window|inputs|verify rc)")
    check(re.search(r"selftest: verify rc=0: [0-9]+ epochs replayed, 0 diverged", out) is not None,
          f"{tag}: every retained epoch replays byte-exact")
    for kind in ("clock", "entropy"):
        bad = record_and_verify(initrd, append + f" sabotage={kind}", mem)
        m = re.search(r"selftest: verify rc=[-0-9]+: ([0-9]+) epochs replayed, ([0-9]+) diverged", bad)
        show(tag, bad, r"selftest: (verify rc|first divergence)")
        check(m is not None and int(m.group(2)) > 0,
              f"{tag}: a replay with sabotage={kind} is caught")
    return out


def gate_hello():
    elf = os.path.join(ROOT, "build", "user", "musl_hello.elf")
    out = replay_checks("hello", None, "run=musl_hello interval=20 keyframes=64")
    check("hello from musl_hello on Linux x86_64 (argc 1, pid 1, tls 42, getrandom 8, clock ok)" in out,
          "hello: the program's output")
    syms = symbols(elf)
    m = Machine(None, "run=musl_hello interval=20 keyframes=64")
    try:
        mcp = Mcp(m.mcp_port)
        hit = mcp.tool("watch_last_write", pid=1, addr=syms["g_greeted"][0], len=4)
        main_lo, main_sz = syms["main"]
        check(main_lo <= (field(hit, "rip") or 0) < main_lo + main_sz,
              "hello: MCP watch_last_write lands on main's store to g_greeted")
        mcp.close()
        g = m.gdb("break main", "reverse-continue", "bt 1")
        check("Reading symbols from target:/musl_hello" in g, "hello: gdb reads the program from the target")
        check(re.search(r"Breakpoint 1, (0x[0-9a-f]+ in )?main ", g) is not None,
              "hello: gdb reverse-continues to a breakpoint in main")
    finally:
        m.close()


def gate_lua():
    subprocess.run(["bash", os.path.join(ROOT, "scripts", "build", "lua.sh")], check=True)
    lua = os.path.join(ROOT, "build", "ext", "lua")
    seed = seed_image([(lua, "/bin/lua")])
    run = 'run="/bin/lua /scripts/gate.lua" interval=10 keyframes=64'
    try:
        out = replay_checks("lua", seed, run)
        check(re.search(r"lua: acc -?[0-9.]+, box 4242, kept 2500 of 5000, file 5000 bytes, sum 500500", out)
              is not None, "lua: floats, tables, the collector, and file I/O give the expected results")
        syms = symbols(lua)
        m = Machine(seed, run)
        try:
            text = m.text()
            box = re.search(r"lua: box (0x[0-9a-f]+)", text)
            check(box is not None, "lua: the script names its table")
            mcp = Mcp(m.mcp_port)
            # Table.array (TValue*) is at offset 16 in Lua 5.4; box[1] is its
            # first value. At the end lua_close has freed it: step back to
            # where the table still lives (the script's last print).
            array = 0
            for _ in range(200):
                arr = mcp.tool("read_memory", pid=1, addr=int(box.group(1), 16) + 16, len=8)
                if field(arr, "u64"):
                    array = field(arr, "u64")
                    break
                if "no further history" in mcp.tool("reverse_step"):
                    break
            hit = mcp.tool("watch_last_write", pid=1, addr=array, len=8)
            lo, sz = syms["luaV_execute"]
            check(lo <= (field(hit, "rip") or 0) < lo + sz,
                  "lua: MCP watch_last_write on box[1] lands in the interpreter's store (luaV_execute)")
            val = mcp.tool("read_memory", pid=1, addr=array, len=8)
            check(field(val, "u64") == 4242, "lua: ... where box[1] has just become 4242")
            mcp.close()
            g = m.gdb("break luaH_resize", "reverse-continue", "bt 1")
            check("Reading symbols from target:/bin/lua" in g, "lua: gdb reads the interpreter from the target")
            check(re.search(r"Breakpoint 1, (0x[0-9a-f]+ in )?luaH_resize ", g) is not None,
                  "lua: gdb reverse-continues to a breakpoint in the interpreter (luaH_resize)")
        finally:
            m.close()
    finally:
        os.unlink(seed)


def gate_sqlite():
    subprocess.run(["bash", os.path.join(ROOT, "scripts", "build", "sqlite.sh")], check=True)
    sq = os.path.join(ROOT, "build", "ext", "sqlite3")
    seed = seed_image([(sq, "/bin/sqlite3")])
    run = 'run="/bin/sqlite3 -batch -init /scripts/sqlite.sql /tmp/gate.db .quit" interval=25 keyframes=64'
    try:
        out = replay_checks("sqlite", seed, run)
        for want in ("rows|20000|137043.857", "first|w00000", "after|16000|146179.000", "ok"):
            check(re.search(r"(?m)^" + re.escape(want) + "$", out) is not None, f"sqlite: the script prints {want}")
        syms = symbols(sq)
        m = Machine(seed, run)
        try:
            mcp = Mcp(m.mcp_port)
            # sqlite3Stat: the library's memory statistics, updated on every
            # allocation and free.
            hit = mcp.tool("watch_last_write", pid=1, addr=syms["sqlite3Stat"][0], len=8)
            rip = field(hit, "rip") or 0
            inside = [n for n, (a, s) in syms.items() if s and a <= rip < a + s]
            print(f"  [mcp] the write is in {inside}", flush=True)
            check(any(n.startswith("sqlite3") for n in inside),
                  "sqlite: MCP watch_last_write on its memory statistics lands in SQLite's code")
            mcp.close()
            g = m.gdb("break sqlite3_step", "reverse-continue", "bt 1")
            check("Reading symbols from target:/bin/sqlite3" in g, "sqlite: gdb reads the shell from the target")
            check(re.search(r"Breakpoint 1, (0x[0-9a-f]+ in )?sqlite3_step ", g) is not None,
                  "sqlite: gdb reverse-continues to the last sqlite3_step")
        finally:
            m.close()
    finally:
        os.unlink(seed)


def main():
    gates = {"hello": gate_hello, "lua": gate_lua, "sqlite": gate_sqlite}
    for name in (sys.argv[1:] or list(gates)):
        print(f"--- {name}", flush=True)
        gates[name]()
    if FAILS:
        print(f"FAILED: {FAILS} check(s)")
        return 1
    print("PASSED: hello, Lua, and SQLite record, replay byte-exact, catch sabotage, and debug backward over MCP and gdb")
    return 0


if __name__ == "__main__":
    sys.exit(main())
