#!/usr/bin/env bash
# Runs Lean 4.35.0 (needs glibc >= 2.27; the host has 2.26) in an Ubuntu container.
# The toolchain was downloaded by elan into tools/elan and is mounted read-only.
# The image adds git and curl for Mathlib (tools/Dockerfile.lean); build it once with
#   docker build -t valkey-lean:24.04 -f tools/Dockerfile.lean tools
# Usage: tools/lean.sh <lean|lake> <args>   (run from the Lean project directory)
T=$(cd "$(dirname "$0")" && pwd)
LT="$T/elan/toolchains/leanprover--lean4---v4.35.0"
IMG=${LEAN_IMAGE:-valkey-lean:24.04}
cmd=$1; shift
exec docker run --rm -u "$(id -u):$(id -g)" -e HOME=/tmp -e PATH="/opt/lean/bin:/usr/bin:/bin" \
  -v "$LT:/opt/lean:ro" -v "$PWD:/work" -w /work "$IMG" "/opt/lean/bin/$cmd" "$@"
