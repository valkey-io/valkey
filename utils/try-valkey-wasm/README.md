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

## Open items before this is a product

1. **JS `valkey-cli` shim**: line→argv (compile `sdssplitargs` into the module
   and export it), RESP→TTY formatter mirroring `cliFormatReplyTTY`, xterm.js.
2. Browser smoke (Chrome/Firefox/Safari) of the lowered build; decide whether to
   also ship native wasm64 behind feature detection.
3. `FD_SETSIZE` guard: fail loudly if `maxclients` is too large for select.
4. Per-release CI: `emmake make` on tag → publish `.wasm/.mjs` to the site repo.
5. Upstream conversation: which of these hooks land in-tree (the `getTimeZone`
   fix and `bioExecuteJob` refactor stand on their own) vs. stay in an overlay.
