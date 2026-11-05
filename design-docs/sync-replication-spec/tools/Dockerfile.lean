# Container for tools/lean.sh: stock Ubuntu (glibc >= 2.27 for Lean 4.35) plus git and
# curl, which Lake needs to fetch Mathlib and `lake exe cache get` needs to download its
# prebuilt oleans. The Lean toolchain itself is mounted from tools/elan, not installed.
#   docker build -t valkey-lean:24.04 -f tools/Dockerfile.lean tools
FROM ubuntu:24.04
RUN apt-get update && apt-get install -y --no-install-recommends git curl ca-certificates
