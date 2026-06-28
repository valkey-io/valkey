#!/bin/bash
# bench-client.sh — Per-Slot Memory Tracking Benchmark (branch: per-slot-memory)
#
# Run stream benchmarks against a server and produce a results summary.
# Each test is run 3 times and the reported numbers are the average.
# To compare against unstable (baseline), run the same script against a
# separate unstable build on a different port/host.
#
# Prerequisites: run bench-server.sh start first (or have a server running).

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BENCH="$(command -v valkey-benchmark)"
CLI="$(command -v valkey-cli)"

HOST="127.0.0.1"
PORT=6379
NUM_OPS=1000000
NUM_CLIENTS=50
ITERATIONS=3

usage() {
    echo "Usage: $0 [OPTIONS]"
    echo ""
    echo "Options:"
    echo "  -h, --host HOST        Server host (default: $HOST)"
    echo "  -p, --port PORT        Server port (default: $PORT)"
    echo "  -n, --ops NUM_OPS      Number of operations per test (default: $NUM_OPS)"
    echo "  -c, --clients NUM      Number of concurrent clients (default: $NUM_CLIENTS)"
    echo "  -i, --iterations NUM   Iterations per test, results averaged (default: $ITERATIONS)"
    echo "  --help                 Show this help"
    echo ""
    echo "Examples:"
    echo "  $0"
    echo "  $0 -p 7720"
    echo "  $0 -h 192.168.1.10 -p 6379 -n 500000 -c 20"
    echo "  $0 -i 5  # run each test 5 times"
    exit 0
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        -h|--host) HOST="$2"; shift 2 ;;
        -p|--port) PORT="$2"; shift 2 ;;
        -n|--ops) NUM_OPS="$2"; shift 2 ;;
        -c|--clients) NUM_CLIENTS="$2"; shift 2 ;;
        -i|--iterations) ITERATIONS="$2"; shift 2 ;;
        --help) usage ;;
        *) echo "Unknown option: $1"; usage ;;
    esac
done

if [ -z "$BENCH" ] || [ ! -x "$BENCH" ]; then
    echo "ERROR: valkey-benchmark not found in your PATH."
    exit 1
fi

if [ -z "$CLI" ] || [ ! -x "$CLI" ]; then
    echo "ERROR: valkey-cli not found in your PATH."
    exit 1
fi

RESULTS_DIR="$SCRIPT_DIR/results"
mkdir -p "$RESULTS_DIR"

# Helper: reset server state before each test.
reset_server() {
    "$CLI" -h "$HOST" -p $PORT FLUSHALL >/dev/null
    "$CLI" -h "$HOST" -p $PORT CONFIG RESETSTAT >/dev/null
}

# Helper: run a single benchmark iteration, extract ops/sec and p50/p99 latency.
run_bench_once() {
    local label="$1"
    local iter="$2"
    shift 2
    local outfile="$RESULTS_DIR/${label}_${HOST}_${PORT}_run${iter}.txt"

    "$BENCH" -h "$HOST" -p "$PORT" -n "$NUM_OPS" -c "$NUM_CLIENTS" "$@" > "$outfile" 2>&1

    local ops=$(grep -oE '[0-9]+\.[0-9]+ requests per second' "$outfile" | head -1 | grep -oE '[0-9]+\.[0-9]+')
    local p50=$(grep -E '^50\.000%' "$outfile" | grep -oE '<= [0-9]+\.[0-9]+' | grep -oE '[0-9]+\.[0-9]+')
    local p99=$(grep -E '^99\.[0-9]+%' "$outfile" | head -1 | grep -oE '<= [0-9]+\.[0-9]+' | grep -oE '[0-9]+\.[0-9]+')

    echo "$ops $p50 $p99"
}

# Helper: run a benchmark N times, return averaged ops/sec p50 p99.
run_bench() {
    local label="$1"
    shift

    local sum_ops=0 sum_p50=0 sum_p99=0
    for iter in $(seq 1 $ITERATIONS); do
        reset_server
        # Re-setup for tests that need pre-population (caller handles this via
        # the setup_fn mechanism below).
        if [ -n "$BENCH_SETUP_FN" ]; then
            $BENCH_SETUP_FN
        fi

        local result
        result=$(run_bench_once "$label" "$iter" "$@")
        local ops=$(echo "$result" | awk '{print $1}')
        local p50=$(echo "$result" | awk '{print $2}')
        local p99=$(echo "$result" | awk '{print $3}')

        sum_ops=$(echo "$sum_ops + ${ops:-0}" | bc)
        sum_p50=$(echo "$sum_p50 + ${p50:-0}" | bc)
        sum_p99=$(echo "$sum_p99 + ${p99:-0}" | bc)
    done

    local avg_ops=$(echo "scale=2; $sum_ops / $ITERATIONS" | bc)
    local avg_p50=$(echo "scale=3; $sum_p50 / $ITERATIONS" | bc)
    local avg_p99=$(echo "scale=3; $sum_p99 / $ITERATIONS" | bc)

    echo "$avg_ops $avg_p50 $avg_p99"
}

# Helper: pre-populate a stream with N entries.
prepopulate() {
    local key="$1"
    local count="$2"
    "$BENCH" -h "$HOST" -p "$PORT" -n "$count" -c "$NUM_CLIENTS" \
        XADD "$key" '*' field __data__ > /dev/null 2>&1
}

echo "============================================================"
echo " Per-Slot Memory Tracking — Stream Benchmark"
echo " Branch: per-slot-memory"
echo " Server: $HOST:$PORT  Ops: $NUM_OPS  Clients: $NUM_CLIENTS"
echo " Iterations: $ITERATIONS (results averaged)"
echo " Benchmark: $BENCH"
echo "============================================================"
echo ""

# --- XADD (small entries) ---
echo "[1/4] XADD (3B value) — $ITERATIONS iterations..."
BENCH_SETUP_FN=""
read ops1 p50_1 p99_1 <<< $(run_bench "xadd_small" -t xadd)

# --- XADD (large entries, 512-byte value) ---
echo "[2/4] XADD (large entries, 512-byte value) — $ITERATIONS iterations..."
BENCH_SETUP_FN=""
read ops2 p50_2 p99_2 <<< $(run_bench "xadd_large" -t xadd -d 512)

# --- XTRIM ---
echo "[3/4] XTRIM MAXLEN (pre-populate then trim) — $ITERATIONS iterations..."
TRIM_OPS=$((NUM_OPS / 10))
setup_xtrim() { prepopulate "bench_trim" $NUM_OPS; }
BENCH_SETUP_FN="setup_xtrim"
read ops3 p50_3 p99_3 <<< $(run_bench "xtrim" -n "$TRIM_OPS" XTRIM bench_trim MAXLEN $((NUM_OPS - TRIM_OPS)))

# --- XDEL ---
echo "[4/4] XDEL (pre-populate then delete by ID) — $ITERATIONS iterations..."
XDEL_OPS=100000
BENCH_SETUP_FN=""
sum_ops=0; sum_p50=0; sum_p99=0
for iter in $(seq 1 $ITERATIONS); do
    reset_server
    prepopulate "bench_del" 100000
    SAMPLE_ID=$("$CLI" -h "$HOST" -p $PORT XREVRANGE bench_del + - COUNT 1 | head -1 | tr -d '"')
    result=$(run_bench_once "xdel" "$iter" -n "$XDEL_OPS" XDEL bench_del "$SAMPLE_ID")
    ops=$(echo "$result" | awk '{print $1}')
    p50=$(echo "$result" | awk '{print $2}')
    p99=$(echo "$result" | awk '{print $3}')
    sum_ops=$(echo "$sum_ops + ${ops:-0}" | bc)
    sum_p50=$(echo "$sum_p50 + ${p50:-0}" | bc)
    sum_p99=$(echo "$sum_p99 + ${p99:-0}" | bc)
done
ops4=$(echo "scale=2; $sum_ops / $ITERATIONS" | bc)
p50_4=$(echo "scale=3; $sum_p50 / $ITERATIONS" | bc)
p99_4=$(echo "scale=3; $sum_p99 / $ITERATIONS" | bc)

# --- Print results ---
echo ""
echo "============================================================"
echo " Results ($HOST:$PORT) — averaged over $ITERATIONS iterations"
echo "============================================================"
echo ""
printf "%-25s  %12s  %8s  %8s\n" "Command" "ops/sec" "p50(ms)" "p99(ms)"
printf "%-25s  %12s  %8s  %8s\n" "-------" "-------" "-------" "-------"
printf "%-25s  %12s  %8s  %8s\n" "XADD (3B)"     "$ops1" "$p50_1" "$p99_1"
printf "%-25s  %12s  %8s  %8s\n" "XADD (512B)"   "$ops2" "$p50_2" "$p99_2"
printf "%-25s  %12s  %8s  %8s\n" "XTRIM MAXLEN"  "$ops3" "$p50_3" "$p99_3"
printf "%-25s  %12s  %8s  %8s\n" "XDEL"          "$ops4" "$p50_4" "$p99_4"

echo ""
echo "Compare these numbers against a run of the same script on an unstable"
echo "build (without per-slot-memory changes) to measure the tracking overhead."
echo ""
echo "Raw output saved in: $RESULTS_DIR/"
