#!/usr/bin/env bash
# A release tarball works on its own: unpacked somewhere else, without the
# source tree, its launcher records and verifies the heisenbug demo, Lua, and a
# dynamically linked program, gdb reads the recorded machine, and the ISO
# boots.
#
#   tests/release/smoke.sh build/dist/laplace-VERSION-x86_64.tar.gz
set -euo pipefail

tarball=$(readlink -f "$1")
version=$(basename "$tarball" | sed -n 's/^laplace-\(.*\)-x86_64\.tar\.gz$/\1/p')
[ -n "$version" ] || { echo "not a release tarball: $tarball" >&2; exit 2; }
work=$(mktemp -d)
trap 'rm -r -- "$work"' EXIT
tar -C "$work" -xzf "$tarball"
rel=$work/laplace-$version-x86_64
cd "$work"
fail=0

check() {  # check <output> <pattern> <description>
    if echo "$1" | grep -qE "$2"; then echo "[ok] $3"; else echo "[FAIL] $3"; fail=1; fi
}

run() {    # run <disk> <kernel options...>: the console, and the status
    local disk=$1
    shift
    local out status=0
    out=$(timeout 900 "$rel/tools/laplace-run" --no-net --disk "$work/$disk" "$@" < /dev/null 2>&1 | tr -d '\r') || status=$?
    echo "$out"
    echo "laplace-run status $status"
}

for f in boot/laplace.elf boot/laplace.iso seed/demo.cpio seed/rootfs.cpio tools/laplace-run \
         tools/laplace-mcp programs/heisenbug.elf README.md LICENSE CHANGELOG.md; do
    [ -e "$rel/$f" ] || { echo "[FAIL] the release has no $f"; fail=1; }
done

out=$(run demo.img selftest exit)
check "$out" "^Laplace $version: a time-traveling debugger" "the kernel names its version ($version)"
check "$out" "recording to the IDE disk" "it records to the launcher's disk"
check "$out" "selftest: PASSED" "the heisenbug demo records, verifies byte-exact, and is debugged backward"
check "$out" "laplace-run status 0" "laplace-run reports the self-test's success"

out=$(run lua.img 'run="/bin/lua /scripts/gate.lua"' interval=10 keyframes=64 selftest exit)
check "$out" "lua: acc -?[0-9.]+, box 4242, kept 2500 of 5000, file 5000 bytes, sum 500500" \
    "Lua 5.4 from the demo seed runs its script"
check "$out" "selftest: verify rc=0: [0-9]+ epochs replayed, 0 diverged" "... and replays byte-exact"

out=$(run dyn.img run=/bin/hello_dyn interval=5 keyframes=64 selftest exit)
check "$out" "hello from /bin/hello_dyn on Linux x86_64" "a dynamically linked program starts through ld-musl"
check "$out" "selftest: verify rc=0: [0-9]+ epochs replayed, 0 diverged" "... and replays byte-exact"

# gdb on the stopped heisenbug, with the release's symbols.
if command -v gdb >/dev/null; then
    port=$((20000 + RANDOM % 20000))
    GDB_PORT=$port MCP_PORT=$((port + 1)) "$rel/tools/laplace-run" --no-net --disk "$work/gdb.img" \
        < /dev/null > "$work/gdb-console.log" 2>&1 &
    qpid=$!
    # The heisenbug fires when preemption, the clock, and a random payload
    # line up, which can take a while; gdb attaching to a machine that still
    # runs would stop it somewhere else.
    for _ in $(seq 1 600); do
        grep -q "monitor: stopped" "$work/gdb-console.log" 2>/dev/null && break
        sleep 0.5
    done
    stopped=$(tr -d '\r' < "$work/gdb-console.log" | grep -m1 "monitor: stopped" || true)
    check "$stopped" "monitor: stopped \\(break: pid 1 asserted" "the demo stops at the heisenbug's assertion"
    g=$(timeout 300 gdb -q -batch "$rel/programs/heisenbug.elf" -ex 'set remotetimeout 120' \
        -ex "target remote 127.0.0.1:$port" -ex 'watch -l g.batch_limit' -ex 'reverse-continue' \
        -ex 'print g.batch_limit' 2>&1 || true)
    pkill -P "$qpid" 2>/dev/null || true      # QEMU, the launcher's child
    wait "$qpid" 2>/dev/null || true
    check "$g" "Hardware watchpoint 1: -location g.batch_limit" "gdb attaches to the stopped machine"
    check "$g" "New value = 48" "watch + reverse-continue finds the clobbering store"
    echo "$g" | grep -q "New value = 48" || echo "$g" | tail -12 | sed 's/^/  gdb> /'
else
    echo "[skip] gdb is not installed"
fi

# The ISO: GRUB boots the kernel, which runs the demo until it stops.
timeout 120 "${QEMU:-qemu-system-x86_64}" -m 256M -display none -no-reboot -cdrom "$rel/boot/laplace.iso" \
    -serial "file:$work/iso.log" -serial null -serial null < /dev/null > /dev/null 2>&1 &
ipid=$!
for _ in $(seq 1 200); do
    grep -q "monitor: stopped" "$work/iso.log" 2>/dev/null && break
    sleep 0.5
done
kill "$ipid" 2>/dev/null || true
wait "$ipid" 2>/dev/null || true
iso=$(tr -d '\r' < "$work/iso.log" 2>/dev/null || true)
check "$iso" "^Laplace $version: a time-traveling debugger" "the ISO boots the kernel through GRUB"
check "$iso" "monitor: stopped" "... which records the demo until it stops"

if [ "$fail" -ne 0 ]; then echo "FAILED"; exit 1; fi
echo "PASSED: the $version release records, replays, and debugs on its own"
