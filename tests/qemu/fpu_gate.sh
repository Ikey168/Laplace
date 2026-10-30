#!/usr/bin/env bash
# In-QEMU extended-state gate (#237).
#
# Two copies of fpuprobe keep a double in an XMM register (and, with AVX, a
# vector in YMM8) live across kernel entries while the scheduler interleaves
# them. Each checks its results against an integer-only computation and stops
# the machine on a mismatch. Run with FXSAVE (qemu64) and XSAVE+AVX (-cpu max):
# both copies must finish correct and the recording must replay byte-exact.
# A single copy's recording must also catch one flipped bit of XMM15 in a
# restored keyframe.
set -euo pipefail
cd "$(dirname "$0")/../.."

QEMU=${QEMU:-qemu-system-x86_64}
fail=0

boot() {   # boot <cpu> <run list>
    timeout 300 "$QEMU" -cpu "$1" -m 256M -display none -no-reboot \
        -device isa-debug-exit,iobase=0xf4,iosize=0x04 \
        -kernel build/laplace.elf -append "run=$2 selftest exit" \
        -serial stdio -serial null -serial null 2>&1 | tr -d '\r' || true
}

for cpu in qemu64 max; do
    out=$(boot "$cpu" fpuprobe,fpuprobe)
    echo "$out" | grep -E "boot: user FPU|selftest: verify" | sed "s/^/  [$cpu] /"
    oks=$(echo "$out" | grep -c " ok$" || true)
    if [ "$oks" -eq 2 ] && echo "$out" | grep -q "selftest: verify rc=0: .* 0 diverged"; then
        echo "[ok] two interleaved copies kept their FPU state on -cpu $cpu; replay byte-exact"
    else
        echo "[FAIL] -cpu $cpu: $oks of 2 copies correct"; echo "$out" | tail -20; fail=1
    fi
    if [ "$cpu" = max ] && ! echo "$out" | grep -q "avx 50000,100000,150000,200000"; then
        echo "[FAIL] -cpu max: the AVX lanes were not checked"; fail=1
    fi
    out=$(boot "$cpu" fpuprobe)
    if echo "$out" | grep -q "selftest: perturbed XMM15 in a keyframe: divergence detected"; then
        echo "[ok] -cpu $cpu: a flipped XMM15 bit in a keyframe is caught"
    else
        echo "[FAIL] -cpu $cpu: the XMM15 perturbation was not caught"; fail=1
    fi
done

if [ $fail -ne 0 ]; then echo "FAILED"; exit 1; fi
echo "PASSED: x87/SSE/AVX state survives preemption and is part of what replay verifies"
