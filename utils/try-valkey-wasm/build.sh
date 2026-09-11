#!/usr/bin/env bash
# Build valkey-server as an LP64 WebAssembly module with Emscripten.
#
# Two pointer modes, selected with WASM_MODE:
#   lowered (default): wasm64 for clang/lld, lowered to wasm32 by Binaryen.
#                      Runs on every wasm32 engine (incl. Safari) while the
#                      C code is compiled LP64 (sizeof(long)==8).
#   native:            true wasm64 (memory64). Chrome 133+/Firefox 134+.
#
# Usage: source emsdk env, then  utils/try-valkey-wasm/build.sh [clean]
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
MODE="${WASM_MODE:-lowered}"
JOBS="${JOBS:-$(nproc)}"

case "$MODE" in
  lowered) ARCH_FLAGS="-sMEMORY64=2" ;;
  native)  ARCH_FLAGS="-sMEMORY64=1" ;;
  *) echo "unknown WASM_MODE=$MODE" >&2; exit 2 ;;
esac

if [[ "${1:-}" == "clean" ]]; then
  (cd "$ROOT" && emmake make distclean >/dev/null 2>&1 || true)
fi

# Common compile flags. -D__TRY_VALKEY_WASM__ gates spike-only code.
CFLAGS_COMMON="$ARCH_FLAGS -Wno-gnu-zero-variadic-macro-arguments -Wno-unused-command-line-argument"

# Link flags: keep the runtime alive after main() returns, export the driver
# API, allow the heap to grow, and export the module as an ES6 factory.
LDFLAGS_COMMON="$ARCH_FLAGS \
  -sALLOW_MEMORY_GROWTH=1 \
  -sINITIAL_MEMORY=64MB \
  -sEXIT_RUNTIME=0 \
  -sMODULARIZE=1 -sEXPORT_ES6=1 -sEXPORT_NAME=createValkey \
  -sENVIRONMENT=web,worker,node \
  -sEXPORTED_RUNTIME_METHODS=ccall,cwrap,HEAPU8,stringToNewUTF8,UTF8ToString,getValue,setValue,callMain \
  -sINVOKE_RUN=0 \
  -sEXPORTED_FUNCTIONS=_main,_malloc,_free \
  -sALLOW_UNIMPLEMENTED_SYSCALLS=1"

export CC=emcc CXX=em++ AR=emar RANLIB=emranlib LD=emcc

cd "$ROOT"
emmake make -j"$JOBS" -C src valkey-server.mjs DEBUG="${DEBUG-}" \
  MALLOC=libc BUILD_TLS=no BUILD_RDMA=no USE_SYSTEMD=no \
  OPTIMIZATION="${OPTIMIZATION:--O2}" \
  CFLAGS="$CFLAGS_COMMON ${CFLAGS:-}" CXXFLAGS="$CFLAGS_COMMON ${CXXFLAGS:-}" \
  LDFLAGS="$LDFLAGS_COMMON ${LDFLAGS:-}" \
  FINAL_LIBS="-lm" \
  PROG_SUFFIX=".mjs" \
  V="${V:-}"
