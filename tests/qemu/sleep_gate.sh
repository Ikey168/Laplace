#!/usr/bin/env bash
# In-QEMU gate for blocking, sleeping, and idling (#246).
#
#  1. Two processes, one napping 8 x 25 ms with nanosleep while the other
#     works: they record and replay byte-exact; each wake is an input
#     delivered at the step it was journaled at.
#  2. One process sleeps 10 s with nothing else to run: the machine idles
#     with hlt. At a 10 ms keyframe interval, only a handful of keyframes are
#     taken (idle time adds no steps); recording continues correctly after the
#     wake, and the replay does not wait the 10 s again.
#  3. While everything is blocked, a debugger (MCP) stops the machine, lists
#     the waiting process, and verifies the recording so far.
set -euo pipefail
cd "$(dirname "$0")/../.."

QEMU=${QEMU:-qemu-system-x86_64}
fail=0

check() {  # check <output> <pattern> <description>
    if echo "$1" | grep -qE "$2"; then echo "[ok] $3"; else echo "[FAIL] $3"; fail=1; fi
}

boot() {   # boot <append>
    timeout 300 "$QEMU" -m 256M -display none -no-reboot \
        -device isa-debug-exit,iobase=0xf4,iosize=0x04 \
        -kernel build/laplace.elf -append "$1 selftest exit" \
        -serial stdio -serial null -serial null 2>&1 | tr -d '\r' || true
}

out=$(boot 'run="musl_sleeper sleep 8 25","musl_sleeper work 60" interval=10 keyframes=64')
echo "$out" | grep -E "sleeper:|selftest: (window|inputs|verify)" | sed "s/^/  [two] /"
check "$out" "sleeper: 8 naps of 25 ms, none early" "a process sleeps and wakes on time"
check "$out" "sleeper: worker done" "the other process runs meanwhile"
check "$out" "selftest: verify rc=0: [0-9]+ epochs replayed, 0 diverged" "both replay byte-exact, wakes at the same steps"

start=$(date +%s)
out=$(boot 'run="musl_sleeper idle 10000" interval=10 keyframes=64')
took=$(( $(date +%s) - start ))
echo "$out" | grep -E "sleeper:|selftest: (window|verify)" | sed "s/^/  [idle] /"
check "$out" "sleeper: woke after 100[0-9][0-9] ms; working" "the machine idles 10 s and wakes the sleeper"
check "$out" "sleeper: done" "recording continues after the idle"
taken=$(echo "$out" | sed -nE 's/.*; ([0-9]+) keyframes taken.*/\1/p')
if [ -n "$taken" ] && [ "$taken" -le 20 ]; then
    echo "[ok] $taken keyframes in 10 s at a 10 ms interval: idle time adds no steps"
else
    echo "[FAIL] keyframes taken: '${taken}'"; fail=1
fi
check "$out" "selftest: verify rc=0: [0-9]+ epochs replayed, 0 diverged" "the idle recording replays byte-exact"
vms=$(echo "$out" | sed -nE 's/selftest: verify took ([0-9]+) ms/\1/p')
if [ -n "$vms" ] && [ "$vms" -lt 8000 ]; then
    echo "[ok] verifying took ${vms} ms (the whole run ${took}s): the replay did not wait out the idle"
else
    echo "[FAIL] verifying took '${vms}' ms"; fail=1
fi

python3 - <<'PY' || fail=1
import json, os, socket, subprocess, sys, tempfile, time
root = os.getcwd()
def free_port():
    s = socket.socket(); s.bind(("127.0.0.1", 0)); p = s.getsockname()[1]; s.close(); return p
mcp_port = free_port()
console = tempfile.mktemp()
q = subprocess.Popen(["qemu-system-x86_64", "-m", "256M", "-display", "none", "-no-reboot",
                      "-kernel", "build/laplace.elf", "-append", 'run="musl_sleeper idle 60000"',
                      "-serial", f"file:{console}", "-serial", "null",
                      "-serial", f"tcp:127.0.0.1:{mcp_port},server,nowait"])
ok = True
try:
    for _ in range(300):
        time.sleep(0.1)
        if os.path.exists(console) and "idle for" in open(console, errors="replace").read():
            break
    time.sleep(1.0)   # well into the idle
    bridge = subprocess.Popen([sys.executable, os.path.join(root, "tools", "laplace-mcp"),
                               "--port", str(mcp_port)],
                              stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, bufsize=1)
    def call(method, params=None, i=[0]):
        i[0] += 1
        msg = {"jsonrpc": "2.0", "id": i[0], "method": method}
        if params is not None: msg["params"] = params
        bridge.stdin.write(json.dumps(msg) + "\n"); bridge.stdin.flush()
        return json.loads(bridge.stdout.readline())
    def tool(name, **a):
        r = call("tools/call", {"name": name, "arguments": a})
        return r["result"]["content"][0]["text"]
    call("initialize", {"protocolVersion": "2024-11-05", "capabilities": {},
                        "clientInfo": {"name": "sleep_gate", "version": "1"}})
    procs = tool("list_processes")
    ver = tool("verify_replay")
    text = open(console, errors="replace").read()
    stopped = "monitor: stopped (interrupt: a debugger asked to stop)" in text
    print("  [mcp] " + text.strip().splitlines()[-1])
    print("  [mcp] list_processes: " + procs.replace("\n", " | ")[:160])
    print("  [mcp] verify_replay: " + ver.strip()[:160])
    for good, what in [(stopped, "a debugger stops the machine while everything is blocked"),
                       ("sleeper" in procs, "it lists the waiting process"),
                       (ver.startswith("byte-exact"), "the recording up to the idle verifies")]:
        print(("[ok] " if good else "[FAIL] ") + what)
        ok = ok and good
    bridge.kill()
finally:
    q.kill()
    q.wait()
sys.exit(0 if ok else 1)
PY

if [ $fail -ne 0 ]; then echo "FAILED"; exit 1; fi
echo "PASSED: processes sleep and wake deterministically, and the machine idles without adding steps"
