#!/usr/bin/env bash
# Runs Lean 4.35.0 (needs glibc >= 2.27; the host has 2.26) in a stock Ubuntu container.
# The toolchain was downloaded by elan into tools/elan; nothing is installed in the image.
# Usage: tools/lean.sh <lean|lake> <args>   (run from the Lean project directory)
T=$(cd "$(dirname "$0")" && pwd)
LT="$T/elan/toolchains/leanprover--lean4---v4.35.0"
cmd=$1; shift
exec docker run --rm -u "$(id -u):$(id -g)" -e HOME=/tmp -v "$LT:/opt/lean:ro" \
  -v "$PWD:/work" -w /work ubuntu:24.04 "/opt/lean/bin/$cmd" "$@"
