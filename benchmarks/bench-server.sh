#!/bin/bash
# bench-server.sh — Per-Slot Memory Tracking Benchmark (branch: per-slot-memory)
#
# Start a single valkey-server instance with cluster enabled for benchmarking
# the per-slot-memory tracking overhead. The baseline comparison is done against
# a separate build (e.g. unstable) without the per-slot-memory changes.
#
# Usage:
#   ./benchmarks/bench-server.sh start
#   ./benchmarks/bench-server.sh stop

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
SERVER="$ROOT_DIR/.build-debug/bin/valkey-server"

PORT=7720
DIR="/tmp/valkey-bench-per-slot-memory"

if [ ! -x "$SERVER" ]; then
    echo "ERROR: server not found at $SERVER — build first."
    exit 1
fi

start_server() {
    mkdir -p "$DIR"

    echo "Starting cluster-enabled server on port $PORT (per-slot-memory branch)..."
    "$SERVER" \
        --port $PORT \
        --cluster-enabled yes \
        --cluster-node-timeout 5000 \
        --protected-mode no \
        --dir "$DIR" \
        --logfile "$DIR/server.log" \
        --daemonize yes \
        --save ""

    # Wait for server to be ready.
    for i in $(seq 1 30); do
        if "$ROOT_DIR/.build-debug/bin/valkey-cli" -p $PORT ping 2>/dev/null | grep -q PONG; then
            break
        fi
        sleep 0.1
    done

    # Assign all slots so writes succeed.
    echo "Assigning all slots to cluster node..."
    "$ROOT_DIR/.build-debug/bin/valkey-cli" -p $PORT cluster addslotsrange 0 16383 >/dev/null

    # Wait for cluster state OK.
    for i in $(seq 1 30); do
        state=$("$ROOT_DIR/.build-debug/bin/valkey-cli" -p $PORT cluster info 2>/dev/null | grep -o 'cluster_state:[a-z]*')
        [ "$state" = "cluster_state:ok" ] && break
        sleep 0.2
    done

    echo "Server ready."
    echo "  Port: $PORT"
    echo "  Logs: $DIR/server.log"
    echo ""
    echo "Run the baseline benchmark against a separate unstable build on a"
    echo "different port, then compare the results."
}

stop_server() {
    echo "Stopping server..."
    "$ROOT_DIR/.build-debug/bin/valkey-cli" -p $PORT shutdown nosave 2>/dev/null || true
    rm -rf "$DIR"
    echo "Done."
}

case "${1:-start}" in
    start) start_server ;;
    stop)  stop_server ;;
    *)     echo "Usage: $0 {start|stop}"; exit 1 ;;
esac
