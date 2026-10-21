#!/usr/bin/env bash
# In-QEMU gate for processes that start processes (#248).
#
#  1. `musl_forkexec parent 50` forks 50 children; each writes a shared array
#     (copy-on-write: the parent's copy must stay intact), then execs
#     `musl_forkexec child I P`, which checks its parent and exits with I. The
#     parent reaps all 50 with wait and checks the statuses. The recording
#     replays byte-exact.
#  2. `musl_forkexec kill`: SIGTERM ends a sleeping child, and wait reports
#     the signal.
#  3. `musl_forkexec parent 2 pause`: children sleep 300 ms between fork and
#     exec (then run musl_hello). After the recording stops, MCP rewinds
#     through the keyframes to a point between a fork and its exec, and both
#     MCP (list_processes) and gdb (qfThreadInfo) show the two processes.
set -euo pipefail
cd "$(dirname "$0")/../.."

QEMU=${QEMU:-qemu-system-x86_64}
fail=0

check() {  # check <output> <pattern> <description>
    if echo "$1" | grep -qE "$2"; then echo "[ok] $3"; else echo "[FAIL] $3"; fail=1; fi
}

boot() {   # boot <append>
    timeout 600 "$QEMU" -m 512M -display none -no-reboot \
        -device isa-debug-exit,iobase=0xf4,iosize=0x04 \
        -kernel build/laplace.elf -append "$1 selftest exit" \
        -serial stdio -serial null -serial null 2>&1 | tr -d '\r' || true
}

out=$(boot 'run="musl_forkexec parent 50" interval=5 keyframes=64')
echo "$out" | grep -E "forkexec:|selftest: (window|verify)" | sed "s/^/  [50] /"
check "$out" "forkexec: 50 children forked, exec'd, and reaped; exit codes sum 1225; 0 bad; parent memory intact; then wait says no children" \
      "50 children fork, exec, exit, and are reaped; the parent's memory is its own"
check "$out" "selftest: verify rc=0: [0-9]+ epochs replayed, 0 diverged" "the recording replays byte-exact"

out=$(boot 'run="musl_forkexec kill" interval=5 keyframes=64')
echo "$out" | grep -E "forkexec:|selftest: verify" | sed "s/^/  [kill] /"
check "$out" "forkexec: killed pid 2: signal 15" "kill ends a child and wait reports SIGTERM"
check "$out" "selftest: verify rc=0: [0-9]+ epochs replayed, 0 diverged" "... and replays byte-exact"

python3 - <<'PY' || fail=1
import json, os, re, socket, subprocess, sys, tempfile, time
def free_port():
    s = socket.socket(); s.bind(("127.0.0.1", 0)); p = s.getsockname()[1]; s.close(); return p
gdb_port, mcp_port = free_port(), free_port()
console = tempfile.mktemp()
q = subprocess.Popen(["qemu-system-x86_64", "-m", "256M", "-display", "none", "-no-reboot",
                      "-kernel", "build/laplace.elf",
                      "-append", 'run="musl_forkexec parent 2 pause" interval=10 keyframes=64',
                      "-serial", f"file:{console}",
                      "-serial", f"tcp:127.0.0.1:{gdb_port},server,nowait",
                      "-serial", f"tcp:127.0.0.1:{mcp_port},server,nowait"])
ok = True
try:
    for _ in range(600):
        time.sleep(0.1)
        if os.path.exists(console) and "monitor: stopped" in open(console, errors="replace").read():
            break
    bridge = subprocess.Popen([sys.executable, "tools/laplace-mcp", "--port", str(mcp_port)],
                              stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, bufsize=1)
    n = [0]
    def call(method, params=None):
        n[0] += 1
        m = {"jsonrpc": "2.0", "id": n[0], "method": method}
        if params is not None: m["params"] = params
        bridge.stdin.write(json.dumps(m) + "\n"); bridge.stdin.flush()
        return json.loads(bridge.stdout.readline())
    def tool(name, **a):
        return call("tools/call", {"name": name, "arguments": a})["result"]["content"][0]["text"]
    call("initialize", {"protocolVersion": "2024-11-05", "capabilities": {},
                        "clientInfo": {"name": "proc_gate", "version": "1"}})
    cps = tool("list_checkpoints")
    lo = int(re.search(r"oldest=(\d+)", cps).group(1))
    hi = int(re.search(r"newest=(\d+)", cps).group(1))
    found = None
    for e in range(lo, hi + 1):
        for off in range(0, 200):
            r = tool("rewind_to", epoch=e, offset=off)
            if r.startswith("rewind failed"):
                break
            procs = tool("list_processes")
            if procs.count("musl_forkexec") >= 2:
                found = (e, off, procs)
                break
        if found:
            break
    print("  [rewind] checkpoints: " + cps.strip().splitlines()[0][:100])
    if not found:
        print("[FAIL] no keyframe between a fork and its exec")
        ok = False
    else:
        e, off, procs = found
        print(f"  [rewind] epoch {e} offset {off}: " + procs.replace("\n", " | ")[:160])
        print("[ok] MCP: rewound between fork and exec, both processes listed")
        s = socket.create_connection(("127.0.0.1", gdb_port), timeout=10)
        def pkt(body):
            cs = sum(body.encode()) & 0xFF
            s.sendall(f"${body}#{cs:02x}".encode())
            data = b""
            while not re.search(rb"\$[^#]*#..", data):
                data += s.recv(4096)
            s.sendall(b"+")
            return re.search(rb"\$([^#]*)#", data).group(1).decode()
        first = pkt("qfThreadInfo")
        rest = pkt("qsThreadInfo")
        tids = [t for t in first.lstrip("m").split(",") if t]
        print(f"  [rewind] gdb qfThreadInfo: {first}, then {rest}")
        good = len(tids) >= 2
        print(("[ok] " if good else "[FAIL] ") + "gdb lists both processes as threads")
        ok = ok and good
        s.close()
    bridge.kill()
finally:
    q.kill(); q.wait()
sys.exit(0 if ok else 1)
PY

if [ $fail -ne 0 ]; then echo "FAILED"; exit 1; fi
echo "PASSED: programs fork, exec, wait, and kill, and the debuggers see every process at any point"
