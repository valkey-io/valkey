#!/bin/bash
# FlashCache Standalone Benchmark Runner
# Usage: ./fc-bench.sh <config.env> [--tag TAG] [--host HOST]
#
# Runs FlashCacheApp on a remote host, captures stats, generates report.
# Requires: FlashCacheApp binary built and available locally or on host.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
RESULTS_BASE="${SCRIPT_DIR}/results"

# --- Defaults ---
TAG=""
HOST=""
KEY="${HOME}/.ssh/id_rsa"
FC_APP_PATH=""
FIO_BASELINE=0
WARMUP_DISCARD_SECS=30
RUN_DURATION_SECS=300

usage() {
    echo "Usage: $0 <config.env> [options]"
    echo ""
    echo "Options:"
    echo "  --tag TAG        Name for this run (default: config filename + timestamp)"
    echo "  --host HOST      Remote host IP (required)"
    echo "  --key PATH       SSH key (default: ~/.ssh/id_rsa)"
    echo "  --fio            Run fio baseline before FC test"
    echo "  --warmup SECS    Seconds to discard at start (default: 30)"
    echo "  --duration SECS  Measurement duration in seconds (default: 300)"
    echo ""
    echo "Config env vars (set in config.env):"
    echo "  DB_FILE          Path to FC database file on host (e.g. /mnt/nvme/fc.db)"
    echo "  DB_SIZE_GIB      Database size in GiB"
    echo "  MAX_ITEMS        Maximum items to store"
    echo "  VALUE_SIZES      Comma-separated mean_size:weight (e.g. 512:1,4096:1)"
    echo "  VALUE_DIST       normal_dist or constant_dist"
    echo "  NUM_LARGE_ITEMS  Number of 100MB items (usually 0)"
    echo "  READ_PREF        prefer_old_item, prefer_new_item, prefer_middle_item"
    echo "  TPS_LIMIT        Target TPS (0 = unlimited)"
    echo "  STAT_INTERVAL    Ops between stat emissions (default: 2000000)"
    echo "  READ_WRITE_RATIO Read:write ratio (e.g. 4 = 4 reads per write)"
    echo "  SNAPSHOT_VER     1 or 2 (default: 2)"
    echo "  SNAPSHOT_TYPE    bgsave or forkless_save (default: bgsave)"
    exit 1
}

# --- Parse args ---
CONFIG=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --tag) TAG="$2"; shift 2 ;;
        --host) HOST="$2"; shift 2 ;;
        --key) KEY="$2"; shift 2 ;;
        --fio) FIO_BASELINE=1; shift ;;
        --warmup) WARMUP_DISCARD_SECS="$2"; shift 2 ;;
        --duration) RUN_DURATION_SECS="$2"; shift 2 ;;
        --help|-h) usage ;;
        *)
            if [[ -z "$CONFIG" ]]; then
                CONFIG="$1"
            else
                echo "Unknown arg: $1"; usage
            fi
            shift ;;
    esac
done

if [[ -z "$CONFIG" ]] || [[ -z "$HOST" ]]; then
    echo "ERROR: config file and --host are required"
    usage
fi

# --- Load config ---
source "$CONFIG"

# --- Generate tag ---
if [[ -z "$TAG" ]]; then
    TAG="$(basename "$CONFIG" .env)-$(date +%Y%m%d-%H%M%S)"
fi

RESULTS_DIR="${RESULTS_BASE}/${TAG}"
mkdir -p "$RESULTS_DIR"

# Copy config for reproducibility
cp "$CONFIG" "${RESULTS_DIR}/config.env"

echo "============================================="
echo "FlashCache Benchmark: ${TAG}"
echo "Host: ${HOST}"
echo "DB: ${DB_FILE} (${DB_SIZE_GIB} GiB)"
echo "Items: ${MAX_ITEMS}, Values: ${VALUE_SIZES}"
echo "R:W ratio: ${READ_WRITE_RATIO:-4}"
echo "============================================="

SSH="ssh -o StrictHostKeyChecking=no -o ConnectTimeout=10 -i ${KEY} ec2-user@${HOST}"

# --- Step 0: fio baseline (optional) ---
if [[ "$FIO_BASELINE" -eq 1 ]]; then
    echo ""
    echo ">>> Running fio baseline..."

    $SSH "
        cd /mnt/nvme &&
        # Sequential write
        fio --name=seq_write --rw=write --ioengine=libaio --iodepth=32 --bs=4k \
            --direct=1 --size=4G --numjobs=1 --runtime=60 --time_based \
            --filename=/mnt/nvme/fio_test --output-format=json > /tmp/fio_seq_write.json 2>&1 &&
        # Sequential read
        fio --name=seq_read --rw=read --ioengine=libaio --iodepth=32 --bs=4k \
            --direct=1 --size=4G --numjobs=1 --runtime=60 --time_based \
            --filename=/mnt/nvme/fio_test --output-format=json > /tmp/fio_seq_read.json 2>&1 &&
        # Random write (simulates FC log-append pattern)
        fio --name=rand_write --rw=randwrite --ioengine=libaio --iodepth=32 --bs=4k \
            --direct=1 --size=4G --numjobs=1 --runtime=60 --time_based \
            --filename=/mnt/nvme/fio_test --output-format=json > /tmp/fio_rand_write.json 2>&1 &&
        # Random read (simulates FC random reads)
        fio --name=rand_read --rw=randread --ioengine=libaio --iodepth=32 --bs=4k \
            --direct=1 --size=4G --numjobs=1 --runtime=60 --time_based \
            --filename=/mnt/nvme/fio_test --output-format=json > /tmp/fio_rand_read.json 2>&1 &&
        # Mixed 70/30 read/write (simulates FC steady state)
        fio --name=mixed_rw --rw=randrw --rwmixread=70 --ioengine=libaio --iodepth=32 --bs=4k \
            --direct=1 --size=4G --numjobs=1 --runtime=60 --time_based \
            --filename=/mnt/nvme/fio_test --output-format=json > /tmp/fio_mixed_rw.json 2>&1 &&
        rm -f /mnt/nvme/fio_test &&
        echo 'FIO_DONE'
    " 2>&1 | tee "${RESULTS_DIR}/fio_run.log" | tail -1

    # Pull results
    scp -o StrictHostKeyChecking=no -i "$KEY" "ec2-user@${HOST}:/tmp/fio_*.json" "${RESULTS_DIR}/" 2>/dev/null

    # Parse fio JSON into summary
    echo ">>> Parsing fio results..."
    python3 - "${RESULTS_DIR}" <<'PYEOF'
import json, sys, os
results_dir = sys.argv[1]
summary = []
for fname in sorted(os.listdir(results_dir)):
    if not fname.startswith("fio_") or not fname.endswith(".json"):
        continue
    with open(os.path.join(results_dir, fname)) as f:
        data = json.load(f)
    for job in data.get("jobs", []):
        name = job["jobname"]
        r = job.get("read", {})
        w = job.get("write", {})
        line = {
            "test": name,
            "read_iops": r.get("iops", 0),
            "read_bw_MBps": r.get("bw", 0) / 1024,
            "read_lat_us_avg": r.get("lat_ns", {}).get("mean", 0) / 1000,
            "read_lat_us_p99": r.get("clat_ns", {}).get("percentile", {}).get("99.000000", 0) / 1000,
            "write_iops": w.get("iops", 0),
            "write_bw_MBps": w.get("bw", 0) / 1024,
            "write_lat_us_avg": w.get("lat_ns", {}).get("mean", 0) / 1000,
            "write_lat_us_p99": w.get("clat_ns", {}).get("percentile", {}).get("99.000000", 0) / 1000,
        }
        summary.append(line)

# Write CSV
with open(os.path.join(results_dir, "fio_summary.csv"), "w") as f:
    if summary:
        f.write(",".join(summary[0].keys()) + "\n")
        for row in summary:
            f.write(",".join(str(v) for v in row.values()) + "\n")

# Print table
print(f"\n{'Test':<15} {'R_IOPS':>10} {'R_BW_MB/s':>10} {'R_lat_us':>10} {'W_IOPS':>10} {'W_BW_MB/s':>10} {'W_lat_us':>10}")
print("-" * 85)
for r in summary:
    print(f"{r['test']:<15} {r['read_iops']:>10.0f} {r['read_bw_MBps']:>10.1f} {r['read_lat_us_avg']:>10.1f} {r['write_iops']:>10.0f} {r['write_bw_MBps']:>10.1f} {r['write_lat_us_avg']:>10.1f}")
PYEOF
    echo ""
fi

# --- Step 1: Ensure FlashCacheApp is on the host ---
echo ">>> Checking FlashCacheApp on host..."
FC_APP_REMOTE=$($SSH "
    if [ -f /mnt/nvme/FlashCacheApp ]; then echo /mnt/nvme/FlashCacheApp;
    elif [ -f /mnt/nvme/fc_bench_build/FlashCacheApp ]; then echo /mnt/nvme/fc_bench_build/FlashCacheApp;
    elif [ -f /mnt/nvme/fc_full_build/FlashCacheApp ]; then echo /mnt/nvme/fc_full_build/FlashCacheApp;
    else echo NOTFOUND; fi
" 2>/dev/null)
if [[ "$FC_APP_REMOTE" == "NOTFOUND" ]]; then
    echo ">>> FlashCacheApp not found on host. Upload it first with:"
    echo "    scp -i $KEY FlashCacheApp ec2-user@${HOST}:/mnt/nvme/"
    exit 1
fi
echo ">>> Found: ${FC_APP_REMOTE}"

# --- Step 2: Pre-allocate DB file ---
echo ">>> Pre-allocating ${DB_FILE} (${DB_SIZE_GIB} GiB)..."
$SSH "fallocate -l ${DB_SIZE_GIB}G ${DB_FILE} 2>/dev/null || truncate -s ${DB_SIZE_GIB}G ${DB_FILE}" 2>/dev/null

# --- Step 3: Run FlashCacheApp + iostat ---
echo ">>> Starting iostat capture (5s interval)..."
$SSH "nohup iostat -x 5 /dev/nvme1n1 > /tmp/fc_bench_iostat.log 2>&1 & echo \$!" 2>/dev/null > "${RESULTS_DIR}/iostat_pid.txt"
IOSTAT_PID=$(cat "${RESULTS_DIR}/iostat_pid.txt" | tr -d '[:space:]')
echo ">>> iostat PID: ${IOSTAT_PID}"

echo ">>> Starting FlashCacheApp (foreground, timeout ${RUN_DURATION_SECS}s + fill)..."
# Run FC in foreground via SSH — output streams directly to file.
# Filter WARNING/NOTICE lines to keep raw.log manageable (they can be 50M+ lines).
# Uses timeout to cap total runtime (fill + measurement).
TOTAL_TIMEOUT=$((RUN_DURATION_SECS + 600))
$SSH "
    cd /mnt/nvme &&
    export LD_LIBRARY_PATH=\${LD_LIBRARY_PATH:-}:/usr/local/lib &&
    timeout ${TOTAL_TIMEOUT} ${FC_APP_REMOTE} \
        ${DB_FILE} \
        ${DB_SIZE_GIB} \
        ${MAX_ITEMS} \
        ${VALUE_SIZES} \
        ${VALUE_DIST:-constant_dist} \
        ${NUM_LARGE_ITEMS:-0} \
        ${READ_PREF:-prefer_old_item} \
        ${TPS_LIMIT:-0} \
        ${STAT_INTERVAL:-2000000} \
        ${READ_WRITE_RATIO:-4} \
        ${SNAPSHOT_VER:-2} \
        ${SNAPSHOT_TYPE:-bgsave} \
        2>&1 | grep -v '^\[WARNING\]\|^dbid:\|^$' || true
" > "${RESULTS_DIR}/raw.log" 2>&1

echo ">>> FlashCacheApp finished. Lines captured: $(wc -l < "${RESULTS_DIR}/raw.log")"

FC_PID="foreground"
echo ">>> Run complete."

# --- Step 4: Pull iostat log ---
echo ">>> Stopping iostat..."
$SSH "kill ${IOSTAT_PID} 2>/dev/null" 2>/dev/null
sleep 2

echo ">>> Pulling iostat..."
scp -o StrictHostKeyChecking=no -i "$KEY" "ec2-user@${HOST}:/tmp/fc_bench_iostat.log" "${RESULTS_DIR}/iostat.log" 2>/dev/null

# --- Step 5: Parse into CSV + summary ---
echo ">>> Generating report..."
python3 - "${RESULTS_DIR}" "${WARMUP_DISCARD_SECS}" <<'PYEOF'
import sys, os, re, statistics

results_dir = sys.argv[1]
warmup_secs = int(sys.argv[2])

raw_path = os.path.join(results_dir, "raw.log")
if not os.path.exists(raw_path):
    print("ERROR: raw.log not found")
    sys.exit(1)

lines = open(raw_path).readlines()

# Parse the pipe-delimited stat lines (skip header and non-data lines)
header = None
data_lines = []
for line in lines:
    line = line.strip()
    if not line or '|' not in line:
        continue
    parts = [p.strip() for p in line.split('|')]
    if len(parts) < 14:
        continue
    # Check if first field is numeric (data) vs text (header)
    try:
        float(parts[0])
        data_lines.append(parts)
    except ValueError:
        if header is None:
            header = parts

if not header:
    header = ["num_items", "read_tps", "write_tps", "disk_read_MBps", "disk_write_MBps",
              "gc_read_MBps", "gc_write_MBps", "active_db_GiB", "allocated_db_GiB",
              "gc_rate_MBps", "write_amp", "num_read_throttled", "num_write_throttled",
              "memory_GiB"]

# Write all data as CSV
csv_path = os.path.join(results_dir, "fc_timeseries.csv")
with open(csv_path, "w") as f:
    f.write(",".join(header[:14]) + "\n")
    for row in data_lines:
        f.write(",".join(row[:14]) + "\n")

# Discard warmup (first N lines based on stat emission rate)
# Assume ~1 line per second based on stress_test_app behavior
discard_lines = max(1, warmup_secs)
steady_state = data_lines[discard_lines:]

if not steady_state:
    print(f"WARNING: Only {len(data_lines)} data lines, nothing after warmup discard")
    steady_state = data_lines

# Compute summary stats
def col_stats(idx):
    vals = []
    for row in steady_state:
        try:
            vals.append(float(row[idx]))
        except (IndexError, ValueError):
            pass
    if not vals:
        return {"avg": 0, "min": 0, "max": 0, "p50": 0, "p99": 0}
    vals.sort()
    n = len(vals)
    return {
        "avg": statistics.mean(vals),
        "min": min(vals),
        "max": max(vals),
        "p50": vals[n // 2],
        "p99": vals[int(n * 0.99)] if n > 1 else vals[-1],
    }

summary = {}
for i, name in enumerate(header[:14]):
    summary[name] = col_stats(i)

# Write summary JSON
import json
with open(os.path.join(results_dir, "summary.json"), "w") as f:
    json.dump(summary, f, indent=2)

# Print human-readable report
print("\n" + "=" * 70)
print(f"FLASHCACHE BENCHMARK REPORT")
print(f"Data points: {len(data_lines)} total, {len(steady_state)} after warmup")
print("=" * 70)
print(f"\n{'Metric':<25} {'Avg':>12} {'Min':>12} {'Max':>12} {'P50':>12}")
print("-" * 73)
key_metrics = ["read_tps", "write_tps", "disk_read_MBps", "disk_write_MBps",
               "gc_read_MBps", "gc_write_MBps", "write_amp", "memory_GiB",
               "active_db_GiB", "num_items"]
for m in key_metrics:
    if m in summary:
        s = summary[m]
        print(f"{m:<25} {s['avg']:>12.1f} {s['min']:>12.1f} {s['max']:>12.1f} {s['p50']:>12.1f}")

print("\n" + "=" * 70)
print(f"Results saved to: {results_dir}/")
print(f"  - raw.log          (raw FlashCacheApp output)")
print(f"  - fc_timeseries.csv (parsed time-series)")
print(f"  - summary.json     (aggregated stats)")
if os.path.exists(os.path.join(results_dir, "fio_summary.csv")):
    print(f"  - fio_summary.csv  (fio baseline)")

# --- Parse iostat log into CSV ---
iostat_path = os.path.join(results_dir, "iostat.log")
if os.path.exists(iostat_path):
    iostat_data = []
    cpu_data = []
    with open(iostat_path) as f:
        cpu_line_next = False
        dev_line_next = False
        for line in f:
            line = line.strip()
            if line.startswith("avg-cpu:"):
                cpu_line_next = True
                continue
            if cpu_line_next and line and not line.startswith("avg"):
                parts = line.split()
                if len(parts) >= 6:
                    cpu_data.append({
                        "user": float(parts[0]),
                        "nice": float(parts[1]),
                        "system": float(parts[2]),
                        "iowait": float(parts[3]),
                        "steal": float(parts[4]),
                        "idle": float(parts[5]),
                    })
                cpu_line_next = False
                continue
            if line.startswith("Device") or line.startswith("device"):
                dev_line_next = True
                continue
            if dev_line_next and line and line.startswith("nvme"):
                parts = line.split()
                if len(parts) >= 20:
                    iostat_data.append({
                        "r_iops": float(parts[1]),
                        "r_kBps": float(parts[2]),
                        "rrqm_ps": float(parts[3]),
                        "r_await": float(parts[5]),
                        "rareq_sz": float(parts[6]),
                        "w_iops": float(parts[7]),
                        "w_kBps": float(parts[8]),
                        "wrqm_ps": float(parts[9]),
                        "w_await": float(parts[11]),
                        "wareq_sz": float(parts[12]),
                        "aqu_sz": float(parts[20]),
                        "util_pct": float(parts[21]),
                    })
                dev_line_next = False
                continue
            if dev_line_next:
                dev_line_next = False

    # Write iostat CSV
    if iostat_data:
        iostat_csv = os.path.join(results_dir, "iostat_timeseries.csv")
        with open(iostat_csv, "w") as f:
            keys = list(iostat_data[0].keys())
            # Add CPU cols if available
            if cpu_data and len(cpu_data) == len(iostat_data):
                keys += ["cpu_user", "cpu_sys", "cpu_iowait", "cpu_idle"]
                for i, row in enumerate(iostat_data):
                    row["cpu_user"] = cpu_data[i]["user"]
                    row["cpu_sys"] = cpu_data[i]["system"]
                    row["cpu_iowait"] = cpu_data[i]["iowait"]
                    row["cpu_idle"] = cpu_data[i]["idle"]
            f.write(",".join(keys) + "\n")
            for row in iostat_data:
                f.write(",".join(str(row.get(k, 0)) for k in keys) + "\n")
        print(f"  - iostat_timeseries.csv ({len(iostat_data)} samples)")
    else:
        print(f"  - iostat.log (raw, not parsed)")

print("=" * 70)
PYEOF

echo ""
echo ">>> Done! Results at: ${RESULTS_DIR}/"

# --- Cleanup DB file ---
echo ">>> Cleaning up DB file on host..."
$SSH "rm -f ${DB_FILE}" 2>/dev/null
