#!/usr/bin/env bash
# In-QEMU gate for preemption at any user instruction, without a counter (#245).
#
#  1. Two processes compute without a system call between "start" and
#     "done": both start before either finishes, so each was preempted inside
#     its loop, and the run replays byte-exact.
#  2. A loop whose registers are the same at every iteration (only memory
#     moves) is preempted and replays byte-exact.
#  3. Copies with `rep movsb` are preempted between iterations of the
#     instruction, and the replay finds those points again.
#  4. One process spins on a flag that a 5 ms interval timer's handler sets:
#     the signals arrive inside the spin (the same state, again and again),
#     and the run replays byte-exact.
#
# QEMU_ACCEL=kvm runs the same under KVM (-enable-kvm -cpu host); the default
# is TCG. Neither offers a branch counter here.
set -euo pipefail
cd "$(dirname "$0")/../.."

QEMU=${QEMU:-qemu-system-x86_64}
ACCEL=()
# KVM runs the loops far faster than TCG: more work, so they still span
# several 1 ms slices.
K=1 KR=1
if [ "${QEMU_ACCEL:-tcg}" = kvm ]; then ACCEL=(-enable-kvm -cpu host); K=8 KR=20; fi
fail=0

check() {  # check <output> <pattern> <description>
    if echo "$1" | grep -qE "$2"; then echo "[ok] $3"; else echo "[FAIL] $3"; fail=1; fi
}

boot() {   # boot <append>
    timeout 900 "$QEMU" "${ACCEL[@]}" -m 256M -display none -no-reboot \
        -device isa-debug-exit,iobase=0xf4,iosize=0x04 \
        -kernel build/laplace.elf -append "$1 selftest exit" \
        -serial stdio -serial null -serial null 2>&1 | tr -d '\r' || true
}

show() {   # show <tag> <output>
    echo "$2" | grep -E "spinner|selftest: (preemption|verify|first)" | sed "s/^/  [$1] /"
}

recorded() { echo "$1" | sed -nE 's/.*preemption points: ([0-9]+) recorded.*/\1/p'; }

out=$(boot "run=\"musl_spinner compute A $((1000000 * K))\",\"musl_spinner compute B $((1000000 * K))\" interval=10 keyframes=64")
show compute "$out"
first_done=$(echo "$out" | grep -nE "spinner [AB]: done" | head -1 | cut -d: -f1)
last_start=$(echo "$out" | grep -nE "spinner [AB]: start" | tail -1 | cut -d: -f1)
if [ -n "$first_done" ] && [ -n "$last_start" ] && [ "$last_start" -lt "$first_done" ]; then
    echo "[ok] both loops started before either finished: preempted inside a loop with no system call"
else
    echo "[FAIL] the loops ran one after the other"; fail=1
fi
n=$(recorded "$out")
if [ -n "$n" ] && [ "$n" -gt 0 ]; then echo "[ok] $n preemption points recorded in user code"; else echo "[FAIL] no preemption points"; fail=1; fi
check "$out" "selftest: verify rc=0: [0-9]+ epochs replayed, 0 diverged" "the interleaving replays byte-exact"
check "$out" "input record with other arguments: divergence detected" "a changed record is caught"

out=$(boot 'run="musl_spinner memloop A 3000000","musl_spinner compute B 600000" interval=10 keyframes=64')
show memloop "$out"
check "$out" "spinner A: done 2dc6c0" "the memory-only loop counts to 3000000"
n=$(recorded "$out")
if [ -n "$n" ] && [ "$n" -gt 0 ]; then echo "[ok] $n preemption points recorded"; else echo "[FAIL] no preemption points"; fail=1; fi
check "$out" "selftest: verify rc=0: [0-9]+ epochs replayed, 0 diverged" "a loop where only memory changes replays byte-exact"

out=$(boot "run=\"musl_spinner rep A $((200 * KR))\",\"musl_spinner compute B $((300000 * K))\" interval=10 keyframes=64")
show rep "$out"
reps=$(echo "$out" | sed -nE 's/.*\(([0-9]+) inside string instructions, [0-9]+ pages.*/\1/p')
found=$(echo "$out" | sed -nE 's/.*found [0-9]+ by state \(([0-9]+) inside string.*/\1/p')
if [ -n "$reps" ] && [ "$reps" -gt 0 ] && [ -n "$found" ] && [ "$found" -gt 0 ]; then
    echo "[ok] $reps points inside rep movsb recorded; replays found $found of them again"
else
    echo "[FAIL] string-instruction points: recorded '${reps}', found '${found}'"; fail=1
fi
check "$out" "selftest: verify rc=0: [0-9]+ epochs replayed, 0 diverged" "the copies replay byte-exact"

out=$(boot 'run="musl_spinner alarm 20" interval=10 keyframes=64')
show alarm "$out"
check "$out" "spinner: 20 alarms arrived while spinning" "a spinning process gets its timer's signals"
check "$out" "selftest: verify rc=0: [0-9]+ epochs replayed, 0 diverged" "the spin replays byte-exact"

if [ $fail -ne 0 ]; then echo "FAILED"; exit 1; fi
echo "PASSED: processes are preempted at any instruction, and replays find each point again by its state"
