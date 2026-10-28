#!/usr/bin/env bash
# In-QEMU gate for threads and futexes (#251), unmodified musl pthreads.
#
#  1. Six threads: four add to a counter under a mutex, a producer and a
#     consumer pass 200 items through a queue on a condition variable; all
#     are joined. The run records and replays byte-exact.
#  2. Two threads add to a shared counter without atomics. Preempted between
#     a load and a store (#245), they lose updates; the run replays
#     byte-exact, the same lost updates included; replays with a clock read
#     one tick late, or one random bit flipped, are caught (#256).
#  3. The same race, debugged over MCP. At a point where one thread is about
#     to store a value it loaded before it was preempted, watch_last_write on
#     the counter lands right after the other thread's store: the update the
#     first one is about to overwrite. list_processes names the threads, and
#     get_registers gives fs_base and the vector registers.
#  4. The same race in stock gdb, which reads the program from the target
#     (no file argument), shows xmm0 and $fs_base, and reverse-continues to a
#     watchpoint on the counter, written by a racing thread.
set -euo pipefail
cd "$(dirname "$0")/../.."

QEMU=${QEMU:-qemu-system-x86_64}
fail=0

check() {  # check <output> <pattern> <description>
    if echo "$1" | grep -qE "$2"; then echo "[ok] $3"; else echo "[FAIL] $3"; fail=1; fi
}

boot() {   # boot <append>
    timeout 900 "$QEMU" -m 256M -display none -no-reboot \
        -device isa-debug-exit,iobase=0xf4,iosize=0x04 \
        -kernel build/laplace.elf -append "$1 selftest exit" \
        -serial stdio -serial null -serial null 2>&1 | tr -d '\r' || true
}

out=$(boot 'run="musl_threads sync 20000" interval=10 keyframes=64')
echo "$out" | grep -E "threads:|selftest: (preemption|verify)" | sed "s/^/  [sync] /"
check "$out" "threads: 6 threads joined; counter 80000 \(want 80000\); queue passed items summing to 20100" \
      "mutexes, a condition variable, and join work"
check "$out" "selftest: verify rc=0: [0-9]+ epochs replayed, 0 diverged" "the threaded run replays byte-exact"

out=$(boot 'run="musl_threads race 300000" interval=10 keyframes=64')
echo "$out" | grep -E "race:|selftest: (preemption|verify)" | sed "s/^/  [race] /"
lost=$(echo "$out" | sed -nE 's/.*\(([0-9]+) updates lost\).*/\1/p')
if [ -n "$lost" ] && [ "$lost" -gt 0 ]; then
    echo "[ok] $lost updates lost to the race while recording"
else
    echo "[FAIL] no lost updates ('${lost}')"; fail=1
fi
check "$out" "selftest: verify rc=0: [0-9]+ epochs replayed, 0 diverged" "the race replays byte-exact"
for kind in clock entropy; do
    bad=$(boot "run=\"musl_threads race 300000\" interval=10 keyframes=64 sabotage=$kind")
    echo "$bad" | grep -E "selftest: (verify rc|first divergence)" | sed "s/^/  [sabotage=$kind] /"
    if echo "$bad" | grep -qE "selftest: verify rc=[-0-9]+: [0-9]+ epochs replayed, [1-9][0-9]* diverged"; then
        echo "[ok] a replay with sabotage=$kind is caught (#256)"
    else
        echo "[FAIL] a replay with sabotage=$kind went unnoticed"; fail=1
    fi
done

# 3. The race over MCP.
elf=build/user/musl_threads.elf
counter=$(nm "$elf" | awk '$3 == "g_race" { print $1 }')
dis=$(objdump -d "$elf" | awk '/<racer>:/,/ret/')
load_end=$(echo "$dis" | grep -A1 "<g_race>" | sed -n 2p | sed -E 's/^ *([0-9a-f]+):.*/\1/')
store=$(echo "$dis" | grep "<g_race>" | sed -n 2p | sed -E 's/^ *([0-9a-f]+):.*/\1/')
after_store=$(echo "$dis" | grep -A1 "<g_race>" | tail -1 | sed -E 's/^ *([0-9a-f]+):.*/\1/')
echo "  [mcp] counter at 0x$counter; racer loads it before 0x$load_end, stores it at 0x$store"

racer_lo=$(echo "$dis" | sed -n 2p | sed -E 's/^ *([0-9a-f]+):.*/\1/')
racer_hi=$(echo "$dis" | tail -1 | sed -E 's/^ *([0-9a-f]+):.*/\1/')
RACER_LO=$racer_lo RACER_HI=$racer_hi COUNTER=$counter LOAD_END=$load_end STORE=$store AFTER_STORE=$after_store python3 - <<'PY' || fail=1
import json, os, re, socket, subprocess, sys, tempfile, time
root = os.getcwd()
counter = int(os.environ["COUNTER"], 16)
load_end, store = int(os.environ["LOAD_END"], 16), int(os.environ["STORE"], 16)
after_store = int(os.environ["AFTER_STORE"], 16)
racer_lo, racer_hi = int(os.environ["RACER_LO"], 16), int(os.environ["RACER_HI"], 16)

def free_port():
    s = socket.socket(); s.bind(("127.0.0.1", 0)); p = s.getsockname()[1]; s.close(); return p
def field(text, key):
    m = re.search(r"\b%s=(0x[0-9a-f]+|\d+)" % key, text)
    return int(m.group(1), 0) if m else None

port = free_port()
console = tempfile.mktemp()
q = subprocess.Popen(["qemu-system-x86_64", "-m", "256M", "-display", "none", "-no-reboot",
                      "-kernel", "build/laplace.elf", "-append",
                      'run="musl_threads race 300000" interval=10 keyframes=64',
                      "-serial", f"file:{console}", "-serial", "null",
                      "-serial", f"tcp:127.0.0.1:{port},server,nowait"])
ok = True
def report(good, what):
    global ok
    print(("[ok] " if good else "[FAIL] ") + what)
    ok = ok and good
try:
    for _ in range(3000):
        time.sleep(0.1)
        if os.path.exists(console) and "monitor: stopped" in open(console, errors="replace").read():
            break
    text = open(console, errors="replace").read()
    for line in text.splitlines():
        if line.startswith("race:"): print("  [mcp] " + line.strip())
    bridge = subprocess.Popen([sys.executable, os.path.join(root, "tools", "laplace-mcp"), "--port", str(port)],
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
                        "clientInfo": {"name": "threads_gate", "version": "1"}})
    cps = tool("list_checkpoints")
    oldest, newest = field(cps, "oldest"), field(cps, "newest")
    print(f"  [mcp] list_checkpoints: {cps.strip()[:120]}")
    # Walk the entries of the retained epochs: find a thread preempted
    # between its load of the counter and its store, and the entry after
    # which it runs again.
    found = None
    epoch = oldest
    while epoch is not None and epoch <= newest and not found:
        w = tool("rewind_to", epoch=epoch, offset=0)
        seen = []
        for _ in range(400):
            w = tool("where")
            if field(w, "epoch") != epoch: break
            v = field(tool("read_memory", pid=field(w, "pid") or 1, addr=counter, len=8), "u64")
            seen.append((field(w, "offset"), field(w, "pid"), field(w, "rip"), v))
            s = tool("step_forward")
            if "no further history" in s or "failed" in s: break
        for i, (off, pid, rip, val) in enumerate(seen):
            if pid is None or rip is None or not (load_end <= rip <= store): continue
            # Thread pid holds a stale value. The other racer must store to
            # the counter before pid's next entry; the segment before that
            # entry is pid's again.
            other = None
            for j in range(i + 1, len(seen)):
                off2, pid2, rip2, val2 = seen[j]
                if pid2 == pid:
                    prev = seen[j - 1]
                    if other is not None and prev[3] is not None and prev[3] != val:
                        found = (epoch, prev[0], pid, other)
                    break
                if pid2 is not None and rip2 is not None and racer_lo <= rip2 <= racer_hi:
                    other = pid2
            if found: break
        epoch += 1
    report(found is not None, "a thread was preempted between its load and its store, and the other ran")
    if found:
        epoch, off, victim, other = found
        at = tool("rewind_to", epoch=epoch, offset=off, insn=1)
        print(f"  [mcp] rewind_to(epoch={epoch}, offset={off}, insn=1): {at.strip()[:140]}")
        report(field(at, "pid") == victim and load_end <= (field(at, "rip") or 0) <= store,
               f"thread {victim} resumes between its load and its store")
        hit = tool("watch_last_write", pid=victim, addr=counter, len=8)
        print(f"  [mcp] watch_last_write(pid={victim}, counter): {hit.strip()[:140]}")
        report(field(hit, "pid") == other and field(hit, "rip") == after_store,
               f"the last write to the counter is thread {other}'s store, which thread {victim} is about to overwrite")
        procs = tool("list_processes")
        print("  [mcp] list_processes: " + procs.replace("\n", " | ")[:200])
        report(f"pid={other} thread_of=1" in procs, "list_processes names the threads and their process (#252)")
        regs = tool("get_registers", pid=other)
        print("  [mcp] get_registers: " + regs[:120] + " ...")
        report("fs_base=0x" in regs and "xmm0=0x" in regs and "xmm15=0x" in regs,
               "get_registers gives fs_base and the vector registers (#252)")
    bridge.kill()
finally:
    q.kill()
    q.wait()
sys.exit(0 if ok else 1)
PY

# 4. The same race in stock gdb, with no file argument (#252): gdb reads the
#    program from the target, lists the threads, shows a vector register, and
#    a reverse-continue to a watchpoint on the counter stops in a racing
#    thread at its store.
port=$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1])')
console=$(mktemp)
"$QEMU" -m 256M -display none -no-reboot -kernel build/laplace.elf \
    -append 'run="musl_threads race 300000" interval=10 keyframes=64' -serial "file:$console" \
    -serial "tcp:127.0.0.1:$port,server,nowait" -serial null &
qpid=$!
for _ in $(seq 1200); do grep -q "monitor: stopped" "$console" 2>/dev/null && break; sleep 0.5; done
gdir=$(mktemp -d)
gdbout=$(cd "$gdir" && timeout 600 gdb -batch -nx -ex "target remote 127.0.0.1:$port" \
    -ex 'info registers xmm0' -ex 'p/x $fs_base' -ex 'watch -l g_race' -ex 'reverse-continue' \
    -ex 'info threads' -ex 'thread 1' -ex 'bt 1' 2>&1 || true)
kill $qpid 2>/dev/null || true
wait $qpid 2>/dev/null || true
rm -rf "$console" "$gdir"
echo "$gdbout" | grep -E "Reading symbols|xmm0|hit Hardware|Old value|New value|racer|Thread|^\\$" | cut -c1-140 | sed "s/^/  [gdb] /"
check "$gdbout" "Reading symbols from target:/musl_threads" "gdb loads the program's symbols from the target, with no file argument"
check "$gdbout" "xmm0 +\{v4_float" "info registers xmm0 shows the vector register"
check "$gdbout" '\$1 = 0x[0-9a-f]+' '$fs_base reads'
check "$gdbout" "Thread [23] hit Hardware watchpoint 1: -location g_race" \
      "reverse-continue stops on the counter's watchpoint in a racing thread"
check "$gdbout" "in racer \(arg=0x[01]\) at user/linux/threads.c" "... at the racer's store, with source"
check "$gdbout" "Thread 1 \(musl_threads \(pid 1\)\)" "info threads lists the main thread too"

if [ $fail -ne 0 ]; then echo "FAILED"; exit 1; fi
echo "PASSED: pthreads programs record and replay, and a lost update is traced to the other thread's store"
