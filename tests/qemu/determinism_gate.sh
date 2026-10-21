#!/usr/bin/env bash
# In-QEMU determinism gate (#241).
#
#  1. tscprobe reads the clock and the CPU's identity with raw RDTSC, RDTSCP,
#     and CPUID and branches on them; its recording must replay byte-exact, on
#     the default CPU model and on -cpu max.
#  2. rdrandleak executes RDRAND, which nothing can trap; verification must
#     report the divergence and name that exact instruction (its address is
#     read from the ELF).
set -euo pipefail
cd "$(dirname "$0")/../.."

QEMU=${QEMU:-qemu-system-x86_64}
fail=0

boot() {   # boot <cpu> <program>
    timeout 300 "$QEMU" -cpu "$1" -m 256M -display none -no-reboot \
        -device isa-debug-exit,iobase=0xf4,iosize=0x04 \
        -kernel build/laplace.elf -append "run=$2 selftest exit" \
        -serial stdio -serial null -serial null 2>&1 || true
}

for cpu in qemu64 max; do
    out=$(boot "$cpu" tscprobe)
    echo "$out" | grep -E "tscprobe:|selftest: (verify|perturbed)" | sed "s/^/  [$cpu] /"
    if echo "$out" | grep -q "selftest: PASSED"; then
        echo "[ok] tscprobe on -cpu $cpu replays byte-exact"
    else
        echo "[FAIL] tscprobe on -cpu $cpu"; fail=1
    fi
done

want=$(objdump -d build/user/rdrandleak.elf | awk '/\trdrand /{sub(":","",$1); print $1; exit}')
out=$(boot max rdrandleak)
echo "$out" | grep -E "selftest: (verify|first|leak)" | sed "s/^/  [max] /"
if echo "$out" | grep -q "selftest: leak: rdrand at rip $want "; then
    echo "[ok] the RDRAND leak is caught and located at $want"
else
    echo "[FAIL] the RDRAND leak at $want was not reported"; fail=1
fi

if [ $fail -ne 0 ]; then echo "FAILED"; exit 1; fi
echo "PASSED: raw clock and CPUID reads replay exactly; an untrappable leak is named"
