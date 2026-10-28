#!/usr/bin/env bash
# ==============================================================================
# tools/crash_demo.sh - the README durability demo (Phase 6, checkpoint C6)
# ==============================================================================
#
# Loads keys into a fresh database, kill -9's the loader mid-flight, reopens
# (which runs WAL recovery), and verifies the survivors form a clean prefix
# of the load with correct values and an agreeing key count. Proves the
# engine never hands back a corrupt tree after a hard kill.
#
#   tools/crash_demo.sh [num_keys]     # default 500,000
#
# Uses the kvstore_demo binary (built by any preset). sync_on_write is off in
# the loader, so this demonstrates crash-CONSISTENCY (no corruption, clean
# prefix), not per-write durability - see src/main.cc.
#
set -euo pipefail

cd "$(dirname "$0")/.."

N="${1:-500000}"
DEMO="${KVSTORE_DEMO_BIN:-build/release/kvstore_demo}"

if [ ! -x "$DEMO" ]; then
    echo "building release kvstore_demo..."
    cmake --preset release > /dev/null
    cmake --build --preset release -j "$(getconf _NPROCESSORS_ONLN)" --target kvstore_demo > /dev/null
fi

DB_DIR="$(mktemp -d)/crashdemo"
trap 'rm -rf "$(dirname "$DB_DIR")"' EXIT

echo "== Loading $N keys into $DB_DIR (will be killed mid-flight) =="
"$DEMO" load "$DB_DIR" "$N" &
LOADER=$!

# Let it get well underway, then hard-kill it (no clean shutdown/checkpoint).
sleep 0.6
if kill -0 "$LOADER" 2>/dev/null; then
    kill -9 "$LOADER" 2>/dev/null || true
    echo "== kill -9 sent to loader (pid $LOADER) =="
else
    echo "== loader already finished before the kill (try a larger N) =="
fi
wait "$LOADER" 2>/dev/null || true

echo
echo "== Reopening (runs recovery) and verifying =="
"$DEMO" verify "$DB_DIR" "$N"

echo
echo "== Structural integrity check via kvstore_inspect =="
INSPECT="$(dirname "$DEMO")/kvstore_inspect"
if [ -x "$INSPECT" ]; then
    "$INSPECT" "$DB_DIR" > /dev/null && echo "kvstore_inspect: tree + leaf chain + free list consistent (exit 0)"
else
    echo "kvstore_inspect not built next to the demo - skipping"
fi
