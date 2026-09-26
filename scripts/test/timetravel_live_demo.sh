#!/usr/bin/env bash
#
# Host harness for the time-travel storage stack (#200, #230). This does not
# boot anything: the booted end-to-end test is tests/qemu/timetravel_e2e.py.
#
# Builds and runs tests/timetravel_live_e2e.c over the real keyframe retention
# store (#195), input journal (#194), divergence detector (#197), and MCP loop
# (#199), with a one-word model of machine state on a RAM-array disk. Exits
# non-zero on any mismatch.

set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

BIN="$(mktemp -d)/timetravel_live_e2e"

echo "==> building the host harness"
gcc -Iinclude -Wall -o "$BIN" \
    tests/timetravel_live_e2e.c \
    kernel/keyframe_store.c kernel/snapshot_store.c kernel/keyframe_ring.c \
    kernel/journal_capture.c kernel/checkpoint_journal.c \
    kernel/divergence.c kernel/divergence_scan.c \
    kernel/mcp.c kernel/mcp_server.c

echo "==> running: record, drive rewind/reverse over MCP, check for divergence"
"$BIN"
RC=$?
if [ $RC -ne 0 ]; then
    echo "FAILED: headless live end-to-end run reported a mismatch"
    exit $RC
fi

echo "PASSED: host harness green (the booted test is tests/qemu/timetravel_e2e.py)"
exit 0
