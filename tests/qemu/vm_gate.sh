#!/usr/bin/env bash
# In-QEMU gate for process memory (#239).
#
#  musl_vmprobe (user/linux/vmprobe.c) runs rounds of malloc/realloc/free,
#  anonymous mmap, mprotect (read-only, no access, writable again), mremap,
#  munmap, madvise(DONTNEED), MAP_FIXED_NOREPLACE, brk up and down, and stack
#  recursion of up to 176 KiB, with keyframes every few ticks so mappings are
#  created, changed, and removed between keyframes. Every keyframe in the
#  window must replay to the recorded state (pages, regions, registers), at
#  512 MiB and at 4 GiB of RAM. Then `musl_vmprobe overflow` recurses without
#  bound: the 8 MiB stack fills page by page, and the first touch below it,
#  in no region, stops the machine with a fault; that run replays too.
set -euo pipefail
cd "$(dirname "$0")/../.."

QEMU=${QEMU:-qemu-system-x86_64}
fail=0

check() {  # check <output> <pattern> <description>
    if echo "$1" | grep -qE "$2"; then echo "[ok] $3"; else echo "[FAIL] $3"; fail=1; fi
}

for mem in 512M 4G; do
    out=$(timeout 300 "$QEMU" -m "$mem" -display none -no-reboot \
        -device isa-debug-exit,iobase=0xf4,iosize=0x04 \
        -kernel build/laplace.elf -append "run=musl_vmprobe interval=3 keyframes=40 selftest exit" \
        -serial stdio -serial null -serial null 2>&1 | tr -d '\r' || true)
    echo "$out" | grep -E "vmprobe:|selftest: (window|verify)" | sed "s/^/  [$mem] /"
    check "$out" "vmprobe: 6 rounds of malloc, mmap, mprotect, mremap, brk, and stack: sum a2950002" \
          "[$mem] the program's memory behaves as on Linux"
    check "$out" "selftest: window epochs [0-9]+\.\.[0-9]+ \(([2-9][0-9]) keyframes\)" \
          "[$mem] keyframes fall throughout the run"
    check "$out" "selftest: verify rc=0: ([2-9][0-9]) epochs replayed, 0 diverged" \
          "[$mem] every keyframe replays to the recorded memory"
    check "$out" "selftest: PASSED" "[$mem] the self-test passes"
done

out=$(timeout 300 "$QEMU" -m 256M -display none -no-reboot \
    -device isa-debug-exit,iobase=0xf4,iosize=0x04 \
    -kernel build/laplace.elf -append "run=\"musl_vmprobe overflow\" selftest exit" \
    -serial stdio -serial null -serial null 2>&1 | tr -d '\r' || true)
echo "$out" | grep -E "monitor: stopped|selftest: verify" | sed "s/^/  [overflow] /"
check "$out" "stopped \(fault: pid 1 vector 14 error [46] address 7fffff7ef[0-9a-f]{3}\)" \
      "a stack overflow faults just below the 8 MiB stack and stops the machine"
check "$out" "selftest: verify rc=0: 1 epochs replayed, 0 diverged" "... and 8 MiB of stack growth replays"

if [ $fail -ne 0 ]; then echo "FAILED"; exit 1; fi
echo "PASSED: brk, mmap, mprotect, mremap, munmap, demand paging, and the stack guard record and replay"
