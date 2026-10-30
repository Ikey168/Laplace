#!/usr/bin/env bash
# In-QEMU loader gate (#240).
#
#  1. A static musl program compiled outside the kernel, passed as a Multiboot
#     module, starts with the arguments and environment from the command line
#     (run="/musl_args.elf one two" env="FOO=bar HOME=/root"); so does the same
#     program built as a static PIE. Both recordings replay byte-exact.
#  2. A recording on disk names the binaries it ran (path and CRC): booting it
#     again with a different binary under that name refuses to resume.
set -euo pipefail
cd "$(dirname "$0")/../.."

QEMU=${QEMU:-qemu-system-x86_64}
MUSL_CC=${MUSL_CC:-musl-gcc}
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
fail=0
check() { if echo "$1" | grep -qE "$2"; then echo "[ok] $3"; else echo "[FAIL] $3"; fail=1; fi; }

$MUSL_CC -march=x86-64 -static -O2 -o "$W/musl_args.elf" user/linux/args.c
$MUSL_CC -march=x86-64 -static-pie -O2 -o "$W/musl_args_pie.elf" user/linux/args.c

for m in musl_args musl_args_pie; do
    out=$(timeout 120 "$QEMU" -m 256M -display none -no-reboot \
        -device isa-debug-exit,iobase=0xf4,iosize=0x04 -kernel build/laplace.elf \
        -initrd "$W/$m.elf" -append "run=\"/$m.elf one two\" env=\"FOO=bar HOME=/root\" selftest exit" \
        -serial stdio -serial null -serial null 2>&1 | tr -d '\r' || true)
    echo "$out" | grep -E "args:" | sed "s/^/  [$m] /"
    check "$out" "args: argc 3 \[/$m.elf\] \[one\] \[two\]" "$m gets its arguments"
    check "$out" "args: env \[FOO=bar\] \[HOME=/root\]" "$m gets its environment"
    check "$out" "selftest: PASSED" "$m's recording replays byte-exact"
done

# Record with one binary, then boot the recording with another of that name.
mkdir -p "$W/v2"
cp "$W/musl_args_pie.elf" "$W/v2/musl_args.elf"
truncate -s 64M "$W/disk.img"
boot_disk() {   # boot_disk <module>
    timeout 20 "$QEMU" -m 256M -display none -no-reboot -kernel build/laplace.elf \
        -initrd "$1" -append "run=\"/musl_args.elf one two\"" \
        -drive "file=$W/disk.img,format=raw,if=ide,index=0" \
        -serial stdio -serial null -serial null 2>&1 | tr -d '\r' || true
}
out=$(boot_disk "$W/musl_args.elf")
check "$out" "args: argc 3" "the first boot records musl_args to the disk"
out=$(boot_disk "$W/v2/musl_args.elf")
echo "$out" | grep -E "timetravel: the recording|boot: (resumed|the recording)" | sed "s/^/  /"
check "$out" "the recording ran .*musl_args.elf, which this boot has in a different version" \
      "a changed binary is detected"
check "$out" "could not be restored; cold boot" "... and the recording is not resumed with it"

if [ $fail -ne 0 ]; then echo "FAILED"; exit 1; fi
echo "PASSED: programs load from outside the kernel with argv and envp; recordings name their binaries"
