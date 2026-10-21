#!/usr/bin/env bash
# In-QEMU gate for the generic input journal (#242).
#
#  1. musl_inputprobe (user/linux/inputprobe.c) reads 10 MiB from /dev/urandom
#     (journaled entropy, 64 KiB per call) and the clocks, all within one
#     epoch, and keeps a hash of every byte. The input records hold 10 MiB of
#     payload in the page pool; the replay hands back exactly those bytes.
#  2. A replay whose first input call names other arguments than the recorded
#     one stops at that step, and verify reports the "inputs" component.
#  3. Clock and entropy now go through the same record (determinism and
#     Linux gates keep checking their replays).
set -euo pipefail
cd "$(dirname "$0")/../.."

QEMU=${QEMU:-qemu-system-x86_64}
fail=0

check() {  # check <output> <pattern> <description>
    if echo "$1" | grep -qE "$2"; then echo "[ok] $3"; else echo "[FAIL] $3"; fail=1; fi
}

out=$(timeout 600 "$QEMU" -m 512M -display none -no-reboot \
    -device isa-debug-exit,iobase=0xf4,iosize=0x04 \
    -kernel build/laplace.elf -append "run=musl_inputprobe interval=1000000 selftest exit" \
    -serial stdio -serial null -serial null 2>&1 | tr -d '\r' || true)
echo "$out" | grep -E "inputprobe:|selftest: (window|inputs|verify|input record)" | sed "s/^/  /"
check "$out" "inputprobe: read 10485760 bytes of journaled input; hash [0-9a-f]{16}" "a program reads 10 MiB of input"
check "$out" "selftest: window epochs 1\.\.1 \(1 keyframes\)" "... within one epoch"
check "$out" "selftest: inputs: [0-9]+ records, 1048[5-9][0-9]{3} payload bytes journaled" \
      "the journal holds its 10 MiB of payload"
check "$out" "selftest: verify rc=0: 1 epochs replayed, 0 diverged" "the replay gets the same bytes"
check "$out" "input record with other arguments: divergence detected \(epoch 1 component 15: inputs, step [0-9]+\)" \
      "a replay that makes another input call is stopped and named at that step"

if [ $fail -ne 0 ]; then echo "FAILED"; exit 1; fi
echo "PASSED: inputs of any size are journaled and replayed, and a replay that strays is caught at the call"
