#!/bin/bash
# bench-server.sh — Per-Slot Memory Tracking Benchmark (branch: per-slot-memory)
#
# Start a single valkey-server instance with cluster enabled for benchmarking
# the per-slot-memory tracking overhead. The baseline comparison is done against
# a separate build (e.g. unstable) without the per-slot-memory changes.

set -e

SERVER="$(command -v valkey-server)"
CLI="$(command -v valkey-cli)"

HOST="127.0.0.1"
PORT=6379

usage() {
    echo "Usage: $0 {start|stop} [OPTIONS]"
    echo ""
    echo "Options:"
    echo "  -h, --host HOST    Bind address (default: $HOST)"
    echo "  -p, --port PORT    Server port (default: $PORT)"
    echo "  --help             Show this help"
    echo ""
    echo "Examples:"
    echo "  $0 start"
    echo "  $0 start -p 7720"
    echo "  $0 start -h 0.0.0.0 -p 7720"
    echo "  $0 stop -p 7720"
    echo "  VALKEY_SERVER=/path/to/valkey-server $0 start"
    exit 0
}

# Parse arguments.
ACTION=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        start|stop) ACTION="$1"; shift ;;
        -h|--host) HOST="$2"; shift 2 ;;
        -p|--port) PORT="$2"; shift 2 ;;
        --help) usage ;;
        *) echo "Unknown option: $1"; usage ;;
    esac
done

if [ -z "$ACTION" ]; then
    echo "ERROR: must specify 'start' or 'stop'."
    echo ""
    usage
fi

if [ -z "$SERVER" ] || [ ! -x "$SERVER" ]; then
    echo "ERROR: valkey-server not found. Set VALKEY_SERVER or add it to PATH."
    exit 1
fi

if [ -z "$CLI" ] || [ ! -x "$CLI" ]; then
    echo "ERROR: valkey-cli not found. Set VALKEY_CLI or add it to PATH."
    exit 1
fi

DIR="/tmp/valkey-bench-per-slot-memory-${PORT}"

start_server() {
    mkdir -p "$DIR"

    echo "Starting cluster-enabled server on $HOST:$PORT (per-slot-memory branch)..."
    echo "  Server: $SERVER"
    "$SERVER" \
        --bind "$HOST" \
        --port $PORT \
        --cluster-enabled yes \
        --cluster-node-timeout 5000 \
        --protected-mode no \
        --save "" \
        &
        

    # Wait for server to be ready.
    for i in $(seq 1 30); do
        if "$CLI" -h "$HOST" -p $PORT ping 2>/dev/null | grep -q PONG; then
            break
        fi
        sleep 0.1
    done

    # Assign all slots so writes succeed.
    echo "Assigning all slots to cluster node..."
    "$CLI" -h "$HOST" -p $PORT cluster addslotsrange 0 16383 >/dev/null

    # Wait for cluster state OK.
    for i in $(seq 1 30); do
        state=$("$CLI" -h "$HOST" -p $PORT cluster info 2>/dev/null | grep -o 'cluster_state:[a-z]*')
        [ "$state" = "cluster_state:ok" ] && break
        sleep 0.2
    done

    echo "Server ready."
    echo "  Host: $HOST"
    echo "  Port: $PORT"
    echo ""
    echo "Run the baseline benchmark against a separate unstable build on a"
    echo "different port, then compare the results."
}

stop_server() {
    echo "Stopping server on $HOST:$PORT..."
    "$CLI" -h "$HOST" -p $PORT shutdown nosave 2>/dev/null || true
    rm -rf "$DIR"
    echo "Done."
}

case "$ACTION" in
    start) start_server ;;
    stop)  stop_server ;;
esac
