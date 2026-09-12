# try-valkey: real valkey-server as a 64-bit WebAssembly module

Spike result (2026-09-11, upstream `da91ccd12`): **the unmodified Valkey command
engine runs in WebAssembly, compiled LP64, driven through an in-process
connection type, and its replies are byte-identical to a native build at the
same SHA for 605/607 probes** (the two differences are `CLIENT INFO`'s address
field and `LOLWUT`'s random art).

This replaces the v86 approach (32-bit x86 emulator booting an Alpine image
with a 32-bit `valkey-server` from apk) and removes try-valkey from the list of
reasons Valkey keeps a 32-bit build.

## Numbers

| | |
|---|---|
| Build | `emmake make` of the stock tree, `-O2`, no DWARF: **2.1 MB wasm** (814 KB gzip, 658 KB brotli) + 107 KB JS |
| Cold start (Node 22, this host) | instantiate 19 ms, `main()` to ready 38 ms, first `PING` 4 ms |
| Pointer model | `sizeof(long)==8`, `__LP64__`, `INFO` reports `os:Emscripten 6.0.9 wasm64`, `arch_bits:64` |
| Engine compatibility | lowered mode (`-sMEMORY64=2`): runs on every wasm32 engine incl. Safari. Native wasm64 (`WASM_MODE=native`) also links; needs Chrome 133+/Firefox 134+/Node 24+ |
| Differential test | 607 commands (strings, hashes, lists, sets, zsets, MULTI/WATCH, EVAL/FCALL, RESP2+RESP3, error texts, 400 seeded random ops): 605 identical |
| **Stock Tcl unit suite** | **2508 passed, 0 failed** (59 units, external mode over `tcp-bridge.mjs`, ~4 min of test time). 94 skipped: busy-script KILL tests, replication-stream tests (SYNC needs fork), IPv6 bind, CLIENT LIST ip filter; plus the harness's own external-mode/needs:debug/slow/large-memory exclusions |
| CLI layer vs real `valkey-cli --no-raw` | 97 command lines incl. quoting/escapes/binary/RESP3/MULTI: 96 identical (the one difference is the try page's `maxmemory-policy`) |

## How it works

```
 browser / node                       wasm (valkey-server.wasm)
 ─────────────────                     ──────────────────────────────────────
 RESP bytes ──► tv_write(id, buf) ──► memConnection.inbox ──► readQueryFromClient
                tv_tick()          ──► aeProcessEvents(DONT_WAIT):
                                         beforeSleep → writeToClient → connWrite
                                         serverCron, expiry, blocked-client timeouts
 RESP bytes ◄── tv_read(id, buf)   ◄── memConnection.outbox
```

* **`src/connmem.c`** -- a `ConnectionType` (`CONN_TYPE_MEM`) registered like RDMA
  via `connTypeRegister()`. No fd, no ae file events; the host pump invokes the
  stored read/write handlers. Clients created through the normal
  `acceptCommonHandler()` path, so MULTI, WATCH, pub/sub, CLIENT TRACKING,
  blocking commands and `CLIENT LIST` (`addr=mem:0`) all work unmodified.
  Exports `tv_connect / tv_write / tv_tick / tv_pending / tv_read / tv_close`.
* **Event loop**: `ae` auto-selects `ae_select.c` (no `__linux__`), and
  `aeMain()` hands the loop to the host via `emscripten_exit_with_live_runtime()`.
  Requires `maxclients` small enough that `setsize < FD_SETSIZE` (1024).
* **Threads**: none. `bio.c` gets a `BIO_INLINE` mode (jobs run at submit time)
  so the build needs no `-pthread`, hence no SharedArrayBuffer / COOP+COEP
  headers -- it can be served from GitHub Pages as-is.
* **Lua**: statically linked as today (`STATIC_LUA=1`); `module.c` resolves
  `ValkeyModule_OnLoad_lua` from a table instead of `dlsym()` (no dynamic
  linking in wasm).
* **fork()**: not shimmed. `--save "" --appendonly no`; deny `BGSAVE`,
  `BGREWRITEAOF`, `SHUTDOWN`, `REPLICAOF`, `DEBUG` via ACL for the visitor.
  Plain `SAVE` works (synchronous, to MEMFS).

## Patch footprint (all `__EMSCRIPTEN__`-gated except where noted)

| File | Change |
|---|---|
| `src/connmem.c` | new: mem connection type + host API (stub register elsewhere) |
| `src/connection.{h,c}` | `CONN_TYPE_MEM` enum/name, `RegisterConnectionTypeMem()` call (unconditional, no-op natively) |
| `src/server.c` | `initListeners()` registers the mem listener |
| `src/ae.c` | `aeMain()` yields to host |
| `src/bio.c` | loop body split into `bioExecuteJob()` (unconditional refactor); `BIO_INLINE` mode |
| `src/module.c` | static-symbol table in `moduleLoadStaticSymbol()` |
| `src/util.c` | `getTimeZone()`: `gettimeofday(NULL,&tz)` is obsolete and returns garbage here; use `tm_gmtoff` |
| `src/Makefile` | `connmem.o` |

Native build and `unit/lazyfree` + `unit/type/incr` pass with the patches.

## Files here

* `build.sh` -- `source ~/emsdk/env.sh && utils/try-valkey-wasm/build.sh` (`WASM_MODE=lowered|native`, `DEBUG=""` for a release build)
* `roundtrip.mjs` -- boots the server, exercises MULTI, EVAL, RESP3, pub/sub across two connections, expiry, BLPOP timeout
* `diff-harness.mjs <native valkey-server>` -- the byte-for-byte comparison
* `boot-probe.mjs`, `time-probe.c` -- diagnostics used during the spike

## Bugs the wasm build found in Valkey itself

Strict wasm type checking and a small default stack turned three latent issues
into hard failures. All three are upstream-worthy on their own:

1. **`valkeymodule.h` return-type mismatches**: `FreeModuleUser`,
   `ACLAddLogEntry`, `ACLAddLogEntryByUserName` are declared `void` but
   implemented (and documented) as returning `int`. x86-64 ignores it; wasm
   traps at the indirect call as soon as a Lua script uses
   `redis.acl_check_cmd`. `api-sig-check.py` compares every declaration
   against `module.c` (the other reported differences are typedef aliases).
2. **`getTimeZone()`** on non-Linux reads `gettimeofday`'s obsolete timezone
   argument, which modern libcs leave untouched (garbage log timestamps).
3. **Deep recursion in `luaReplyToServerReply`**: a recursive Lua table
   recurses ~8000 levels before `lua_checkstack` fails. Fine with an 8 MB
   native stack; on any engine with a smaller call-stack limit it is an
   uncatchable overflow. The wasm build caps it at 256 levels with the same
   `reached lua stack limit` error.

Toolchain gotchas: Emscripten's default 64 KB stack (lzf's 64K-slot table
lives on the stack -> `-sSTACK_SIZE=8MB`); `long double` printed at double
precision unless `-sPRINTF_LONG_DOUBLE=1` (HINCRBYFLOAT 1.23 ->
1.22999999999999998); `getaddrinfo` glue has a BigInt bug in the lowered mode
(sidestepped: TCP listen is disabled in this build anyway).

## Known limitation

The server is single-threaded on the page's main thread, so a busy Lua
script (`while true do end`) cannot be interrupted by `SCRIPT KILL` from a
second connection -- there is no second thread to deliver it. Native Valkey
handles this via `processEventsWhileBlocked`, which needs readable fds. A
Web Worker + `Atomics.wait`-based wake would fix it (needs COOP/COEP).

## Running the suite yourself

```
node utils/try-valkey-wasm/tcp-bridge.mjs --port 7379 &
./runtest --host 127.0.0.1 --port 7379 --singledb --clients 1 \
  --tags "-needs:repl -needs:save -needs:debug -needs:reset -needs:other-server -needs:latency -slow -large-memory" \
  --skipfile utils/try-valkey-wasm/skip-busy-script-tests.txt --single unit/type/string ...
```

Demo page: `cp web/* out/web/ && cp ../../src/valkey-server.{mjs,wasm} out/web/ &&
python3 -m http.server 8765 --bind 127.0.0.1 --directory out/web`.

## CLI features

`web/cli-hints.mjs` is a port of valkey-cli's linenoise callbacks, fed by the real
server's `COMMAND DOCS`: Tab cycles through matching commands/subcommands (then
back to what was typed), and the grey inline hint shows the argument syntax still
to be typed, with already-typed tokens and option groups removed, exactly as
valkey-cli does (`hints-check.mjs` prints a table of cases).

## CI (`.github/workflows/try-valkey-wasm.yml`)

On every push to the branch that touches `src/`, `deps/` or `utils/try-valkey-wasm/`: install emsdk (pinned
6.0.9), run `build.sh`, boot-smoke under Node, build **native at the same SHA**, then run `diff-harness.mjs`
(607 commands byte-for-byte; only CLIENT INFO's address and LOLWUT may differ) and `cli-diff.mjs` (the JS CLI
layer against real `valkey-cli --no-raw`). The bundle is uploaded as a workflow artifact (30 days).
`workflow_dispatch` with `publish=true` deploys it to GitHub Pages; a `try-valkey-v*` tag attaches the tarball
to a GitHub Release. This is the reproducible record of the port for anyone who wants to redo it.

## Page options

The start panel has two buttons (silent / with sound). Everything else is behind URL params or the browser
console: `tryvalkey.help()` lists them, `tryvalkey.options({sound: 'sid', dl: 4, boot: 3})` sets them and
replays, `tryvalkey.reset()` clears. Options: `sound` (fm | sid | turbine | launch | mute), `mode` (connect |
keyturn), `tape=1`, `dl`/`boot` (minimum seconds, to demo the vamps), `mock=1`, `run=1`. Defaults: silent,
real timing, FM synth, no tape, CONNECT. `tryvalkey.server` is the CLI engine (`.exec('PING')`).

## Hosting

Live at [valkey-rainfall.github.io/valkey/tryme.html](https://valkey-rainfall.github.io/valkey/tryme.html)
(plain terminal: `index.html`) from the fork's orphan `gh-pages` branch, which holds only the deployable bundle.
`publish-pages.sh` rebuilds that branch from `web/` plus the current `src/valkey-server.{mjs,wasm}` and pushes it.
GitHub Pages serves `.wasm` as `application/wasm` and `.mjs` as `text/javascript`; no special headers are needed
because the build uses no threads (no SharedArrayBuffer, no COOP/COEP).

## Open items before this is a product

1. ~~JS `valkey-cli` shim~~ done (`web/`), verified against real `valkey-cli`.
2. ~~Safari smoke~~ done: the lowered build runs in Safari (Rain, Sep 12 2026; the
   sign-on graphics are a bit off there, the server and terminal are fine).
   Decide whether to also ship native wasm64 behind feature detection.
3. `FD_SETSIZE` guard: fail loudly if `maxclients` is too large for select.
4. Per-release CI: `emmake make` on tag -> publish `.wasm/.mjs` to the site repo.
5. Upstream: the three findings above plus `bioExecuteJob` refactor stand on
   their own; decide which wasm hooks land in-tree vs. stay in an overlay.
6. `-sSTACK_OVERFLOW_CHECK` builds crash at boot in `genValkeyInfoString`
   (also plain wasm32) -- unresolved, debug-tooling only.
