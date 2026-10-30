#!/usr/bin/env bash
# In-QEMU gate for unmodified Linux programs (#238).
#
#  1. musl_hello (user/linux/hello.c, musl-gcc -static, linked at 0x400000)
#     runs: startup (auxv, TLS through arch_prctl), uname, getrandom,
#     clock_gettime, stdio; its recording replays byte-exact.
#  2. musl_sysprobe: an unimplemented system call is reported and returns
#     ENOSYS; the clocks agree and advance; getrandom fills its buffer. Its
#     clock and random values are kept in memory, so the replay must match
#     them, and a replay with perturbed entropy must be caught.
set -euo pipefail
cd "$(dirname "$0")/../.."

QEMU=${QEMU:-qemu-system-x86_64}
fail=0

boot() {   # boot <run list>
    timeout 300 "$QEMU" -m 256M -display none -no-reboot \
        -device isa-debug-exit,iobase=0xf4,iosize=0x04 \
        -kernel build/laplace.elf -append "run=$1 selftest exit" \
        -serial stdio -serial null -serial null 2>&1 | tr -d '\r' || true
}

check() {  # check <output> <pattern> <description>
    if echo "$1" | grep -qE "$2"; then echo "[ok] $3"; else echo "[FAIL] $3"; fail=1; fi
}

out=$(boot musl_hello)
echo "$out" | grep -E "hello from|selftest: verify" | sed "s/^/  /"
check "$out" "hello from musl_hello on Linux x86_64 \(argc 1, pid 1, tls 42, getrandom 8, clock ok\)" \
      "a static musl program starts, uses TLS, and prints"
check "$out" "selftest: PASSED" "its recording replays byte-exact"

out=$(boot musl_sysprobe)
echo "$out" | grep -E "linux:|sysprobe:|selftest: (verify|perturbed replay)" | sed "s/^/  /"
check "$out" "linux: pid 1: system call 333 is not implemented" "an unimplemented call is reported"
check "$out" "syscall 333 -> -1 errno ENOSYS" "... and returns ENOSYS to a program that carries on"
check "$out" "monotonic advanced yes, realtime agrees yes, year plausible" "the clocks agree and advance"
check "$out" "getrandom 64 bytes, 6[0-4] nonzero; tls ok" "getrandom fills its buffer"
check "$out" "selftest: verify rc=0: [0-9]+ epochs replayed, 0 diverged" \
      "clock and random values replay identically"
check "$out" "selftest: perturbed replay: divergence detected" "a replay with perturbed entropy is caught"

if [ $fail -ne 0 ]; then echo "FAILED"; exit 1; fi
echo "PASSED: unmodified static Linux programs run, and their system calls replay exactly"
