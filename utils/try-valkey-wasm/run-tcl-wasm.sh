#!/usr/bin/env bash
# Run the stock Tcl unit-test suite against the wasm server.
#
# Starts tcp-bridge.mjs (the in-process server exposed on a TCP port), runs
# ./runtest in external mode over every unit/ test file (cluster and module
# units excluded: no cluster bus, no dlopen), then stops the bridge.
#
#   utils/try-valkey-wasm/run-tcl-wasm.sh            # needs src/valkey-server.{mjs,wasm} and tclsh 8.6
#   PORT=7400 utils/try-valkey-wasm/run-tcl-wasm.sh
#
# Skips (all documented in README.md): tests tagged needs:repl / needs:save
# (fork), needs:debug, needs:reset, needs:other-server, needs:latency, slow and
# large-memory; plus the skip file: busy-script KILL tests (a busy Lua script
# owns the single thread), replication-stream tests (SYNC needs fork), IPv6
# bind, CLIENT LIST ip filters (the address is mem:N).
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
PORT="${PORT:-7379}"
TAGS="-needs:repl -needs:save -needs:debug -needs:reset -needs:other-server -needs:latency -slow -large-memory"

for f in "$ROOT/src/valkey-server.mjs" "$ROOT/src/valkey-server.wasm"; do
  [[ -f "$f" ]] || { echo "missing $f -- run build.sh first" >&2; exit 2; }
done

cd "$ROOT"
mkdir -p "$HERE/out"

# tests/support/set_executable_path.tcl refuses to start unless src/valkey-server exists and is executable,
# even in external mode where it is never launched. The server under test is the wasm one behind the bridge;
# provide a stub only if no native binary is present, and say so if anyone runs it.
if [[ ! -x src/valkey-server ]]; then
  printf '#!/bin/sh\necho "stub: the server under test is the wasm build behind tcp-bridge.mjs" >&2; exit 1\n' > src/valkey-server
  chmod +x src/valkey-server
  STUBBED=1
fi
node "$HERE/tcp-bridge.mjs" --port "$PORT" > "$HERE/out/bridge-ci.log" 2>&1 &
BRIDGE=$!
trap 'kill $BRIDGE 2>/dev/null' EXIT
for i in $(seq 1 50); do
  grep -q "tcp-bridge: wasm valkey-server" "$HERE/out/bridge-ci.log" 2>/dev/null && break
  sleep 0.2
done
grep -q "tcp-bridge: wasm valkey-server" "$HERE/out/bridge-ci.log" || { echo "bridge did not start:"; cat "$HERE/out/bridge-ci.log"; exit 2; }

UNITS=()
for t in tests/unit/*.tcl tests/unit/type/*.tcl; do
  UNITS+=(--single "${t#tests/}")
done
UNITS=("${UNITS[@]/%.tcl/}")

./runtest --host 127.0.0.1 --port "$PORT" --singledb --clients 1 --timeout 120 \
  --tags "$TAGS" --skipfile "$HERE/skip-busy-script-tests.txt" "${UNITS[@]}"
RC=$?

if ! kill -0 $BRIDGE 2>/dev/null; then
  echo "!!! the wasm server crashed during the run:"; tail -30 "$HERE/out/bridge-ci.log"; RC=1
fi
[[ "${STUBBED:-}" == 1 ]] && rm -f src/valkey-server
exit $RC
