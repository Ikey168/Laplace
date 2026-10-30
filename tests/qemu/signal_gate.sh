#!/usr/bin/env bash
# In-QEMU gate for signals (#250), with user/linux/sigprobe.c.
#
#  1. alarm: a 10 ms ITIMER_REAL; the SIGALRM handler (with floating point of
#     its own) interrupts a loop 20 times, the loop's FPU state survives, and
#     the recording replays byte-exact (each expiry is a journaled event).
#  2. segv: a SIGSEGV handler gets si_addr 0x10 and recovers with siglongjmp.
#  3. mask: blocked signals stay pending until unblocked; sigsuspend is woken
#     by a child's kill; the handler runs on the alternate stack; SA_RESETHAND.
#  4. abort: SIGABRT with no handler stops the machine and the stop names it.
#  5. null: a null-pointer store with no handler stops the machine with
#     SIGSEGV, and gdb shows the faulting source line.
set -euo pipefail
cd "$(dirname "$0")/../.."

QEMU=${QEMU:-qemu-system-x86_64}
fail=0

check() {  # check <output> <pattern> <description>
    if echo "$1" | grep -qE "$2"; then echo "[ok] $3"; else echo "[FAIL] $3"; fail=1; fi
}

boot() {   # boot <mode>
    timeout 300 "$QEMU" -m 256M -display none -no-reboot \
        -device isa-debug-exit,iobase=0xf4,iosize=0x04 \
        -kernel build/laplace.elf -append "run=\"musl_sigprobe $1\" interval=5 keyframes=64 selftest exit" \
        -serial stdio -serial null -serial null 2>&1 | tr -d '\r' || true
}

replays() {  # replays <output> <what>
    check "$1" "selftest: verify rc=0: [0-9]+ epochs replayed, 0 diverged" "$2 replays byte-exact"
}

out=$(boot alarm)
echo "$out" | grep -E "sigprobe:" | sed "s/^/  [alarm] /"
check "$out" "sigprobe: 20 SIGALRMs interrupted the loop at 20 distinct points; loop FPU intact, handler FPU worked" \
      "SIGALRM interrupts a loop; FPU state is saved in the frame and restored"
replays "$out" "the timer-driven run"

out=$(boot segv)
echo "$out" | grep -E "sigprobe:" | sed "s/^/  [segv] /"
check "$out" "sigprobe: caught SIGSEGV at 0x10 \(code 1\) and recovered" "a SIGSEGV handler sees si_addr and recovers"
replays "$out" "the fault-and-recover run"

out=$(boot mask)
echo "$out" | grep -E "sigprobe:" | sed "s/^/  [mask] /"
check "$out" "blocked SIGUSR1 pending, delivered on unblock yes; sigsuspend returned -1 after SIGUSR2, handler on the alternate stack yes, reset to default yes" \
      "masks, pending, sigsuspend, sigaltstack, SA_RESETHAND"
replays "$out" "the masking run"

out=$(boot abort)
echo "$out" | grep -E "sigprobe:|monitor: stopped" | sed "s/^/  [abort] /"
check "$out" "monitor: stopped \(signal: pid 1 got SIGABRT with no handler\)" "SIGABRT with no handler stops the machine"
replays "$out" "the run up to abort()"

out=$(boot null)
echo "$out" | grep -E "sigprobe:|monitor: stopped" | sed "s/^/  [null] /"
check "$out" "monitor: stopped \(fault: pid 1 SIGSEGV \(vector 14 error [0-9a-f]+ address 0\)\)" \
      "a null-pointer store stops the machine with SIGSEGV"

line=$(grep -n "the faulting line" user/linux/sigprobe.c | cut -d: -f1)
port=$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1])')
console=$(mktemp)
"$QEMU" -m 256M -display none -no-reboot -kernel build/laplace.elf \
    -append 'run="musl_sigprobe null"' -serial "file:$console" \
    -serial "tcp:127.0.0.1:$port,server,nowait" -serial null &
qpid=$!
for _ in $(seq 100); do grep -q "monitor: stopped" "$console" 2>/dev/null && break; sleep 0.2; done
gdbout=$(timeout 60 gdb -batch -nx build/user/musl_sigprobe.elf \
    -ex "target remote 127.0.0.1:$port" -ex 'info program' -ex 'info line *$pc' -ex 'bt 1' 2>&1 || true)
kill $qpid 2>/dev/null || true
wait $qpid 2>/dev/null || true
rm -f "$console"
echo "$gdbout" | grep -E "SIGSEGV|signal|Line|#0" | sed "s/^/  [gdb] /"
check "$gdbout" "SIGSEGV" "gdb reports the stop as SIGSEGV"
check "$gdbout" "Line $line of \"user/linux/sigprobe.c\"" "gdb shows the faulting line ($line)"

if [ $fail -ne 0 ]; then echo "FAILED"; exit 1; fi
echo "PASSED: signals are delivered at deterministic points, replay exactly, and faults stop for the debugger"
