#!/usr/bin/env bash
#
# utils/rdma-rx-phase-benchmark.sh
#
# Run a persistent-connection, phase-changing SET workload against Valkey/RDMA.
# One valkey-benchmark process and the same RDMA QPs stay alive across phases.
#
# Changing rdma-rx-size requires a new server (MR is registered at connect).
# --static-bounds repeats the same KV sequence at rx=1M, 4M, 16M (independent
# server+benchmark per cell) so a later dynamic-resize branch can compare.
#
#   --transport rxe        software RXE (creates dummy/rxe if needed)
#   --transport physical   existing NIC; never create/delete RDMA devices
#
# Default phases (growth → shrink):
#   4KiB:20s, 64KiB:20s, 1MiB:30s, 4MiB:30s, 64KiB:30s, 4KiB:60s
#
# Results: <repo>/rdma-phase-results/<timestamp>/
#
set -Eeuo pipefail
export LC_ALL=C
export VALKEY_RDMA_BENCH_STATS=1

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

RDMA_TRANSPORT="${RDMA_TRANSPORT:-rxe}"
RDMA_NETDEV="${RDMA_NETDEV:-dummy0}"
RDMA_DEVICE="${RDMA_DEVICE:-rxe_dummy}"
RDMA_BIND_IP="${RDMA_BIND_IP:-${RDMA_IP:-10.200.0.1}}"
RDMA_SERVER_HOST="${RDMA_SERVER_HOST:-$RDMA_BIND_IP}"
RDMA_PREFIX="${RDMA_PREFIX:-24}"
RDMA_PORT="${RDMA_PORT:-16379}"
RDMA_SKIP_MEMLOCK="${RDMA_SKIP_MEMLOCK:-0}"
KEEP_RXE=0
CREATED_NETDEV=0
CREATED_RXE=0
ROLE="all"
RX_SIZE="${RX_SIZE:-1048576}"
RX_SIZES=""
REPEATS=1
CLIENTS=4
PIPELINE=16
THREADS=0
PHASES="4096:20,65536:20,1048576:30,4194304:30,65536:30,4096:60"
SMOKE=0
SERVER_PID=""
SERVER_LOG=""

PHASES_CSV_HEADER='commit,run_id,rx_size,repeat,phase_index,value_size,duration,clients,pipeline,requests,rps,payload_gbps,avg_latency_us,p50_latency_us,p99_latency_us,reconnects,errors,tx_bytes,tx_wait_count,tx_wait_ns,rx_reannounce,reannounces_per_gib,stall_ratio'

log()  { printf '[%s] %s\n' "$(date +%H:%M:%S)" "$*"; }
warn() { log "WARNING: $*"; }
die()  { log "ERROR: $*"; exit 1; }

usage() {
    cat <<EOF
Usage: $0 [--transport rxe|physical] [--smoke] [options]

  --transport rxe|physical   default: rxe (or RDMA_TRANSPORT)
  --bind <ip>                server --rdma-bind (physical / dual-host)
  --server-host <ip>         client -h (defaults to bind ip)
  --port <port>              RDMA port (default 16379)
  --rx-size <bytes>          server rdma-rx-size (default 1048576)
  --rx-sizes <list>          comma list (bytes or 1M/4M/16M); restart server per size
  --repeats <n>              independent runs per rx-size (default 1)
  --static-bounds            rx=1M,4M,16M × 3 repeats, same KV sequence
  --clients <n>              default 4
  --pipeline <n>             default 16
  --phases <spec>            size:seconds,... (default 6-phase growth/shrink)
  --smoke                    32:2,64:2,32:2 for a quick harness check
  --role all|client          all starts a local server; client uses --server-host
  --skip-memlock             do not raise memlock
  --keep-rxe                 do not delete dummy/RXE created by this script

Physical mode never creates, deletes, or modifies RDMA/network devices.
Changing RX size always starts a new server; phases within one run keep the same QPs.
EOF
    exit "${1:-0}"
}

parse_bytes() {
    local s="$1"
    case "$s" in
        *[Kk]) echo $(( ${s%[Kk]} * 1024 )) ;;
        *[Mm]) echo $(( ${s%[Mm]} * 1048576 )) ;;
        *) echo "$s" ;;
    esac
}

parse_args() {
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --transport)
                [[ $# -ge 2 ]] || die "--transport requires rxe|physical"
                RDMA_TRANSPORT="$2"; shift 2 ;;
            --bind) RDMA_BIND_IP="$2"; RDMA_SERVER_HOST="${RDMA_SERVER_HOST:-$2}"; shift 2 ;;
            --server-host) RDMA_SERVER_HOST="$2"; shift 2 ;;
            --port) RDMA_PORT="$2"; shift 2 ;;
            --rx-size) RX_SIZE="$(parse_bytes "$2")"; shift 2 ;;
            --rx-sizes) RX_SIZES="$2"; shift 2 ;;
            --repeats) REPEATS="$2"; shift 2 ;;
            --static-bounds)
                RX_SIZES="1M,4M,16M"
                REPEATS=3
                KEEP_RXE=1
                shift ;;
            --clients) CLIENTS="$2"; shift 2 ;;
            --pipeline) PIPELINE="$2"; shift 2 ;;
            --phases) PHASES="$2"; shift 2 ;;
            --smoke) SMOKE=1; shift ;;
            --role) ROLE="$2"; shift 2 ;;
            --skip-memlock) RDMA_SKIP_MEMLOCK=1; shift ;;
            --keep-rxe) KEEP_RXE=1; shift ;;
            -h|--help) usage 0 ;;
            *) die "unknown argument: $1" ;;
        esac
    done
    [[ "$RDMA_TRANSPORT" == "rxe" || "$RDMA_TRANSPORT" == "physical" ]] \
        || die "--transport must be rxe or physical"
    [[ "$ROLE" == "all" || "$ROLE" == "client" ]] || die "--role must be all or client"
    [[ "$REPEATS" =~ ^[1-9][0-9]*$ ]] || die "--repeats must be a positive integer"
    if (( SMOKE )); then
        PHASES="32:2,64:2,32:2"
        CLIENTS=2
        PIPELINE=2
    fi
}

rxe_link_exists() {
    rdma link show 2>/dev/null | grep -q "^[[:space:]]*link $RDMA_DEVICE/"
}

setup_memlock() {
    [[ $EUID -ne 0 ]] || die "run as a normal user; sudo only for memlock/RXE"
    if [[ "$RDMA_SKIP_MEMLOCK" == "1" ]]; then
        warn "RDMA_SKIP_MEMLOCK=1: not raising memlock"
        return 0
    fi
    sudo -v || die "sudo -v failed"
    sudo prlimit --memlock=unlimited --pid "$$" || die "prlimit --memlock failed"
    [[ "$(ulimit -l)" == "unlimited" ]] || die "memlock is not unlimited"
    log "memlock: unlimited (PID $$, inherited by children)"
}

setup_transport() {
    if [[ "$RDMA_TRANSPORT" == "physical" ]]; then
        log "transport=physical: skipping dummy/RXE create/delete"
        log "  bind=$RDMA_BIND_IP host=$RDMA_SERVER_HOST port=$RDMA_PORT"
        ibv_devices 2>/dev/null | head -5 || warn "ibv_devices returned nothing"
        return 0
    fi
    log "transport=rxe: dummy netdev + rdma_rxe"
    sudo modprobe dummy
    sudo modprobe rdma_rxe
    if ! ip link show dev "$RDMA_NETDEV" >/dev/null 2>&1; then
        sudo ip link add "$RDMA_NETDEV" type dummy
        CREATED_NETDEV=1
    fi
    if ! ip -4 -o addr show dev "$RDMA_NETDEV" | awk -v ip="$RDMA_BIND_IP" '{split($4,a,"/"); if (a[1]==ip) found=1} END {exit !found}'; then
        sudo ip addr add "$RDMA_BIND_IP/$RDMA_PREFIX" dev "$RDMA_NETDEV"
    fi
    sudo ip link set "$RDMA_NETDEV" up
    if ! rxe_link_exists; then
        sudo rdma link add "$RDMA_DEVICE" type rxe netdev "$RDMA_NETDEV"
        CREATED_RXE=1
        local i
        for i in $(seq 1 50); do
            rxe_link_exists && break
            sleep 0.1
        done
    fi
    rxe_link_exists || die "RXE link $RDMA_DEVICE did not come up"
    log "RXE ready: $RDMA_DEVICE on $RDMA_NETDEV ($RDMA_BIND_IP)"
}

stop_server() {
    if [[ -n "${SERVER_PID:-}" ]]; then
        kill "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
        SERVER_PID=""
        # RXE/CM can still hold large MRs after wait(2) returns. Immediate
        # re-listen + 4x16MiB reg_mr has hung the next client's first write
        # (ae blocked in valkeyRdmaWrite, no PHASE_END until timeout).
        sleep "${CELL_SETTLE_SEC:-3}"
    fi
}

cleanup() {
    stop_server
    if [[ "$RDMA_TRANSPORT" == "physical" || "$KEEP_RXE" == "1" ]]; then
        return 0
    fi
    if (( CREATED_RXE )) && rxe_link_exists; then
        sudo -n rdma link delete "$RDMA_DEVICE" 2>/dev/null || true
    fi
    if (( CREATED_NETDEV )) && ip link show dev "$RDMA_NETDEV" >/dev/null 2>&1; then
        sudo -n ip link delete "$RDMA_NETDEV" 2>/dev/null || true
    fi
}

start_server() {
    local log_name="${1:-valkey-server.log}"
    mkdir -p "$RESULTS_DIR/server-logs"
    SERVER_LOG="$RESULTS_DIR/server-logs/$log_name"
    VALKEY_RDMA_BENCH_STATS=1 ./src/valkey-server "$REPO_ROOT/valkey.conf" \
        --port 0 --protected-mode no --save "" --appendonly no --daemonize no \
        --rdma-bind "$RDMA_BIND_IP" --rdma-port "$RDMA_PORT" --rdma-rx-size "$RX_SIZE" \
        >"$SERVER_LOG" 2>&1 &
    SERVER_PID=$!
    local i
    for i in $(seq 1 50); do
        if timeout 5 ./src/valkey-cli --rdma -h "$RDMA_BIND_IP" -p "$RDMA_PORT" PING 2>/dev/null | grep -q PONG; then
            log "server up pid=$SERVER_PID rdma=$RDMA_BIND_IP:$RDMA_PORT rx-size=$RX_SIZE"
            return 0
        fi
        kill -0 "$SERVER_PID" 2>/dev/null || die "server died; see $SERVER_LOG"
        sleep 0.2
    done
    die "server did not become ready; see $SERVER_LOG"
}

append_phases_csv() {
    local err="$1" out="$2" rx="$3" rep="$4"
    awk -v commit="$GIT_COMMIT" -v run="$RUN_ID" -v rx="$rx" -v rep="$rep" \
        -v c="$CLIENTS" -v p="$PIPELINE" '
        /^PHASE_END / {
            split($0, a, " ")
            delete kv
            for (i = 2; i <= length(a); i++) {
                n = index(a[i], "=")
                if (n > 0) kv[substr(a[i], 1, n-1)] = substr(a[i], n+1)
            }
            printf "%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n",
                commit, run, rx, rep, kv["index"], kv["value_size"], kv["measured_sec"],
                c, p, kv["requests"], kv["rps"], kv["payload_gbps"],
                kv["avg_latency_us"], kv["p50_latency_us"], kv["p99_latency_us"],
                kv["reconnects"], kv["errors"],
                kv["tx_bytes"], kv["tx_wait_count"], kv["tx_wait_ns"], kv["rx_reannounce"],
                kv["reannounces_per_gib"], kv["stall_ratio"]
        }
    ' "$err" >> "$out"
}

check_run_ok() {
    local err="$1" slog="$2" rc="$3"
    if grep -qiE 'error writing|all clients disconnected|unexpected error|RDMA: FATAL|ASSERTION FAILED' "$err" ${slog:+"$slog"} 2>/dev/null; then
        die "errors in benchmark/server logs; see $err and $slog"
    fi
    if grep -q 'reconnects=[1-9]' "$err"; then
        die "phase reconnects reported; connections did not stay up (see $err)"
    fi
    if ! grep -q 'PHASE_WORKLOAD done' "$err"; then
        die "benchmark did not complete all phases (rc=$rc); see $err"
    fi
    local nbegin nend
    nbegin="$(grep -c 'PHASE_BEGIN ' "$err" || true)"
    nend="$(grep -c 'PHASE_END ' "$err" || true)"
    [[ "$nbegin" == "$nend" && "$nend" -gt 0 ]] || die "phase begin/end mismatch ($nbegin vs $nend)"
    [[ $rc -eq 0 ]] || die "valkey-benchmark exit $rc"
}

run_one_benchmark() {
    local csv="$1" err="$2"
    local timeout_sec=400
    (( SMOKE )) && timeout_sec=60
    log "running phase workload: $PHASES (rx=$RX_SIZE c=$CLIENTS P=$PIPELINE)"
    set +e
    VALKEY_RDMA_BENCH_STATS=1 timeout -k 10 "$timeout_sec" ./src/valkey-benchmark \
        --rdma \
        -h "$RDMA_SERVER_HOST" \
        -p "$RDMA_PORT" \
        -t set \
        -c "$CLIENTS" \
        -P "$PIPELINE" \
        --threads "$THREADS" \
        --csv \
        --data-size-phases "$PHASES" \
        >"$csv" 2>"$err"
    local rc=$?
    set -e
    return "$rc"
}

write_summary() {
    local all="$1" out="$2"
    awk -F, '
        NR == 1 { next }
        {
            k = $3 "," $5 "," $6
            n[k]++
            gbps[k] += $12
            stall[k] += $23
            reann[k] += $22
            rps[k] += $11
            p50[k] += $14
        }
        END {
            print "rx_size,phase_index,value_size,n,mean_rps,mean_payload_gbps,mean_p50_us,mean_reannounces_per_gib,mean_stall_ratio"
            for (k in n) {
                split(k, a, ",")
                printf "%s,%s,%s,%d,%.3f,%.6f,%.3f,%.6f,%.6f\n",
                    a[1], a[2], a[3], n[k],
                    rps[k] / n[k], gbps[k] / n[k], p50[k] / n[k],
                    reann[k] / n[k], stall[k] / n[k]
            }
        }
    ' "$all" | (read -r hdr; echo "$hdr"; sort -t, -k1,1n -k2,2n) > "$out"
}

main() {
    parse_args "$@"
    trap cleanup EXIT

    [[ -x ./src/valkey-benchmark && -x ./src/valkey-server ]] \
        || die "build first: make -j\$(nproc) BUILD_RDMA=yes USE_FAST_FLOAT=yes"

    local -a sizes=()
    if [[ -n "$RX_SIZES" ]]; then
        local tok
        IFS=',' read -ra tok <<< "$RX_SIZES"
        local t
        for t in "${tok[@]}"; do
            sizes+=("$(parse_bytes "$t")")
        done
    else
        sizes+=("$RX_SIZE")
    fi

    setup_memlock
    if [[ "$ROLE" == "all" ]]; then
        setup_transport
    fi

    local ts
    ts="$(date +%Y%m%d-%H%M%S)"
    RUN_ID="$ts"
    RESULTS_DIR="$REPO_ROOT/rdma-phase-results/$ts"
    mkdir -p "$RESULTS_DIR/raw"
    GIT_COMMIT="$(git rev-parse HEAD)"
    git rev-parse HEAD > "$RESULTS_DIR/git-commit.txt"
    printf '%s\n' "$0 $*" > "$RESULTS_DIR/cmdline.txt"
    cat > "$RESULTS_DIR/metadata.json" <<EOF
{"commit":"$GIT_COMMIT","transport":"$RDMA_TRANSPORT","bind":"$RDMA_BIND_IP","host":"$RDMA_SERVER_HOST","port":$RDMA_PORT,"rx_sizes":"$RX_SIZES","rx_size":$RX_SIZE,"repeats":$REPEATS,"clients":$CLIENTS,"pipeline":$PIPELINE,"threads":$THREADS,"phases":"$PHASES","role":"$ROLE"}
EOF

    printf '%s\n' "$PHASES_CSV_HEADER" > "$RESULTS_DIR/phases.csv"

    local rx rep cell=0 total=$(( ${#sizes[@]} * REPEATS ))
    for rx in "${sizes[@]}"; do
        RX_SIZE="$rx"
        for rep in $(seq 1 "$REPEATS"); do
            cell=$((cell + 1))
            log "cell $cell/$total rx=$RX_SIZE repeat=$rep/$REPEATS"
            if [[ "$ROLE" == "all" ]]; then
                stop_server
                start_server "server-rx${RX_SIZE}-r${rep}.log"
            fi
            local csv="$RESULTS_DIR/raw/rx${RX_SIZE}-r${rep}.csv"
            local err="$RESULTS_DIR/raw/rx${RX_SIZE}-r${rep}.err"
            local rc=0
            run_one_benchmark "$csv" "$err" || rc=$?
            check_run_ok "$err" "${SERVER_LOG:-}" "$rc"
            append_phases_csv "$err" "$RESULTS_DIR/phases.csv" "$RX_SIZE" "$rep"
        done
    done

    if [[ "$ROLE" == "all" ]]; then
        stop_server
    fi

    write_summary "$RESULTS_DIR/phases.csv" "$RESULTS_DIR/summary.csv"
    cp "$RESULTS_DIR/phases.csv" "$RESULTS_DIR/benchmark.stdout.csv" 2>/dev/null || true

    log "results: $RESULTS_DIR"
    log "phases.csv rows: $(( $(wc -l < "$RESULTS_DIR/phases.csv") - 1 ))"
    column -s, -t "$RESULTS_DIR/summary.csv" 2>/dev/null || cat "$RESULTS_DIR/summary.csv"
    log "ALL RUNS PASSED (persistent connections within each cell; new server per rx-size/repeat)"
}

main "$@"
