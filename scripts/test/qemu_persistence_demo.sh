#!/usr/bin/env bash
#
# Persistence resume demo (#202, #231): yank power, boot, resume.
#
#   1. Headless: builds and runs tests/persistence_ide_resume_e2e.c, which
#      drives the real checkpoint engine and snapshot store through the same
#      checkpoint_ide binding the kernel uses, over a file-backed disk image.
#
#   2. Booted: boots the Laplace kernel in QEMU with an IDE disk, runs the
#      persistent counter (user/laplace/counter.c: no save, no load), kills QEMU
#      with SIGKILL (a power cut) at several points, and boots again from the
#      same disk. Every boot after the first must resume the machine from the
#      newest keyframe on the disk, and the counter must never go backward past
#      what it had printed before the cut.
#
# Needs gcc and, for layer 2, nasm and qemu-system-x86_64 (set
# LAPLACE_SKIP_QEMU=1 to run layer 1 only).

set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

# --- Layer 1: headless resume gate over the IDE-backed store ---
WORK="$(mktemp -d)"
BIN="$WORK/ide_resume"
DISK_HEADLESS="$WORK/ide.img"
trap 'rm -rf "$WORK"' EXIT

echo "==> building the headless IDE-resume harness"
gcc -Iinclude -O2 -Wall -o "$BIN" \
    tests/persistence_ide_resume_e2e.c \
    kernel/checkpoint.c kernel/snapshot_store.c \
    kernel/checkpoint_extstate.c kernel/checkpoint_barrier.c \
    kernel/checkpoint_ide.c

echo "==> power cycle over the durable IDE store"
echo "-- boot fresh, count to 5, cut power --"
"$BIN" "$DISK_HEADLESS" run 5
echo "-- reboot: counter must resume at 5 from the IDE store --"
"$BIN" "$DISK_HEADLESS" verify 5
echo "-- count 5 more, cut power, reboot --"
"$BIN" "$DISK_HEADLESS" run 5
"$BIN" "$DISK_HEADLESS" verify 10
echo "[ok] headless IDE-store resume gate passed"

# --- Layer 2: the booted kernel, power-cut and resumed ---
if [ "${LAPLACE_SKIP_QEMU:-0}" = "1" ]; then
    echo "[skip] LAPLACE_SKIP_QEMU=1: booted layer not run"
    exit 0
fi
QEMU="${QEMU:-qemu-system-x86_64}"
command -v "$QEMU" >/dev/null 2>&1 || { echo "FAILED: $QEMU not found"; exit 1; }

echo "==> building the kernel"
make -s kernel >/dev/null || { echo "FAILED: kernel build"; exit 1; }

DISK="$WORK/disk.img"
truncate -s 64M "$DISK"

boot_and_cut() {   # $1 = seconds before the power cut, $2 = console log
    rm -f "$WORK/qemu.pid"
    "$QEMU" -m 256M -display none -no-reboot -kernel build/laplace.elf \
        -append "run=counter" -drive "file=$DISK,format=raw,if=ide,index=0" \
        -serial "file:$2" -serial null -serial null \
        -pidfile "$WORK/qemu.pid" -daemonize || return 1
    sleep "$1"
    kill -9 "$(cat "$WORK/qemu.pid")" 2>/dev/null   # pull the plug
    sleep 0.5
}

counters() { sed -n 's/^counter: \([0-9]*\).*/\1/p' "$1" | tr -d '\r'; }

fail=0
last=0
cycle=0
for secs in 3 4 2.5 3.5; do
    cycle=$((cycle + 1))
    log="$WORK/boot$cycle.log"
    boot_and_cut "$secs" "$log" || { echo "FAILED: QEMU did not start"; exit 1; }
    first=$(counters "$log" | head -1)
    newest=$(counters "$log" | tail -1)
    if [ "$cycle" = 1 ]; then
        grep -q "store formatted" "$log" || { echo "  [FAIL] boot 1 did not format a fresh store"; fail=1; }
        echo "[boot 1] fresh disk: counted ${first:-?} .. ${newest:-?}, then power cut after ${secs}s"
        [ "${first:-0}" = 20000 ] || { echo "  [FAIL] boot 1 did not start from zero"; fail=1; }
    else
        resumed=$(sed -n 's/.*resumed the machine from keyframe \([0-9]*\).*/\1/p' "$log" | tr -d '\r')
        echo "[boot $cycle] resumed from keyframe ${resumed:-NONE}: counted ${first:-?} .. ${newest:-?} (had printed $last before the cut), cut after ${secs}s"
        [ -n "$resumed" ] || { echo "  [FAIL] boot $cycle cold-booted instead of resuming"; fail=1; }
        [ -n "$first" ] && [ "$first" -ge "$last" ] || { echo "  [FAIL] counter went back from $last to ${first:-nothing}"; fail=1; }
    fi
    [ -n "$newest" ] || { echo "  [FAIL] boot $cycle printed no counter"; fail=1; newest=$last; }
    last=$newest
done

if [ "$fail" = 0 ]; then
    echo "PASSED: the booted machine resumed from the IDE disk after every power cut"
    exit 0
fi
echo "FAILED: see above"; for l in "$WORK"/boot*.log; do echo "--- $l"; tail -5 "$l"; done
exit 1
