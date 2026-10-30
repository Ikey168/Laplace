#!/usr/bin/env bash
# In-QEMU gate for memory at scale (#239, #244).
#
#  1. `musl_bigmem churn 64` mallocs and frees 64 MiB in sizes from 16 bytes
#     to 1 MiB; the recording replays with no divergence.
#  2. `musl_bigmem resident 256 300` keeps 256 MiB resident and rewrites a
#     64-page working set per round. With an 8-keyframe ring, the full base
#     keyframe written while the memory filled is evicted long before the end;
#     the keyframes that remain still restore and replay with no divergence,
#     and each writes only the pages its epoch dirtied.
set -euo pipefail
cd "$(dirname "$0")/../.."

QEMU=${QEMU:-qemu-system-x86_64}
fail=0

check() {  # check <output> <pattern> <description>
    if echo "$1" | grep -qE "$2"; then echo "[ok] $3"; else echo "[FAIL] $3"; fail=1; fi
}

boot() {   # boot <memory> <append>
    timeout 900 "$QEMU" -m "$1" -display none -no-reboot \
        -device isa-debug-exit,iobase=0xf4,iosize=0x04 \
        -kernel build/laplace.elf -append "$2 selftest exit" \
        -serial stdio -serial null -serial null 2>&1 | tr -d '\r' || true
}

out=$(boot 512M 'run="musl_bigmem churn 64" interval=20')
echo "$out" | grep -E "bigmem:|selftest: (keyframe pages|verify)" | sed "s/^/  [churn] /"
check "$out" "bigmem: churned 6[4-9] MiB in [0-9]+ operations, peak 1[0-9] MiB live: sum 58808aed" \
      "64 MiB of malloc and free in varied sizes runs"
check "$out" "selftest: verify rc=0: [0-9]+ epochs replayed, 0 diverged" "... and replays byte-exact"

out=$(boot 1G 'run="musl_bigmem resident 256 300" interval=5 keyframes=8 pool=320')
echo "$out" | grep -E "bigmem:|selftest: (window|keyframe pages|verify|restored)" | sed "s/^/  [resident] /"
check "$out" "bigmem: 300 rounds over a 64-page working set: sum 18e99c01" "256 MiB stays resident through 300 rounds"
taken=$(echo "$out" | sed -nE 's/.*\(8 keyframes\), end step [0-9]+; ([0-9]+) keyframes taken.*/\1/p')
written=$(echo "$out" | sed -nE 's/selftest: keyframe pages: ([0-9]+) written.*/\1/p')
newest=$(echo "$out" | sed -nE 's/.*the newest keyframe has ([0-9]+) pages and wrote ([0-9]+);.*/\1 \2/p')
if [ -n "$taken" ] && [ "$taken" -ge 40 ]; then
    echo "[ok] $taken keyframes taken, 8 retained: the base keyframe was evicted"
else
    echo "[FAIL] expected at least 40 keyframes, got '${taken}'"; fail=1
fi
if [ -n "$written" ] && [ -n "$taken" ]; then
    per=$(( (written - 65536) / taken ))
    if [ "$per" -le 256 ]; then
        echo "[ok] after the fill, keyframes wrote about $per pages each (of 65536 resident)"
    else
        echo "[FAIL] keyframes wrote $per pages each after the fill"; fail=1
    fi
else
    echo "[FAIL] no keyframe statistics"; fail=1
fi
set -- $newest
if [ "${1:-0}" -ge 65536 ] && [ "${2:-99999}" -le 256 ]; then
    echo "[ok] the newest keyframe holds $1 pages and wrote $2"
else
    echo "[FAIL] the newest keyframe: '${newest}'"; fail=1
fi
check "$out" "selftest: verify rc=0: 8 epochs replayed, 0 diverged" \
      "every retained keyframe restores and replays byte-exact without its base"

if [ $fail -ne 0 ]; then echo "FAILED"; exit 1; fi
echo "PASSED: keyframes write only dirtied pages, and survive evicting the keyframe that wrote the rest"
