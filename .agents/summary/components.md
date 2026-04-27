# Components

<!-- metadata: topic=components; audience=ai-agents,developers -->

Groups of source files under `src/`, with the role each plays. Files are listed without size metrics because those change over time.

## 1. Server Lifecycle and Dispatch

| File | Role |
|---|---|
| `server.c` / `server.h` | Main entry, event-loop wiring, cron tasks, command dispatch (`processCommand`), global `valkeyServer` state. `main()` is declared `__attribute__((weak))` so that unit-test executables can provide their own. |
| `config.c` | Config file / CLI parsing, `CONFIG GET/SET`. |
| `db.c` | Per-database key lookup, expire, notify, signal-key-modified. |
| `object.c` | `robj` allocation, encoding choice, refcount, OBJ_* introspection helpers. |
| `notify.c` | Keyspace notifications (publish events on key change). |
| `pubsub.c` | `PUBLISH`/`SUBSCRIBE`/`PSUBSCRIBE`, including sharded pubsub. |
| `multi.c` | `MULTI`/`EXEC` transaction buffering and watch. |
| `blocked.c` / `timeout.c` | Blocking commands (e.g. `BLPOP`), per-client timeouts, ready-keys handling. |
| `lazyfree.c` | Async free of large objects (off-main-thread). |
| `evict.c` | Maxmemory eviction policies. |
| `expire.c` | Active and passive key expiration. |
| `defrag.c` / `allocator_defrag.c` | Active memory defragmentation using jemalloc hints. |
| `acl.c` | ACL users, passwords, categories, command rules, key patterns, channel patterns. |
| `commandlog.c` | Commandlog / slowlog equivalent. |
| `latency.c` | Latency monitor events and history. |
| `debug.c` | `DEBUG` command family, assertions, crash reporting. |
| `tracking.c` | Client-side cache invalidation tracking (RESP3). |
| `logreqres.c` | Request/response logging for schema validation. |

## 2. Networking

| File | Role |
|---|---|
| `ae.c`, `ae.h`, `ae_epoll.c`, `ae_kqueue.c`, `ae_evport.c`, `ae_select.c` | Portable event loop — one backend selected at compile time. |
| `networking.c` | Client I/O state machine, query buffer, output buffer, reply building, argv parsing. |
| `anet.c` | Low-level socket helpers. |
| `connection.c` + `connection.h` | Connection type abstraction (`ConnectionType` vtable). |
| `socket.c`, `unix.c` | TCP and Unix-domain connection types. |
| `tls.c`, `tls.h` | OpenSSL-backed TLS connection type (built-in or module). |
| `rdma.c` | RDMA connection type (Linux, experimental; module-only in CMake). |
| `resp_parser.c`/`.h` | RESP protocol parsing helpers (used by modules' call-reply path). |
| `syncio.c` | Synchronous socket I/O (used only during handshake-like phases). |
| `io_threads.c` | Optional worker threads that parallelize socket read/write. |
| `memory_prefetch.c` | CPU-cache prefetch hints for hashtable walks during pipelining. |

## 3. Data Types (commands that operate on a given type)

| File | Type |
|---|---|
| `t_string.c` | `OBJ_STRING` — strings, counters, `SET/GET/INCR/APPEND/...` |
| `t_list.c` | `OBJ_LIST` — quicklist-backed lists, `LPUSH/LPOP/BLPOP/...` |
| `t_hash.c` | `OBJ_HASH` — hashes (listpack or hashtable encoding), field expiration |
| `t_set.c` | `OBJ_SET` — sets (intset, listpack, or hashtable) |
| `t_zset.c` | `OBJ_ZSET` — sorted sets (listpack or skiplist + hashtable); contains the skiplist implementation |
| `t_stream.c` | Streams — rax-based log, consumer groups |
| `vset.c` | Vector sets (HNSW-like ANN vectors) |
| `hyperloglog.c` | HyperLogLog approximate cardinality (encoded as string with sparse/dense layouts) |
| `geo.c`, `geohash.c`, `geohash_helper.c` | Geospatial commands on top of sorted sets |
| `bitops.c` | Bit-level commands (`SETBIT`, `BITCOUNT`, `BITOP`, `BITPOS`) |
| `sort.c` | `SORT`/`SORT_RO` |

## 4. Core Data Structures (low-level)

| File | Structure |
|---|---|
| `sds.c`/`.h` | Simple Dynamic String — the string type used everywhere |
| `dict.c` (legacy) / `hashtable.c` (current) | Hash table implementations; new code should use `hashtable` |
| `kvstore.c` | Sharded keyspace on top of `hashtable` |
| `listpack.c`, `ziplist.c` | Compact sequential encodings (listpack is the modern one; ziplist is kept for compatibility with older RDBs/migrations) |
| `quicklist.c` | Linked list of listpacks |
| `intset.c` | Sorted integer set |
| `zipmap.c` | Legacy hash encoding (still supported for RDB loading) |
| `rax.c` | Radix tree (used by streams and client tracking) |
| `adlist.c` | Plain doubly-linked list |
| `vector.c` | Dynamic array primitive |
| `queues.c` / `mutexqueue.c` / `fifo.c` | Producer/consumer queue primitives |
| `entry.c` | Unified "entry" / key abstraction used by newer hashtable paths |

## 5. Persistence

| File | Role |
|---|---|
| `rdb.c` / `rdb.h` | RDB save/load, including forked save and diskless send |
| `aof.c` | AOF writing, rewrite, multi-part manifest |
| `rio.c` / `rio.h` | Abstract stream I/O: file / socket / in-memory / zero-copy |
| `valkey-check-rdb.c` | Standalone validator binary |
| `valkey-check-aof.c` | Standalone validator binary |
| `childinfo.c` | Coordination between parent and forked child (copy-on-write info, fork ACK) |

## 6. Replication, Cluster, Sentinel

| File | Role |
|---|---|
| `replication.c` | Primary/replica state machine, PSYNC, backlog, dual-channel replication |
| `cluster.c` / `cluster.h` | Cluster common dispatch and public API |
| `cluster_legacy.c` / `cluster_legacy.h` | Gossip-based cluster protocol (the long-standing implementation) |
| `cluster_migrateslots.c` / `.h` | Newer atomic slot migration (see `design-docs/atomic-slot-migration.md`) |
| `cluster_slot_stats.c` / `.h` | Per-slot traffic statistics |
| `crc16.c` / `crc16_slottable.c` | CRC16 for slot hashing |
| `sentinel.c` | Sentinel mode — runs as `valkey-sentinel`; quorum-based failover |

## 7. Scripting and Modules

| File | Role |
|---|---|
| `module.c` | Module API implementation (thousands of exported `RM_*` / `VM_*` functions) |
| `redismodule.h` | Redis-compatibility module API header |
| `valkeymodule.h` | Valkey-branded module API header |
| `call_reply.c` / `.h` | Structured command reply (used by modules calling commands) |
| `script.c` / `.h` | Common scripting plumbing (effects, flags, contexts) |
| `scripting_engine.c` / `.h` | Pluggable scripting engine registration surface |
| `eval.c` | Built-in Lua `EVAL`/`EVALSHA` implementation |
| `functions.c` / `.h` | Functions library (`FUNCTION LOAD`, `FCALL`) |
| `src/lua/` | Valkey-specific Lua glue (script_lua, function_lua, engine_lua, debug_lua) |

## 8. Command Metadata Pipeline

- `src/commands/*.json` — 425 command description files (arity, flags, keys, ACL, reply schema).
- `utils/generate-command-code.py` — generator that emits `src/commands.def`.
- `src/commands.c` / `commands.h` — tiny wrappers; the bulk of the table lives in the generated `commands.def`.
- `src/commandlog.c` — records slow commands for introspection.

## 9. Tooling Binaries (share many `src/` files)

| Binary | Key files |
|---|---|
| `valkey-cli` | `valkey-cli.c`, `cli_common.c`, `cli_commands.c`, `linenoise` (from `deps/`), `libvalkey` |
| `valkey-benchmark` | `valkey-benchmark.c`, `fuzzer_client.c`, `fuzzer_command_generator.c` |
| `valkey-check-rdb` / `valkey-check-aof` | reuse `rdb.c`/`aof.c` logic |

## 10. Platform / Infra Helpers

| File | Role |
|---|---|
| `monotonic.c` / `.h` | Processor clock (TSC/CNTVCT) with fallback to `clock_gettime` |
| `threads_mngr.c` | Signal-based cross-thread stack collection |
| `bio.c` | Background I/O worker pool |
| `zmalloc.c` | Allocator wrapper (jemalloc/libc/tcmalloc) + accounting |
| `setproctitle.c`, `setcpuaffinity.c` | Process presentation / affinity |
| `syscheck.c` | Startup system checks (transparent hugepages, overcommit, etc.) |
| `util.c`, `valkey_strtod.c`, `localtime.c`, `strl.c`, `siphash.c`, `sha1.c`, `sha256.c`, `mt19937-64.c`, `rand.c`, `crc64.c`, `crcspeed.c`, `crccombine.c` | Misc utilities |
| `serverassert.c` / `.h` | Assertion macros that produce a panic + crash report |
| `setproctitle.c` | Portable `setproctitle` for process lists |

## 11. Tracing

| File | Role |
|---|---|
| `src/trace/trace.c` + `trace_*.c`/`.h` | LTTng tracepoint providers: `valkey_server`, `valkey_commands`, `valkey_db`, `valkey_cluster`, `valkey_rdb`, `valkey_aof` |
| `src/trace/README.md` | Install / enable instructions |

## 12. Example Modules (shipped for reference)

Under `src/modules/`:

- `helloworld.c` — general API demo
- `hellotype.c` — custom keyspace data type
- `hellocluster.c` — cluster messaging
- `hellohook.c` — event hooks
- `hellotimer.c` — timers
- `helloblock.c` — blocking commands
- `helloacl.c` — custom auth
- `hellodict.c` — dict / `OnLoad` walkthrough
- `src/modules/lua/` — Lua engine packaged as a module when built with `BUILD_LUA=module`

## 13. Tests

See `tests/README.md` and `src/unit/README.md`.

| Area | Location | Framework |
|---|---|---|
| C++ unit tests for low-level structures | `src/unit/*.cpp` (including `cluster/`) | GoogleTest + `ld --wrap` mock glue |
| Tcl unit tests | `tests/unit/*.tcl` | Valkey Tcl harness (`runtest`) |
| Tcl integration tests | `tests/integration/*.tcl` | `runtest` |
| Tcl cluster tests (new) | `tests/unit/cluster/*.tcl` | `runtest-cluster` |
| Tcl cluster tests (legacy, deprecated) | `tests/cluster/` | `runtest-cluster` |
| Sentinel tests | `tests/sentinel/` | `runtest-sentinel` |
| Module API tests | `tests/modules/*.c` (47 modules) + `tests/unit/moduleapi/*.tcl` | `runtest-moduleapi` |
| RDMA tests | `tests/rdma/` | `runtest-rdma` |
| Shared helpers | `tests/support/*.tcl`, `tests/helpers/` | — |
