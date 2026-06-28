#!/bin/bash
# bench-client.sh — Per-Slot Memory Tracking Benchmark (branch: per-slot-memory)
#
# Run stream benchmarks against the server started by bench-server.sh and
# produce a results summary. To compare against unstable (baseline), run the
# same script against a separate unstable build on a different port.
#
# Prerequisites: run bench-server.sh start first.
#
# Usage:
#   ./benchmarks/bench-client.sh [PORT] [NUM_OPS] [NUM_CLIENTS]
#
# Defaults: port 7720, 1,000,000 ops, 50 clients.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BENCH="$ROOT_DIR/.build-debug/bin/valkey-benchmark"
CLI="$ROOT_DIR/.build-debug/bin/valkey-cli"

PORT=${1:-7720}
NUM_OPS=${2:-1000000}
NUM_CLIENTS=${3:-50}
RESULTS_DIR="$SCRIPT_DIR/results"

if [ ! -x "$BENCH" ]; then
    echo "ERROR: valkey-benchmark not found at $BENCH — build first."
    exit 1
fi

mkdir -p "$RESULTS_DIR"

# Helper: run a benchmark, extract ops/sec and p50/p99 latency.
run_bench() {
    local label="$1"
    shift
    local outfile="$RESULTS_DIR/${label}_port${PORT}.txt"

    "$BENCH" -p "$PORT" -n "$NUM_OPS" -c "$NUM_CLIENTS" "$@" > "$outfile" 2>&1

    local ops=$(grep -oE '[0-9]+\.[0-9]+ requests per second' "$outfile" | head -1 | grep -oE '[0-9]+\.[0-9]+')
    local p50=$(grep -E '50\.0+%' "$outfile" | grep -oE '[0-9]+\.[0-9]+' | tail -1)
    local p99=$(grep -E '99\.0+%' "$outfile" | grep -oE '[0-9]+\.[0-9]+' | tail -1)

    echo "$ops $p50 $p99"
}

# Helper: pre-populate a stream with N entries.
prepopulate() {
    local key="$1"
    local count="$2"
    echo "  Pre-populating $key with $count entries on port $PORT..."
    "$BENCH" -p "$PORT" -n "$count" -c "$NUM_CLIENTS" -t xadd --xadd-stream-key "$key" > /dev/null 2>&1
}

echo "============================================================"
echo " Per-Slot Memory Tracking — Stream Benchmark"
echo " Branch: per-slot-memory"
echo " Port: $PORT  Ops: $NUM_OPS  Clients: $NUM_CLIENTS"
echo "============================================================"
echo ""

# Flush server.
"$CLI" -p $PORT FLUSHALL >/dev/null

# --- XADD (small entries) ---
echo "[1/4] XADD (small entries)..."
read ops1 p50_1 p99_1 <<< $(run_bench "xadd_small" -t xadd)
"$CLI" -p $PORT FLUSHALL >/dev/null

# --- XADD (large entries, 512-byte value) ---
echo "[2/4] XADD (large entries, 512-byte value)..."
read ops2 p50_2 p99_2 <<< $(run_bench "xadd_large" -t xadd -d 512)
"$CLI" -p $PORT FLUSHALL >/dev/null

# --- XTRIM ---
echo "[3/4] XTRIM MAXLEN (pre-populate then trim)..."
prepopulate "bench_trim" $NUM_OPS
TRIM_OPS=$((NUM_OPS / 10))
read ops3 p50_3 p99_3 <<< $(run_bench "xtrim" -n "$TRIM_OPS" XTRIM bench_trim MAXLEN $((NUM_OPS - TRIM_OPS)))
"$CLI" -p $PORT FLUSHALL >/dev/null

# --- XDEL ---
echo "[4/4] XDEL (pre-populate then delete by ID)..."
prepopulate "bench_del" 100000
SAMPLE_ID=$("$CLI" -p $PORT XREVRANGE bench_del + - COUNT 1 | head -1 | tr -d '"')
XDEL_OPS=100000
read ops4 p50_4 p99_4 <<< $(run_bench "xdel" -n "$XDEL_OPS" XDEL bench_del "$SAMPLE_ID")
"$CLI" -p $PORT FLUSHALL >/dev/null

# --- Print results ---
echo ""
echo "============================================================"
echo " Results (port $PORT)"
echo "============================================================"
echo ""
printf "%-25s  %12s  %8s  %8s\n" "Command" "ops/sec" "p50(ms)" "p99(ms)"
printf "%-25s  %12s  %8s  %8s\n" "-------" "-------" "-------" "-------"
printf "%-25s  %12s  %8s  %8s\n" "XADD (small)"  "$ops1" "$p50_1" "$p99_1"
printf "%-25s  %12s  %8s  %8s\n" "XADD (512B)"   "$ops2" "$p50_2" "$p99_2"
printf "%-25s  %12s  %8s  %8s\n" "XTRIM MAXLEN"  "$ops3" "$p50_3" "$p99_3"
printf "%-25s  %12s  %8s  %8s\n" "XDEL"          "$ops4" "$p50_4" "$p99_4"

echo ""
echo "Compare these numbers against a run of the same script on an unstable"
echo "build (without per-slot-memory changes) to measure the tracking overhead."
echo ""
echo "Raw output saved in: $RESULTS_DIR/"
