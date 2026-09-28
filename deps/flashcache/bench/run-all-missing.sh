#!/bin/bash
# Run all missing FlashCache benchmarks (T2, T3-size-sweep, T4, T5)
# Host: 108.129.72.72 (r7gd.4xlarge, eu-west-1c)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
HOST="108.129.72.72"
BENCH="$SCRIPT_DIR/fc-bench.sh"

echo "================================================================"
echo "FlashCache Benchmark Suite — Full Re-run (with data capture)"
echo "Host: ${HOST}"
echo "Started: $(date -u '+%Y-%m-%d %H:%M UTC')"
echo "================================================================"
echo ""

# FC-T2: Peak Read (prefer_old, 32GB DB)
echo ">>> [1/8] FC-T2: Peak Read (512B, prefer_old, 32GB)"
$BENCH configs/peak-read-32g.env --host $HOST --tag FC-T2 --duration 300

# FC-T3: Size Sweep — individual sizes
echo ""
echo ">>> [2/8] FC-T3a: Size Sweep — 512B"
$BENCH configs/size-sweep-512b.env --host $HOST --tag FC-T3-512b --duration 180

echo ""
echo ">>> [3/8] FC-T3b: Size Sweep — 1KB"
$BENCH configs/size-sweep-1kb.env --host $HOST --tag FC-T3-1kb --duration 180

echo ""
echo ">>> [4/8] FC-T3c: Size Sweep — 4KB"
$BENCH configs/size-sweep-4kb.env --host $HOST --tag FC-T3-4kb --duration 180

echo ""
echo ">>> [5/8] FC-T3d: Size Sweep — 100KB"
$BENCH configs/size-sweep-100kb.env --host $HOST --tag FC-T3-100kb --duration 180

echo ""
echo ">>> [6/8] FC-T3e: Size Sweep — 1MB"
$BENCH configs/size-sweep-1mb.env --host $HOST --tag FC-T3-1mb --duration 180

# FC-T4: GC Stress (2GB DB, 1:1 R/W, prefer_new)
echo ""
echo ">>> [7/8] FC-T4: GC Stress (512B, 2GB, 1:1 R/W, prefer_new)"
$BENCH configs/gc-stress.env --host $HOST --tag FC-T4 --duration 300

# FC-T5: Write-heavy (8GB, 1:1 R/W, prefer_old)
echo ""
echo ">>> [8/8] FC-T5: Write-heavy (512B, 8GB, 1:1 R/W, prefer_old)"
$BENCH configs/write-heavy.env --host $HOST --tag FC-T5 --duration 300

echo ""
echo "================================================================"
echo "ALL RUNS COMPLETE"
echo "Finished: $(date -u '+%Y-%m-%d %H:%M UTC')"
echo ""
echo "Results at: ${SCRIPT_DIR}/results/"
ls -d ${SCRIPT_DIR}/results/FC-T*/ ${SCRIPT_DIR}/results/FC-T3-*/ 2>/dev/null
echo "================================================================"
