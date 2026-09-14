# Retiring 32-bit Valkey: run try-valkey on a 64-bit WebAssembly build

*Design / summary for valkey-io. Rain Valentine, September 2026. PoC: branch `exp/try-valkey-wasm` on
valkey-rainfall/valkey (upstream base `da91ccd12`), live demo at
[valkey-rainfall.github.io/valkey/index.html](https://valkey-rainfall.github.io/valkey/index.html).*

## Goal

Drop 32-bit support from Valkey -- `make 32bit`, the `build-32bit` CI job, the 32-bit bucket layout in
`hashtable.c`, the `arch_bits == 32` maxmemory default and the other `sizeof(long) == 4` branches -- without
losing [try.valkey.io](https://valkey.io/try-valkey/). Today that page is the main thing
keeping 32-bit alive: it boots an Alpine image in v86, a 32-bit-only x86 emulator, with a 32-bit `valkey-server`
7.2.6 from Alpine's package repository ([#1412](https://github.com/valkey-io/valkey/issues/1412)). The page is
also frozen on 7.2.6 because producing a 32-bit image per release was never automated.

The end state: try-valkey runs the current release as a **64-bit** WebAssembly build produced by CI, and the
32-bit removal proceeds as its own PR with no user-facing casualty. (Other 32-bit consumers -- distro
packagers and the Docker `arm/v7` image -- are inventoried below; this proposal removes try-valkey as an
argument for keeping 32-bit, and the removal PR handles the rest with notice.)

## PoC approach

Compile the real `valkey-server` with Emscripten as an **LP64 program** (`sizeof(long)==8`, all 32-bit code
paths dead) and run it in the tab, no emulator, no Linux image. Two design points made it small:

1. **No TCP is needed.** The 2024 proposal rejected WASM because "WASM does not support TCP networking". The
   server does not need TCP; it needs a `ConnectionType`, which is already a pluggable abstraction (the RDMA
   transport registers one out of tree via `connTypeRegister()`). A ~400-line in-memory connection type driven
   from JavaScript is the entire bridge. Clients are created through `acceptCommonHandler()`, so MULTI, WATCH,
   pub/sub, blocking commands, RESP3, ACLs and `CLIENT LIST` are the unmodified server.
2. **64-bit code, wasm32 artifact.** Emscripten's `-sMEMORY64=2` compiles the C with 64-bit pointers and lowers
   the module to plain wasm32 at link time. The artifact runs in every current browser including Safari
   (true memory64 is Chrome 133+ / Firefox 134+ only, and is one link flag away when Safari ships it).

The browser side is a small JS layer: command lines are split by the server's own `sdssplitargs` (exported from
the module) and replies rendered by a port of `valkey-cli`'s `cliFormatReplyTTY`; Tab completion and inline
argument hints are a port of its linenoise callbacks fed by `COMMAND DOCS`. No threads (`bio` jobs run inline),
so no SharedArrayBuffer and no COOP/COEP headers: the bundle is seven static files servable from GitHub Pages.

### Evidence

| | |
|---|---|
| Stock Tcl unit suite against the wasm server (external mode over a TCP→in-memory bridge) | **2508 passed, 0 failed**, 59 units. Skipped: busy-script KILL tests, replication-stream tests (`SYNC` needs fork), IPv6 bind, `CLIENT LIST ip` filters |
| 607-command differential vs a native build at the same SHA | **605 replies byte-identical**; the two differences are `CLIENT INFO`'s address field and `LOLWUT` |
| JS CLI layer vs real `valkey-cli --no-raw`, same 97 lines | 96 identical (the other is the page's `maxmemory-policy`) |
| Native build with all patches applied | C unit tests 828/828; Tcl 5554 passed, 0 failed |
| Artifact | 2.1 MB `.wasm` (658 KB brotli), cold start 40–70 ms, `INFO` reports `arch_bits:64` |
| Browsers | Chromium (automated), Safari (manual) |

All of this reruns in CI -- see the
[workflow](https://github.com/valkey-rainfall/valkey/blob/exp/try-valkey-wasm/.github/workflows/try-valkey-wasm.yml)
and a [passing run](https://github.com/valkey-rainfall/valkey/actions/runs/34677586681) (3 min on
`ubuntu-latest`): pinned emsdk build, native build at the same SHA, both differentials as gates, bundle as
artifact, optional Pages deploy, release on tag.

## Pros and cons

**Pros.** Instant start instead of booting a VM; a fraction of the download; the current release from one CI
job instead of a hand-made 7.2.6 image; and the page keeps working the day `make 32bit` is deleted. The
in-memory transport is also a socket-free way to drive the whole client path in-process, which is what a fuzz
harness or a C-level integration test wants. Running the test suite on a strict-ABI target found three latent
bugs in Valkey (below).

**Cons, honestly.**
- *Single thread.* A `while true do end` script cannot be interrupted by `SCRIPT KILL` from a second connection;
  there is no thread to deliver it (native uses `processEventsWhileBlocked`, which needs readable fds). Fixable
  with a Worker plus `Atomics.wait`, at the price of COOP/COEP headers. Acceptable for a demo page.
- *No fork.* `BGSAVE`, `BGREWRITEAOF`, replication are denied via ACL for the visitor; `SAVE` works.
- *Toolchain care.* Emscripten's 64 KB default stack silently overflowed on `lzf_compress`'s on-stack 64K-slot
  table; `long double` prints at double precision unless `-sPRINTF_LONG_DOUBLE=1`. Both are link flags now; both
  were found only by running the full suite.
- *A new platform to not break.* ~50 lines of `#ifdef __EMSCRIPTEN__` in core files, plus a CI job. This is the
  cost being traded against the 32-bit job and its code paths.
- *Fewer tutorial options than a VM* (the 2024 design could freeze a multi-node cluster inside the image).

## Changes, grouped by where they should land

546 lines across 12 files in `src/`; 413 of them are one new file.

### A. Genuine bugs, upstreamable now regardless of this project

| Change | Why |
|---|---|
| `valkeymodule.h`: `FreeModuleUser`, `ACLAddLogEntry`, `ACLAddLogEntryByUserName` declared `void`, implemented and documented as returning `int` | x86-64 ignores the mismatch; wasm traps on the indirect call the moment a Lua script calls `redis.acl_check_cmd`. Header changed to `int`; ABI-neutral on native. A checker script compares every declaration against `module.c`; these are the only three real mismatches (the rest are typedef aliases) |
| `util.c`: `getTimeZone()` non-Linux path reads `gettimeofday`'s obsolete timezone argument, which modern libcs leave uninitialized | Garbage log timestamps on any non-Linux/non-Solaris target. Derive from `localtime_r().tm_gmtoff` instead (currently gated to Emscripten in the PoC; arguably should replace the whole non-Linux branch) |
| `script_lua.c`: `luaReplyToServerReply` recurses ~8000 levels on a self-referencing table before `lua_checkstack` fails | Only safe with an 8 MB native stack; on any engine with a smaller call-stack limit it is an uncatchable overflow. PoC caps at 256 levels under Emscripten with the existing `ERR reached lua stack limit`; a small cap would be reasonable everywhere |
| `bio.c`: worker-loop body extracted into `bioExecuteJob()` | Pure refactor, no behavior change; enables the inline mode below without forking the loop |

### B. Build-target support: `__EMSCRIPTEN__`-gated, proposed for in-tree behind a build option

These are the wasm equivalent of the existing `__linux__`/`__APPLE__`/`__FreeBSD__` branches. Zero effect on
native builds; the register call compiles to a stub.

| Change | Lines | Purpose |
|---|---|---|
| `connmem.c` (new) + `connection.{h,c}` + `Makefile` | 413 + 8 | `CONN_TYPE_MEM`: socket-less connection type driven by the host; exports `tv_connect/write/tick/read/close` and `tv_encode_command` (`sdssplitargs`). TCP-FIN-like linger so the final `-ERR Protocol error` reaches the host |
| `bio.c` `BIO_INLINE` | 6 | Run background jobs at submit time when there are no threads |
| `script_lua.c` depth cap | see A | |
| `ae.c` | 9 | `aeMain()` hands the loop to the host (`emscripten_exit_with_live_runtime`) |
| `server.c` | 11 | `initListeners()` registers the mem listener so the "not listening anywhere" exit is not taken |
| `module.c` | 24 | Resolve the static Lua module's entry points from a table (no `dlsym` in wasm) |
| `socket.c` | 9 | `connSocketListen()` fails cleanly so `CONFIG SET port` returns the standard error |
| build recipe | — | `MALLOC=libc BUILD_TLS=no`, `-sMEMORY64=2 -sSTACK_SIZE=8MB -sPRINTF_LONG_DOUBLE=1`, `--maxclients` below `FD_SETSIZE` for the select backend |

If maintainers prefer not to carry a wasm target in tree, B can live as a patch set in the try-valkey repo
applied by its CI; the cost is that every upstream refactor of `ae`/`bio`/`connection` can silently break the
page. In tree with a CI job, it cannot.

### C. try-valkey itself: not server code, lives in the try-valkey repo (or the website repo)

The JS CLI layer (`cli-core.mjs`, `cli-hints.mjs`, `tryvalkey.mjs`, the xterm page), the TCP→in-memory bridge
used to run the Tcl suite, the two differential harnesses, the CI workflow, the Pages publisher. The sign-on
animation is offered as a separate contribution and is not part of this proposal.

## Proposed sequence

1. **PRs for A** (three small, independent PRs; each stands on its own merits and passes the existing CI).
2. **Discussion: where B lives.** Proposal: in tree behind `BUILD_WASM=yes`, with a CI job that builds it and runs
   the byte-for-byte differential against the native build of the same commit (the
   [PoC workflow](https://github.com/valkey-rainfall/valkey/blob/exp/try-valkey-wasm/.github/workflows/try-valkey-wasm.yml)
   already does this in ~3 minutes).
3. **try-valkey repo**: C plus a release workflow that builds the bundle on each Valkey release tag and publishes
   to Pages. Switch valkey.io/try-valkey to it.
4. **Deprecate and remove 32-bit**: separate PR, once 3 is live. Announce for packagers.

## Who else builds 32-bit Valkey (checked September 2026)

The project itself publishes no 32-bit binaries ([valkey.io/download](https://valkey.io/download/) is
x86_64 and arm64) -- **except the official Docker image**, which is published for `linux/arm/v7` because
`valkey-container` inherits its base images' architectures. That is the one 32-bit artifact the project
controls and the one with users rather than packagers behind it (32-bit Raspberry Pi OS, embedded ARM).

| Distro | 32-bit arches with a current valkey | |
|---|---|---|
| Debian sid | `i386`, `armhf` official; `x32`, `m68k`, `sh4`, `hppa` unofficial ports | 9.1.2, same as amd64 |
| Ubuntu (devel) | `armhf` | 9.1.2 FULLYBUILT; no i386 (allowlist since 19.10) |
| Alpine edge | `x86`, `armv7`, `armhf` | 9.0.4; source of try-valkey's 7.2.6 image |
| Void Linux | `i686`, `armv6l`, `armv7l` | no `archs` restriction in the template |
| Yocto / OpenEmbedded meta-oe | any MACHINE, 32-bit ARM common | recipe at 9.1.1 |
| Raspberry Pi OS 32-bit | `armhf` | via Debian |

(Fedora/RHEL/EPEL exclude 32-bit x86 in the spec and dropped armv7 in F37; Arch and Homebrew are 64-bit only.)

These distros build 32-bit because they build everything for their arches, not because anyone asked; a
dropped arch is routine for them given notice. So the removal PR should come with: a deprecation note one
minor release ahead, an explicit `#error` with a clear message on 32-bit builds instead of a silent
miscompile, a heads-up to the Debian/Alpine/Void maintainers, and a decision on the Docker `arm/v7` tag
(discontinue with an announcement, or keep 32-bit for it -- pull counts per architecture would inform this).

## Open questions for maintainers

- In tree (B) vs overlay: which do you want to own?
- Should the `getTimeZone()` fix replace the whole non-Linux branch (affects macOS/BSD logging)?
- Is a recursion cap in `luaReplyToServerReply` acceptable on native too, or Emscripten-only?
- Docker `arm/v7`: discontinue with the 32-bit removal, or is that the reason to keep 32-bit?
