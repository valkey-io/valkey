# AGENTS.md

## Scope

These instructions apply to the entire repository unless a subdirectory contains a deeper `AGENTS.md` that overrides them.

## Repo overview

Valkey is a high-performance open-source key-value store server.

- Main implementation: `src/`
- Unit tests: `src/unit/` (GoogleTest, C++)
- Integration tests: `tests/` (Tcl)
- Top-level `Makefile` forwards most targets into `src/Makefile`
- Third-party dependencies in `deps/`: jemalloc, libvalkey, lua, linenoise, hdr_histogram, fast_float
- Command definitions: `src/commands/*.json` (427+ entries in JSON format)

## Source code layout

| Module | Key files | Description |
|---|---|---|
| Server core | server.c, server.h | Main loop, initialization, global state |
| Networking | networking.c, anet.c, socket.c, connection.c, io_threads.c | TCP, connection management, multi-threaded I/O |
| Event loop | ae.c, ae_epoll.c, ae_kqueue.c | Event-driven engine (epoll/kqueue backends) |
| Data structures | dict.c, hashtable.c, sds.c, quicklist.c, listpack.c, intset.c, rax.c | Internal data structures |
| Data type commands | t_string.c, t_hash.c, t_list.c, t_set.c, t_zset.c, t_stream.c | Per-type command implementations |
| Persistence | rdb.c, aof.c | RDB snapshots and AOF journaling |
| Replication | replication.c | Primary/replica replication |
| Cluster | cluster.c, cluster_legacy.c, cluster_migrateslots.c | Cluster mode |
| Sentinel | sentinel.c | High-availability sentinel |
| Scripting | eval.c, script.c, scripting_engine.c | Lua scripting |
| Modules | module.c, module.h | Module system |
| Memory | zmalloc.c, defrag.c, lazyfree.c, allocator_defrag.c | Memory management |
| Security | acl.c, tls.c | ACL and TLS |

## Build

**Default build:**
```bash
make
make -j$(nproc)          # parallel build
```

**Clean rebuild** (required when build settings or bundled deps change):
```bash
make distclean && make
```

**Generated binaries:** `valkey-server`, `valkey-cli`, `valkey-benchmark`, `valkey-sentinel`, `valkey-check-rdb`, `valkey-check-aof`

**Build options:**

| Option | Values | Description |
|---|---|---|
| `BUILD_TLS` | `yes` / `module` | TLS support (builtin or loadable module) |
| `BUILD_RDMA` | `yes` / `module` | RDMA transport support |
| `MALLOC` | `jemalloc` / `libc` / `tcmalloc` | Memory allocator (jemalloc default on Linux) |
| `SANITIZER` | `address` / `undefined` / `thread` | Enable sanitizers for debugging |
| `BUILD_LUA` | `yes` / `no` | Lua scripting engine |
| `USE_SYSTEMD` | `yes` / `no` / `auto` | systemd notify integration |

**Common combinations:**
```bash
make -j4 BUILD_TLS=yes             # With TLS support
make -j4 SANITIZER=address         # With AddressSanitizer
make noopt                          # Debug build without optimization
make valgrind                       # Valgrind-compatible build
```

**CMake build (alternative):**
```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release -DBUILD_TLS=yes
make -j$(nproc)
```

## Testing

### Unit tests

Located in `src/unit/`, written in C++ using GoogleTest (gtest/gmock). Cover data structures and low-level logic.

```bash
# Build and run all unit tests
make -C src test-unit

# Run a single test
./src/unit/valkey-unit-gtests --gtest_filter='TestSuite.TestName'
```

### Integration tests

Located in `tests/`, written in Tcl. Cover command behavior and end-to-end scenarios.

```bash
# Full integration suite
make test

# Single test file
./runtest --single tests/unit/expire.tcl
```

**Specialized test suites:**

| Script | Scope |
|---|---|
| `./runtest` | Main integration tests |
| `./runtest-cluster` | Cluster mode tests |
| `./runtest-sentinel` | Sentinel tests |
| `./runtest-moduleapi` | Module API tests |
| `./runtest-rdma` | RDMA tests |

**Useful `runtest` flags:** `--verbose`, `--tags -slow` (skip slow tests), `--dump-logs` (print logs on failure)

### Choosing the right test scope

| Change type | Test location |
|---|---|
| Data structure / low-level logic | `src/unit/` (GoogleTest) |
| Command behavior / end-to-end | `tests/unit/` or `tests/integration/` (Tcl) |
| Cluster-related | `tests/unit/cluster/` (new framework) preferred over `tests/cluster/` |

Always run the smallest relevant test scope first before running broader suites.

## Code style

- Follow conventions in `DEVELOPMENT_GUIDE.md`.
- CI enforces `clang-format-18` on `*.c`, `*.h`, `*.cpp`, `*.hpp`.
- After modifying C/C++ files, run: `clang-format-18 -i <modified files>`

**Key style rules:**

- Indentation: 4 spaces, no tabs
- Comments: prefer `/* */` style; use `//` only for single-line inline comments
- Naming: `snake_case` for variables, `camelCase` or `namespace_camelCase` for functions, `UPPER_CASE` for macros
- Comments should explain non-obvious behavior and rationale, not restate the code
- Functions should have documentation comments

**License header** — all new files must include:
```c
/*
 * Copyright (c) Valkey Contributors
 * All rights reserved.
 * SPDX-License-Identifier: BSD-3-Clause
 */
```

## Adding or modifying commands

1. Edit or create the command definition in `src/commands/<command>.json`.
2. Regenerate `commands.def` and `fmtargs.h`:
   ```bash
   python utils/generate-command-code.py
   ```
3. Implement the command handler in the appropriate `t_*.c` file.
4. Add integration tests in `tests/unit/`.

> **Note:** CI validates that `commands.def` is in sync with `src/commands/*.json`. Never edit `commands.def` or `fmtargs.h` by hand — they are generated files.

## Working guidelines

- Keep changes minimal and easy to backport.
- Match the style of surrounding code; do not introduce new patterns.
- Avoid unrelated refactors in the same change.
- Prefer heuristics over excessive configuration.

## Files to avoid

- Do not commit runtime artifacts: `dump.rdb`, `nodes.conf`, `*.log`, ad-hoc cluster directories.
- Treat `deps/` as special-case — only modify vendored code when the task explicitly requires it.
- Do not manually edit generated files: `commands.def`, `fmtargs.h`.

## Pull Requests

- Always push to your personal fork. **Never push directly to `valkey-io/valkey`.**
- Never push directly to the `unstable` branch.
- If your fork does not exist, create one before pushing.
- All commits must include a DCO sign-off: `git commit -s`
- For large features, open an Issue for discussion before submitting a PR.

## Troubleshooting

| Symptom | Fix |
|---|---|
| Build fails with jemalloc errors | `make distclean && make` |
| `commands.def` out of sync | `python utils/generate-command-code.py` |
| `clang-format` CI check fails | `clang-format-18 -i <modified files>` |
| Integration tests fail to start (missing `tclsh`) | Install Tcl 8.5+: `apt install tcl` / `brew install tcl-tk` |
| TLS tests fail due to missing certificates | `./utils/gen-test-certs.sh` |
