Valkey Test Suite
=================

Overview
--------

Integration tests are written in Tcl, a high-level, general-purpose, interpreted, dynamic programming language [[source](https://wiki.tcl-lang.org/page/What+is+Tcl)].
`runtest` is the main entrance point for running integration tests.
For example, to run a single test;

```shell
./runtest --single unit/your_test_name
# For additional arguments, you may refer to the `runtest` script itself.
```

The normal execution mode of the test suite involves starting and manipulating
local `valkey-server` instances, inspecting process state, log files, etc.

The test suite also supports execution against an external server, which is
enabled using the `--host` and `--port` parameters. When executing against an
external server, tests tagged `external:skip` are skipped.

There are additional runtime options that can further adjust the test suite to
match different external server configurations. All options are listed by
`./runtest --help`. The following table is just a subset of the options:

| Option                     | Impact                                                   |
| -------------------------- | -------------------------------------------------------- |
| `--singledb`               | Only use database 0, don't assume others are supported. |
| `--ignore-encoding`        | Skip all checks for specific encoding.  |
| `--ignore-digest`          | Skip key value digest validations. |
| `--cluster-mode`           | Run in strict Valkey Cluster compatibility mode. |
| `--large-memory`           | Enables tests that consume more than 100MB |
| `--tls`                    | Run tests with TLS. See below. |
| `--tls-module`             | Run tests with TLS, when TLS support is built as a module. |
| `--other-server-path PATH` | Run compatibility tests with another server executable. |
| `--help`                   | Displays the full set of options. |

Running with TLS requires the following preparations:

* Build Valkey is TLS support, e.g. using `make BUILD_TLS=yes`, or `make BUILD_TLS=module`.
* Run `./utils/gen-test-certs.sh` to generate a root CA, server certificates, and invalid certificates for testing.
* Install TLS support for TCL, e.g. the `tcl-tls` package on Debian/Ubuntu.

Additional tests
----------------

Not all tests are included in the `./runtest` scripts. Some additional entry points are provided by the following scripts, which support a subset of the options listed above:

* `./runtest-cluster` runs more extensive tests for Valkey Cluster.
  Some cluster tests are included in `./runtest`, but not all.
* `./runtest-sentinel` runs tests of Valkey Sentinel.
* `./runtests-module` runs tests of the module API.

Debugging
---------

You can set a breakpoint and invoke a minimal debugger using the `bp` function.

```
... your test code before break-point
bp 1
... your test code after break-point
```

The `bp 1` will give back the tcl interpreter to the developer, and allow you to interactively print local variables (through `puts`), run functions and so forth [[source](https://wiki.tcl-lang.org/page/A+minimal+debugger)]. 
`bp` takes a single argument, which is `1` for the case above, and is used to label a breakpoint with a string.
Labels are printed out when breakpoints are hit, so you can identify which breakpoint was triggered.
Breakpoints can be skipped by setting the global variable `::bp_skip`, and by providing the labels you want to skip.

The minimal debugger comes with the following predefined functions.
* Press `c` to continue past the breakpoint.
* Press `i` to print local variables.

Writing tests that don't flake
------------------------------

Most test failures in CI are not bugs in the server but tests that assume
something has already happened. The server does its work asynchronously
(replication, cluster gossip, background saves, expiry, lazy free), and CI
runners are slower and noisier than a development machine, especially under
`--valgrind`, the sanitizer builds, TLS and macOS. The checklist below covers
the patterns that have needed a follow-up fix most often, in roughly that order.
Both authors and reviewers should check new or changed tests against it.

1. **Don't use `after` to wait for the server.** `after 1000` is a guess about how
   fast the server is. Poll for the condition instead:

   ```tcl
   wait_for_condition 50 100 {
       [s rdb_bgsave_in_progress] eq 0
   } else {
       fail "bgsave did not finish"
   }
   ```

   `wait_for_condition maxtries delay` checks the condition up to `maxtries`
   times and sleeps `delay` milliseconds after each false result, so the budget
   is at least `maxtries * delay` milliseconds plus the time the checks
   themselves take. Be generous: a budget that passes locally with margin to
   spare can still time out under valgrind, which is often 10x slower. The
   budget only covers false results: an error in the condition (`LOADING` while
   a server restarts or loads an RDB, `BUSY`, a closed connection) ends the wait
   immediately instead of retrying, so wrap a command that can fail in `catch`.

2. **Scale budgets for slow modes.** When a wait is long by nature (a full sync,
   a large migration), scale it: `[expr {$::valgrind ? 7000 : 700}]`. For
   workload sizes, scale the other way down, e.g.
   `set key_count [expr {$::valgrind ? 10000 : 40000}]`.

3. **Don't assert on state that isn't reflected yet.** A value that settles
   asynchronously -- `DBSIZE` after a delete on a replica, `INFO` fields after a
   config change, `CLUSTER` output after a topology change -- is not readable the
   instant the command returns. Put the assertion inside `wait_for_condition`.

4. **Wait for replication and cluster convergence explicitly.** Before asserting
   on a replica or a peer, use the helper that matches what you need:
   `wait_for_ofs_sync`, `wait_replica_online`, `wait_for_sync`,
   `wait_for_slot_state`, `wait_for_cluster_state`, `wait_for_cluster_size`,
   `wait_for_cluster_propagation`, or a poll on `debug digest`. Gossip and
   `PSYNC` are not instant. This includes `valkey-cli --cluster` operations
   (`add-node`, `reshard`, `rebalance`): the CLI returns before gossip has
   finished.

5. **Read related stats atomically.** Two consecutive `INFO` calls can straddle
   background traffic (replication keepalives, expiry). Compare fields from one
   `INFO` reply, or wrap several reads in `MULTI`/`EXEC`.

6. **Give log-message waits a real budget and assert the count.**
   `wait_for_log_messages idx patterns from_line maxtries delay` has the same
   budget arithmetic as `wait_for_condition`. When the test depends on a message
   appearing exactly N times, assert it with `count_log_message` rather than
   assuming the first match is the only one.

7. **Avoid tight bounds on time.** Latency and duration assertions
   (`assert_range` on elapsed time, `LATENCY` output, `usec_per_call`) can fail on
   a loaded runner. Widen the range, or assert on an
   ordering or a lower bound instead of an upper bound. Use TTLs of at least one
   second and margins of at least two seconds when comparing against the clock.

8. **Don't assume a background operation has finished.** `BGSAVE`, `BGREWRITEAOF`,
   active expiry, lazy free and fork children all complete on their own
   schedule. Poll `rdb_bgsave_in_progress`, `aof_rewrite_in_progress`, the
   relevant log line, or the key count.

9. **Clean up, even when the test fails.** Delete the keys you created and
   restore any `CONFIG SET` or override, so a test that fails partway does not
   leak state into the next one. A leaked `rdb-key-save-delay` or
   `repl-diskless-sync-delay` turns every later test in the file into a mystery
   failure. Register the undo inline so it cannot be forgotten and runs on
   failure too: `test_config_set <client> <param> <value>` sets a config for the
   current test and restores the old value when it ends, and `test_defer {...}`
   (or `test_defer_fail {...}`, which runs only on failure) registers any cleanup
   script to run when the test ends. `pause_process` is resumed automatically at
   test end; use `pause_process_persistent` only when a later test depends on the
   process staying paused. `with_config <name> <value> {...}` and
   `with_cleanup {...} {...}` remain for a scoped block inside a test;
   `with_config` always acts on `r`, so for another server such as `$primary`
   use `test_config_set` or `with_cleanup`.

10. **Treat output-buffer-limit tests as racy by construction.** A client can be
    disconnected before or after your next poll depending on when the write
    handler runs, and IO threads widen that window. Loop with a bound and accept
    either "omem exceeded the limit" or "client disconnected" as success.

11. **Synchronize deferred and blocked clients.** With a deferring client
    (`valkey_deferring_client`), `$rd read` the reply before asserting on the
    effect of the command. Before unblocking a blocked client, confirm it is
    blocked with `wait_for_blocked_clients_count`.

12. **Probe for platform features and skip when absent.** tmpfs behavior, MPTCP,
    IPv6, TLS and macOS-specific paths differ between runners. Use the existing
    tags (`tls:skip`, `valgrind:skip`, `ipv6`, `mptcp`) or probe the capability
    at runtime and skip the test rather than asserting on a platform assumption.
    Valgrind in particular has no 80-bit floating point: `strtold("+inf")` can
    return a finite value, so a test that expects an Infinity or overflow error
    from a float command passes natively and fails under valgrind. Tag those
    `valgrind:skip` (see `INCRBYFLOAT`/`HINCRBYFLOAT` tests in
    `tests/unit/type/incr.tcl` and `hash.tcl`).

13. **Make failover tests deterministic.** Automatic election rank depends on
    replication offset and timing, so don't assert which replica wins unless
    you have made it the only candidate. To choose a winner, run
    `CLUSTER FAILOVER` on that replica (`FORCE` when the primary is down,
    `TAKEOVER` when there is no quorum); manual failover bypasses rank, delay
    and `cluster-replica-no-failover`. To keep replicas out of an automatic
    election, set `cluster-replica-no-failover yes` on them.
    `cluster-replica-validity-factor 0` only keeps a stale replica eligible;
    it does not choose which one wins.

14. **Do enough work to trigger what you are testing.** A test that writes 20
    keys to exercise a path that triggers at 100 passes by luck. Size the
    workload so the behavior is forced, not probable.

15. **Don't sleep for `serverCron`.** Behavior driven by the cron (query buffer
    resize, dict rehash and resize, LRU/LFU touch, client eviction) runs at `hz`.
    Either freeze it during setup with `DEBUG PAUSE-CRON 1` and release it, or
    raise `hz`, then `wait_for_condition` on the state the cron produces.

16. **Stop replication PINGs before measuring offsets or network stats.** The
    primary pings replicas every `repl-ping-replica-period` seconds, which moves
    `master_repl_offset` and the `total_net_*` counters under your assertion.
    Raise the period on the primary for the duration of the measurement. This
    holds the offset still, but replicas keep sending `REPLCONF ACK` every
    second regardless, so the `total_net_*` counters keep moving: allow for
    that, or measure without replicas attached.

17. **Wait for a closed client to be freed.** `CLIENT KILL`, closing a connection,
    and dropping a `WATCH`ing client all take effect after the command returns.
    Poll `connected_clients`, `watching_clients`
    (`wait_for_watched_clients_count`) or `blocked_clients` rather than asserting
    on the very next command.

18. **Sentinel: wait for the Sentinel timer, not for your command.** After
    `SENTINEL SET`, `SENTINEL RESET`, or shrinking `down-after-milliseconds`,
    `failover-timeout` or the tilt period for test speed, the effect lands on the
    next Sentinel timer tick and INFO exchange. Wait for the resulting state
    (SDOWN flag, new leader, reconnected instance) with a generous budget rather
    than acting immediately.

When a test does flake in CI, fix the synchronization rather than retrying; the
next slower runner will find the same gap.

Tags
----

Tags are applied to tests to classify them according to the subsystem they test,
but also to indicate compatibility with different run modes and required
capabilities.

Tags can be applied in different context levels:
* `start_server` context
* `tags` context that bundles several tests together
* A single test context.

Some tags are restricted to top-level use only. These tags are `large-memory`, `needs:other-server`, `compatible-redis` and `network`.

The following compatibility and capability tags are currently used:

| Tag                       | Indicates |
| ------------------------- | --------- |
| `external:skip`           | Not compatible with external servers. |
| `cluster`                 | Uses cluster with multiple nodes. |
| `cluster:skip`            | Not compatible with `--cluster-mode`. |
| `large-memory`            | Test that requires more than 100MB |
| `tls`                     | Uses TLS. |
| `tls:skip`                | Not compatible with `--tls`. |
| `ipv6`                    | Uses IPv6. |
| `needs:repl`, `repl`      | Uses replication and needs to be able to `SYNC` from server. |
| `needs:debug`             | Uses the `DEBUG` command or other debugging focused commands (like `OBJECT REFCOUNT`). |
| `needs:pfdebug`           | Uses the `PFDEBUG` command. |
| `needs:config-maxmemory`  | Uses `CONFIG SET` to manipulate memory limit, eviction policies, etc. |
| `needs:config-resetstat`  | Uses `CONFIG RESETSTAT` to reset statistics. |
| `needs:reset`             | Uses `RESET` to reset client connections. |
| `needs:save`              | Uses `SAVE` or `BGSAVE` to create an RDB file. |
| `needs:other-server`      | Requires `--other-server-path`. |
| `compatible-redis`        | Tests that run against Redis (compatibility tests). |
| `network`                 | Tests that require network operations. |
| `singledb`                | Test runs as if `--singledb` was given. |
| `valgrind:skip`           | Not compatible with `--valgrind`. |

When using an external server (`--host` and `--port`), filtering using the
`external:skip` tags is done automatically.

When using `--valgrind`, filtering using the `valgrind:skip` tag is done
automatically.

When using `--cluster-mode`, filtering using the `cluster:skip` tag is done
automatically.

When not using `--large-memory`, filtering using the `largemem:skip` tag is done
automatically.

In addition, it is possible to specify additional configuration. For example, to
run tests on a server that does not permit `SYNC` use:

    ./runtest --host <host> --port <port> --tags -needs:repl

