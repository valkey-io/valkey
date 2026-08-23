# Pause Replica Full Synchronization Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add `REPLSYNC PAUSE|RESUME` so an operator can safely abort and hold an in-progress replica full synchronization, then explicitly resume it.

**Architecture:** Represent the held condition with `REPL_STATE_PAUSED` plus a `server.repl_sync_paused` policy flag. Ordinary handshake and transfer cancellation reuses `cancelReplicationHandshake()`, while a command received from the nested diskless RDB-loading event loop requests `rioCloseASAP()` and lets the loader unwind before replication connection cleanup.

**Tech Stack:** Valkey C server, generated JSON command metadata, Tcl integration tests, clang-format-18, Make.

**Spec:** `docs/superpowers/specs/2026-08-21-pause-replication-sync-design.md`

## Global Constraints

- Preserve the configured primary, replication IDs, and replica role while paused.
- Do not reconnect automatically until `REPLSYNC RESUME`.
- Do not close or free a replication connection while the active RDB `rio` still owns it.
- Support standalone and cluster mode because the command does not change topology.
- Keep the command runtime-only: do not persist or propagate it.
- Keep changes minimal and follow existing replication and command styles.
- Add Tcl integration coverage; no STL-based C++ unit tests are needed for this end-to-end state-machine behavior.

---

## File Structure

- Create `src/commands/replsync.json`: command arity, flags, arguments, replies, and ACL metadata.
- Create `tests/integration/replication-replsync.tcl`: focused command, retry suppression, resume, and RDB-load cancellation tests.
- Modify `src/server.h`: add `REPL_STATE_PAUSED`, `server.repl_sync_paused`, and `replsyncCommand()` declaration.
- Modify `src/server.c`: initialize the pause flag and expose `master_sync_paused` in `INFO replication`.
- Modify `src/replication.c`: implement pause/resume, make cancellation pause-aware, clear pause policy on topology changes, and report `paused` from `ROLE`.
- Modify `src/commands/role.json`: document `paused` as a possible replica link state.
- Regenerate `src/commands.def`: register the new command and updated ROLE schema.

### Task 1: Add the paused replication state and command

**Files:**
- Create: `src/commands/replsync.json`
- Create: `tests/integration/replication-replsync.tcl`
- Modify: `src/server.h:387-408,1230-1240,4070-4085`
- Modify: `src/server.c:2410-2430,6625-6650`
- Modify: `src/replication.c:4502-4529,4535-4630,4695-4810,5385-5395`
- Modify: `src/commands/role.json`
- Modify generated: `src/commands.def`

**Interfaces:**
- Produces: `void replsyncCommand(client *c)`.
- Produces: `REPL_STATE_PAUSED` in `repl_state`.
- Produces: `int server.repl_sync_paused`, equal to `1` from a successful pause until resume or topology replacement.
- Produces: `REPLSYNC PAUSE` and `REPLSYNC RESUME`, each returning `OK` on success.
- Produces: `master_sync_paused:0|1` and ROLE state `paused`.

- [ ] **Step 1: Write the failing basic command and state-machine test**

Create `tests/integration/replication-replsync.tcl` with this initial test:

```tcl
start_server {tags {repl} overrides {save ""}} {
    set primary [srv 0 client]
    set primary_host [srv 0 host]
    set primary_port [srv 0 port]

    test {REPLSYNC is rejected on a primary} {
        assert_error {*REPLSYNC is only valid on a replica*} {$primary replsync pause}
        assert_error {*REPLSYNC is only valid on a replica*} {$primary replsync resume}
    }

    $primary config set repl-diskless-sync yes
    $primary config set repl-diskless-sync-delay 0
    $primary config set rdb-key-save-delay 1000
    $primary debug populate 2000 replsync 100

    start_server {tags {repl} overrides {save ""}} {
        set replica [srv 0 client]
        $replica replicaof $primary_host $primary_port

        wait_for_condition 200 50 {
            [s -1 master_sync_in_progress] eq 1
        } else {
            fail "Replica did not start a full synchronization"
        }

        test {REPLSYNC PAUSE aborts and holds a full synchronization} {
            assert_equal OK [$replica replsync pause]
            assert_equal OK [$replica replsync pause]
            wait_for_condition 100 20 {
                [s -1 master_sync_paused] eq 1 &&
                [lindex [$replica role] 3] eq "paused"
            } else {
                fail "Replica did not enter paused synchronization state"
            }

            set sync_full [s 0 sync_full]
            after 1200
            assert_equal $sync_full [s 0 sync_full]
            assert_equal down [s -1 master_link_status]
        }

        test {REPLSYNC RESUME reconnects and completes synchronization} {
            $primary config set rdb-key-save-delay 0
            assert_equal OK [$replica replsync resume]
            assert_equal OK [$replica replsync resume]
            wait_for_sync $replica 200 50
            assert_equal 0 [s -1 master_sync_paused]
            assert_equal connected [lindex [$replica role] 3]
        }

        test {REPLSYNC PAUSE does not interrupt an online replication stream} {
            assert_error {*cannot pause an online replication link*} {$replica replsync pause}
        }
    }
}
```

- [ ] **Step 2: Run the test and verify the command is missing**

Run:

```bash
./runtest --single tests/integration/replication-replsync.tcl
```

Expected: FAIL at the first assertion because `REPLSYNC` is an unknown command.

- [ ] **Step 3: Define command metadata**

Create `src/commands/replsync.json`:

```json
{
    "REPLSYNC": {
        "summary": "Pauses or resumes synchronization with the configured primary.",
        "complexity": "O(1)",
        "group": "server",
        "since": "9.0.0",
        "arity": 2,
        "function": "replsyncCommand",
        "command_flags": [
            "ADMIN",
            "NOSCRIPT",
            "NO_MULTI",
            "LOADING",
            "STALE"
        ],
        "arguments": [
            {
                "name": "action",
                "type": "oneof",
                "arguments": [
                    {"name": "pause", "type": "pure-token", "token": "PAUSE"},
                    {"name": "resume", "type": "pure-token", "token": "RESUME"}
                ]
            }
        ],
        "reply_schema": {"const": "OK"},
        "acl_categories": ["ADMIN", "DANGEROUS", "SLOW"]
    }
}
```

Add `"paused"` to the replica-state `oneOf` constants in `src/commands/role.json`. Regenerate the checked-in table:

```bash
make -C src commands.def
```

- [ ] **Step 4: Add state and initialization**

In `src/server.h`, insert `REPL_STATE_PAUSED` immediately after `REPL_STATE_NONE`, add this field beside `repl_state`, and declare the handler beside `replicaofCommand()`:

```c
REPL_STATE_PAUSED, /* Synchronization paused by an operator */
```

```c
int repl_sync_paused; /* Prevent reconnect while a requested sync abort unwinds. */
```

```c
void replsyncCommand(client *c);
```

In `initServerConfig()` in `src/server.c` initialize:

```c
server.repl_sync_paused = 0;
```

- [ ] **Step 5: Make handshake cancellation pause-aware**

In `cancelReplicationHandshake()`, choose the post-cancel state from the pause flag in both cancellable branches and suppress immediate reconnect while paused:

```c
int next_state = server.repl_sync_paused ? REPL_STATE_PAUSED : REPL_STATE_CONNECT;
```

Assign `next_state` after `replicationAbortSyncTransfer()` and `undoConnectWithPrimary()`. Replace the reconnect guard with:

```c
if (!reconnect || server.repl_sync_paused) return 1;
```

Do not change the return value when there is no cancellable handshake or transfer.

- [ ] **Step 6: Implement pause and resume outside active RDB loading**

Add `replsyncCommand()` beside `replicaofCommand()`:

```c
void replsyncCommand(client *c) {
    if (server.primary_host == NULL) {
        addReplyError(c, "REPLSYNC is only valid on a replica");
        return;
    }

    if (!strcasecmp(objectGetVal(c->argv[1]), "pause")) {
        if (server.repl_state == REPL_STATE_CONNECTED) {
            addReplyError(c, "cannot pause an online replication link");
            return;
        }
        if (server.repl_state == REPL_STATE_PAUSED) {
            addReply(c, shared.ok);
            return;
        }
        if (server.loading_rio != NULL) {
            addReplyError(c, "cannot pause while the RDB loader is active");
            return;
        }

        server.repl_sync_paused = 1;
        if (!cancelReplicationHandshake(0)) server.repl_state = REPL_STATE_PAUSED;
        addReply(c, shared.ok);
    } else if (!strcasecmp(objectGetVal(c->argv[1]), "resume")) {
        server.repl_sync_paused = 0;
        if (server.repl_state == REPL_STATE_PAUSED) {
            server.repl_state = REPL_STATE_CONNECT;
            connectWithPrimary();
        }
        addReply(c, shared.ok);
    } else {
        addReplyErrorObject(c, shared.syntaxerr);
    }
}
```

This task deliberately rejects the active-loader case. Task 2 replaces that temporary error only after a failing diskless-load test exists.

- [ ] **Step 7: Reset pause policy on topology changes**

Set `server.repl_sync_paused = 0` at the start of both `replicationSetPrimary()` and `replicationUnsetPrimary()`. This ensures a new primary configuration and promotion do not inherit a runtime pause.

- [ ] **Step 8: Add ROLE and INFO visibility**

Add the paused switch case in `roleCommand()`:

```c
case REPL_STATE_PAUSED: replica_state = "paused"; break;
```

Add this field adjacent to `master_sync_in_progress` in `genValkeyInfoString()`:

```c
"master_sync_paused:%d\r\n", server.repl_sync_paused,
```

- [ ] **Step 9: Format, build, and run the focused test**

Run:

```bash
clang-format-18 -i src/server.h src/server.c src/replication.c
make noopt BUILD_TLS=yes MALLOC=jemalloc
./runtest --single tests/integration/replication-replsync.tcl
```

Expected: build exits 0 and every test in `replication-replsync.tcl` passes.

- [ ] **Step 10: Commit the basic paused state**

```bash
git add src/server.h src/server.c src/replication.c src/commands/replsync.json src/commands/role.json src/commands.def tests/integration/replication-replsync.tcl
git commit -m "feat: pause replica full synchronization"
```

### Task 2: Safely pause during diskless RDB loading

**Files:**
- Modify: `tests/integration/replication-replsync.tcl`
- Modify: `src/replication.c:2481-2565,2694-2745,4695-4750`

**Interfaces:**
- Consumes: `server.repl_sync_paused`, `REPL_STATE_PAUSED`, and `replsyncCommand(client *)` from Task 1.
- Consumes: `server.loading_rio` and `rioCloseASAP(rio *)`.
- Produces: safe deferred cancellation when PAUSE executes inside `processEventsWhileBlocked()` during diskless RDB loading.

- [ ] **Step 1: Add a failing diskless-load cancellation test**

Append this test to `tests/integration/replication-replsync.tcl`:

```tcl
start_server {tags {repl} overrides {save ""}} {
    set primary [srv 0 client]
    set primary_host [srv 0 host]
    set primary_port [srv 0 port]

    $primary config set repl-diskless-sync yes
    $primary config set repl-diskless-sync-delay 0
    $primary config set rdbcompression no
    $primary debug populate 5000 replsync-load 1000

    start_server {tags {repl} overrides {save "" repl-diskless-load flush-before-load key-load-delay 500 loading-process-events-interval-bytes 1024}} {
        set replica [srv 0 client]
        set replica_log [srv 0 stdout]
        $replica replicaof $primary_host $primary_port

        set log_result [wait_for_log_messages -1 {"*Loading DB in memory*"} 0 500 10]
        set loglines [lindex $log_result 1]

        test {REPLSYNC PAUSE safely aborts active diskless RDB loading} {
            assert_equal OK [$replica replsync pause]
            wait_for_condition 200 20 {
                [s -1 master_sync_paused] eq 1 &&
                [lindex [$replica role] 3] eq "paused" &&
                [s -1 loading] eq 0
            } else {
                fail "Replica did not safely unwind and pause diskless loading"
            }
            wait_for_log_messages -1 {"*Failed trying to load the PRIMARY synchronization DB from socket*"} $loglines 500 10

            set sync_full [s 0 sync_full]
            after 1200
            assert_equal $sync_full [s 0 sync_full]
        }

        test {REPLSYNC RESUME succeeds after aborting diskless loading} {
            $replica config set key-load-delay 0
            assert_equal OK [$replica replsync resume]
            wait_for_sync $replica 300 50
        }
    }
}
```

- [ ] **Step 2: Run the test and verify the temporary loader rejection**

Run:

```bash
./runtest --single tests/integration/replication-replsync.tcl
```

Expected: the new test fails with `ERR cannot pause while the RDB loader is active` while Task 1 tests remain green.

- [ ] **Step 3: Replace loader rejection with deferred abort**

In the PAUSE branch of `replsyncCommand()`, set the policy first and request that the scoped loader stop:

```c
server.repl_sync_paused = 1;
if (server.loading_rio != NULL) {
    rioCloseASAP(server.loading_rio);
} else if (!cancelReplicationHandshake(0)) {
    server.repl_state = REPL_STATE_PAUSED;
}
addReply(c, shared.ok);
```

Do not call `cancelReplicationHandshake()` while `server.loading_rio` is non-NULL. The existing `replicaLoadPrimaryRDBFromSocket()` error path will return to `replicaReceiveRDBFromPrimaryToMemory()`, which invokes cancellation after the scoped `rio` is restored. The pause-aware reconnect guard from Task 1 leaves the final state paused.

- [ ] **Step 4: Run the focused test**

Run:

```bash
clang-format-18 -i src/replication.c
./runtest --single tests/integration/replication-replsync.tcl
```

Expected: all basic and diskless-loading pause/resume tests pass, with no crash or assertion failure.

- [ ] **Step 5: Commit safe RDB-load cancellation**

```bash
git add src/replication.c tests/integration/replication-replsync.tcl
git commit -m "fix: safely abort paused diskless replication load"
```

### Task 3: Verify generated metadata and replication regressions

**Files:**
- Verify: `src/commands/replsync.json`
- Verify: `src/commands/role.json`
- Verify: `src/commands.def`
- Verify: `src/server.h`
- Verify: `src/server.c`
- Verify: `src/replication.c`
- Verify: `tests/integration/replication-replsync.tcl`

**Interfaces:**
- Consumes: complete `REPLSYNC PAUSE|RESUME` behavior from Tasks 1 and 2.
- Produces: evidence that command metadata, formatting, build, focused tests, and existing replication behavior are valid.

- [ ] **Step 1: Check generated command metadata is current**

Run:

```bash
make -C src commands.def
git diff --exit-code -- src/commands.def
```

Expected: both commands exit 0; regeneration creates no additional diff.

- [ ] **Step 2: Run formatting and static diff checks**

Run:

```bash
clang-format-18 -i src/server.h src/server.c src/replication.c
git diff --check
```

Expected: `git diff --check` exits 0 without whitespace errors.

- [ ] **Step 3: Run the no-optimization TLS/jemalloc build**

Run:

```bash
make noopt BUILD_TLS=yes MALLOC=jemalloc
```

Expected: all Valkey binaries link successfully with exit code 0.

- [ ] **Step 4: Run focused and existing replication integration tests**

Run:

```bash
./runtest --single tests/integration/replication-replsync.tcl
./runtest --single tests/integration/replication.tcl
```

Expected: both test files pass with zero failures.

- [ ] **Step 5: Inspect final branch state and commit any generated or formatting updates**

Run:

```bash
git status --short
git diff --check
```

If formatting or command regeneration changed tracked files, commit only those scoped updates:

```bash
git add src/server.h src/server.c src/replication.c src/commands.def
git commit -m "chore: finalize replsync command metadata"
```

If there are no remaining tracked changes, do not create an empty commit.
