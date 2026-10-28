#!/usr/bin/env bash
# ==============================================================================
# tools/run_bench.sh - one-command benchmark run (Phase 6, checkpoint C6)
# ==============================================================================
#
# Configures + builds the 'bench' preset, runs db_bench (and sqlite_bench if
# it was built), and saves console + JSON output under docs/bench_results/
# tagged with the date and an optional label:
#
#   tools/run_bench.sh                # full run, results in docs/bench_results/
#   tools/run_bench.sh after-fix-X    # label the result directory
#   BENCH_FILTER=ReadUniform tools/run_bench.sh   # subset (not saved as official)
#
# Reproducibility: fixed workload seed (KVSTORE_BENCH_SEED, default 42),
# pinned Google Benchmark version, and each result directory records the
# compiler, flags, machine, and repetition count alongside the raw output.
#
set -euo pipefail

cd "$(dirname "$0")/.."
ROOT="$PWD"

LABEL="${1:-baseline}"
STAMP="$(date +%Y-%m-%d)"
OUT_DIR="docs/bench_results/${STAMP}-${LABEL}"
FILTER="${BENCH_FILTER:-}"
REPS="${BENCH_REPETITIONS:-3}"

cmake --preset bench > /dev/null
cmake --build --preset bench -j "$(getconf _NPROCESSORS_ONLN)" -- db_bench \
    $( [ -f build/bench/CMakeCache.txt ] && grep -q 'KVSTORE_SQLITE3_LIB:FILEPATH=/' build/bench/CMakeCache.txt && echo sqlite_bench )

mkdir -p "$OUT_DIR"

{
    echo "date:      $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "label:     $LABEL"
    echo "host:      $(uname -mrs)"
    if [ "$(uname)" = "Darwin" ]; then
        echo "cpu:       $(sysctl -n machdep.cpu.brand_string)"
        echo "mem_bytes: $(sysctl -n hw.memsize)"
    else
        echo "cpu:       $(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2- | sed 's/^ //')"
    fi
    echo "compiler:  $(cc --version | head -1)"
    echo "build:     Release (-O3 -DNDEBUG), preset 'bench'"
    echo "seed:      ${KVSTORE_BENCH_SEED:-42}"
    echo "reps:      $REPS"
    echo "git:       $(git rev-parse --short HEAD 2>/dev/null || echo n/a) (+ uncommitted work)"
} > "$OUT_DIR/environment.txt"

BENCH_ARGS=(
    "--benchmark_repetitions=$REPS"
    "--benchmark_report_aggregates_only=true"
)
if [ -n "$FILTER" ]; then
    BENCH_ARGS+=("--benchmark_filter=$FILTER")
fi

echo "=== db_bench (PagerDB) ==="
./build/bench/db_bench "${BENCH_ARGS[@]}" \
    --benchmark_out="$OUT_DIR/db_bench.json" --benchmark_out_format=json \
    | tee "$OUT_DIR/db_bench.txt"

if [ -x build/bench/sqlite_bench ]; then
    echo
    echo "=== sqlite_bench (baseline) ==="
    ./build/bench/sqlite_bench "${BENCH_ARGS[@]}" \
        --benchmark_out="$OUT_DIR/sqlite_bench.json" --benchmark_out_format=json \
        | tee "$OUT_DIR/sqlite_bench.txt"
else
    echo "sqlite_bench not built (no system sqlite3) - skipping baseline"
fi

echo
echo "Results saved to $OUT_DIR"
