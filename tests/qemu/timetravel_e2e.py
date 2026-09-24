#!/usr/bin/env python3
"""In-QEMU end-to-end gate for the time-traveling debugger (#230).

Boots the Laplace kernel in QEMU with the planted heisenbug
(user/laplace/heisenbug.c), waits for it to fire, and then debugs the recorded
machine backward the way a user or an agent would:

  1. MCP, through tools/laplace-mcp (the stdio bridge an MCP client launches):
     the stop, the clobbered batch_limit, watch_last_write to the exact store
     that clobbered it, the registers there (slot 64 of a 64-slot ring), one
     instruction back (the old value), verify_replay (byte-exact).
  2. gdb, stock, over the gdb remote protocol: a hardware watchpoint plus
     reverse-continue stops on the same store; a breakpoint plus
     reverse-continue and continue; reverse-stepi; monitor verify.
  3. MCP again: resume the machine live past the bug (it keeps recording),
     interrupt it, and verify the extended recording byte-exact.

Every expectation is checked against the heisenbug's own ELF (symbol and
instruction addresses from nm/objdump), and the script exits non-zero on the
first failure. The narrated output is the README recording.
"""

import json
import os
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
KERNEL = os.path.join(ROOT, "build", "laplace.elf")
ELF = os.path.join(ROOT, "build", "user", "heisenbug.elf")
QEMU = os.environ.get("QEMU", "qemu-system-x86_64")
GDB = os.environ.get("GDB", "gdb")

failures = 0


def say(msg):
    print(msg, flush=True)


def check(cond, msg):
    global failures
    say(f"  [{'ok' if cond else 'FAIL'}] {msg}")
    if not cond:
        failures += 1
        raise SystemExit(finish())


def finish():
    if failures:
        say("FAILED")
        return 1
    say("PASSED: recorded a heisenbug in the booted OS and found the clobbering store "
        "backward, over MCP and gdb, with every reconstructed epoch byte-exact")
    return 0


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def elf_facts():
    """Addresses the checks rely on, read from the heisenbug binary."""
    nm = subprocess.run(["nm", ELF], capture_output=True, text=True, check=True).stdout
    sym = {m.group(2): int(m.group(1), 16)
           for m in re.finditer(r"^([0-9a-f]+) \w (\w+)$", nm, re.M)}
    dis = subprocess.run(["objdump", "-d", "--no-show-raw-insn", ELF],
                         capture_output=True, text=True, check=True).stdout
    # The store into the ring: mov %edi,0x<g>(,%rax,4) inside produce().
    store = nxt = None
    in_produce = False
    lines = dis.splitlines()
    for i, line in enumerate(lines):
        if re.match(r"^[0-9a-f]+ <produce>:", line):
            in_produce = True
        elif in_produce and re.match(r"^[0-9a-f]+ <", line):
            break
        elif in_produce and re.search(r"mov\s+%edi," + "0x%x" % sym["g"] + r"\(,%rax,4\)", line):
            store = int(line.split(":")[0].strip(), 16)
            nxt = int(lines[i + 1].split(":")[0].strip(), 16)
    assert store is not None and nxt is not None, "store instruction not found in produce()"
    return {"g": sym["g"], "batch_limit": sym["g"] + 64 * 4, "produce": sym["produce"],
            "store": store, "after_store": nxt}


class Mcp:
    """An MCP client speaking through the stdio bridge, like a real one."""

    def __init__(self, port):
        self.proc = subprocess.Popen(
            [sys.executable, os.path.join(ROOT, "tools", "laplace-mcp"), "--port", str(port)],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, bufsize=1)
        self.next_id = 0

    def send(self, method, params=None, notify=False):
        msg = {"jsonrpc": "2.0", "method": method}
        if params is not None:
            msg["params"] = params
        if not notify:
            self.next_id += 1
            msg["id"] = self.next_id
        self.proc.stdin.write(json.dumps(msg) + "\n")
        self.proc.stdin.flush()
        return None if notify else self.next_id

    def recv(self):
        line = self.proc.stdout.readline()
        if not line:
            raise RuntimeError("MCP bridge closed")
        return json.loads(line)

    def call(self, method, params=None):
        self.send(method, params)
        return self.recv()

    def tool(self, name, **args):
        r = self.call("tools/call", {"name": name, "arguments": args})
        text = r["result"]["content"][0]["text"]
        err = r["result"].get("isError", False)
        say(f"  mcp> {name}({', '.join(f'{k}={v}' for k, v in args.items())})")
        say(f"       {text}")
        return text, err

    def close(self):
        self.proc.stdin.close()
        self.proc.wait(timeout=10)


def field(text, key):
    m = re.search(r"\b%s=(0x[0-9a-f]+|\d+)" % key, text)
    return int(m.group(1), 0) if m else None


def main():
    for tool in (QEMU, GDB, "nm", "objdump"):
        if not shutil.which(tool):
            say(f"FAILED: {tool} not found")
            return 1
    facts = elf_facts()
    work = tempfile.mkdtemp(prefix="laplace-e2e-")
    console = os.path.join(work, "console.log")
    disk = os.path.join(work, "disk.img")
    with open(disk, "wb") as f:
        f.truncate(64 * 1024 * 1024)
    gdb_port, mcp_port = free_port(), free_port()

    say("=== Laplace: a heisenbug recorded in the booted OS, debugged backward ===")
    say(f"[boot] QEMU: IDE disk, console on COM1, gdb on :{gdb_port} (COM2), MCP on :{mcp_port} (COM3)")
    qemu = subprocess.Popen(
        [QEMU, "-m", "256M", "-display", "none", "-no-reboot", "-kernel", KERNEL,
         "-drive", f"file={disk},format=raw,if=ide,index=0",
         "-serial", f"file:{console}",
         "-serial", f"tcp:127.0.0.1:{gdb_port},server,nowait",
         "-serial", f"tcp:127.0.0.1:{mcp_port},server,nowait"],
        stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)
    try:
        deadline = time.monotonic() + 90
        text = ""
        while time.monotonic() < deadline:
            time.sleep(0.25)
            if os.path.exists(console):
                text = open(console, errors="replace").read()
                if "monitor: stopped" in text:
                    break
        stop = re.search(r"monitor: stopped \((.*)\) at epoch (\d+) step (\d+)", text)
        check(stop is not None, "the machine stopped for the debugger")
        say(f"[run ] {stop.group(1)} at epoch {stop.group(2)}, step {stop.group(3)}")
        check("asserted (code bad)" in stop.group(1), "the heisenbug's assertion fired (code 0xbad)")

        say("\n[mcp ] an agent attaches through tools/laplace-mcp")
        mcp = Mcp(mcp_port)
        init = mcp.call("initialize", {"protocolVersion": "2025-06-18", "capabilities": {},
                                       "clientInfo": {"name": "e2e", "version": "1"}})
        mcp.send("notifications/initialized", notify=True)
        check(init["result"]["serverInfo"]["name"] == "laplace", "initialize handshake")
        tools = [t["name"] for t in mcp.call("tools/list")["result"]["tools"]]
        check(len(tools) == 13 and all("inputSchema" in t for t in mcp.call("tools/list")["result"]["tools"]),
              f"tools/list: {len(tools)} tools with input schemas")

        where, _ = mcp.tool("where")
        check("end of the recording" in where, "positioned at the end of the recording")
        procs, _ = mcp.tool("list_processes")
        pid = int(re.search(r"pid=(\d+) name=heisenbug", procs).group(1))
        addr = hex(facts["batch_limit"])
        now, _ = mcp.tool("read_memory", pid=pid, addr=addr, len=4)
        clobbered = field(now, "u32")
        check(clobbered is not None and clobbered != 48, f"batch_limit is {clobbered}, not 48: clobbered")

        say("\n[mcp ] who wrote batch_limit last?")
        hit, err = mcp.tool("watch_last_write", pid=pid, addr=addr, len=4)
        check(not err and "last write at" in hit, "watch_last_write found the write")
        check(field(hit, "rip") == facts["after_store"],
              f"landed right after the store in produce() ({facts['after_store']:#x})")
        regs, _ = mcp.tool("get_registers", pid=pid)
        check(field(regs, "rax") == 64, "rax (the slot index) is 64: one past a 64-slot ring")
        at, _ = mcp.tool("read_memory", pid=pid, addr=addr, len=4)
        check(field(at, "u32") == clobbered, "the value there is the clobbered one")
        back, _ = mcp.tool("reverse_stepi")
        check(field(back, "rip") == facts["store"], f"one instruction back is the store ({facts['store']:#x})")
        before, _ = mcp.tool("read_memory", pid=pid, addr=addr, len=4)
        check(field(before, "u32") == 48, "before the store, batch_limit was still 48")
        fwd, _ = mcp.tool("stepi")
        check(field(fwd, "rip") == facts["after_store"], "stepi forward re-executes the store")
        ver, err = mcp.tool("verify_replay")
        checked = field(ver, "epochs_checked") or 0
        check(not err and "byte-exact" in ver and checked >= 1,
              f"every retained epoch ({checked}) re-executes byte-exact")

        say("\n[gdb ] stock gdb attaches to the same machine")
        script = os.path.join(work, "session.gdb")
        with open(script, "w") as f:
            f.write(f"""set pagination off
set confirm off
set remotetimeout 120
file {ELF}
target remote 127.0.0.1:{gdb_port}
print g.batch_limit
watch -l g.batch_limit
reverse-continue
print g.batch_limit
info registers rax
delete
break produce
reverse-continue
reverse-stepi
x/i $pc
monitor verify
detach
""")
        out = subprocess.run([GDB, "-batch", "-nx", "-x", script], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                             text=True, timeout=300).stdout
        for line in out.splitlines():
            if line.strip() and not line.startswith("warning"):
                say(f"  gdb> {line}")
        check(f"$1 = {clobbered}" in out, "gdb reads the clobbered value")
        check("Hardware watchpoint 1" in out and "New value = 48" in out,
              "watch + reverse-continue: stopped where batch_limit was about to change from 48")
        check(re.search(r"in produce \(payload=payload@entry=%d, burst=burst@entry=1\)" % clobbered, out) is not None,
              "  ...inside produce(), during a burst, with the clobbering payload")
        check("$2 = 48" in out and re.search(r"rax\s+0x40\s+64", out) is not None,
              "  ...with batch_limit still 48 and slot index rax = 64")
        check("Breakpoint 2, produce" in out, "break produce + reverse-continue stops in produce()")
        check("byte-exact" in out, "monitor verify: byte-exact")

        say("\n[mcp ] resume past the bug: the machine keeps running and recording")
        before_window, _ = mcp.tool("list_checkpoints")
        newest_before = field(before_window, "newest")
        rid = mcp.send("tools/call", {"name": "resume", "arguments": {}})
        time.sleep(1.5)
        pid2 = mcp.send("ping")   # any request stops a live machine at its next entry
        resumed = mcp.recv()
        pong = mcp.recv()
        text = resumed["result"]["content"][0]["text"]
        say(f"  mcp> resume()\n       {text}")
        check(resumed["id"] == rid and "ran live and stopped" in text and "interrupt" in text,
              "resumed live, then stopped by the next request")
        check(pong["id"] == pid2, "the interrupting request was answered")
        after_window, _ = mcp.tool("list_checkpoints")
        check((field(after_window, "newest") or 0) > (newest_before or 0), "the recording grew past the bug")
        ver2, err = mcp.tool("verify_replay")
        check(not err and "byte-exact" in ver2, "the extended recording re-executes byte-exact")
        mcp.close()
    finally:
        qemu.kill()
        qemu.wait()
        if failures and os.environ.get("LAPLACE_KEEP"):
            shutil.copy(console, os.environ["LAPLACE_KEEP"])
        shutil.rmtree(work, ignore_errors=True)
    return finish()


if __name__ == "__main__":
    sys.exit(main())
