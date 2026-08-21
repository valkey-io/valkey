# Pause Replica Full Synchronization

## Purpose

Add an operator command that can stop an in-progress replica synchronization before memory pressure causes the replica to be killed. The operation must preserve the configured primary and replica role, prevent automatic retry until explicitly resumed, and abort diskless RDB loading without closing a connection that is still owned by the RDB loader.

## Command interface

Add a new server command with two forms:

```text
REPLSYNC PAUSE
REPLSYNC RESUME
```

The command is local administrative control. It is not replicated or persisted. It is available in standalone and cluster mode because it does not change topology.

`REPLSYNC PAUSE` is valid only on a configured replica that is not already online. It is idempotent when already paused. If the primary link is online, it returns an error because interrupting an established replication stream is outside this feature's scope.

`REPLSYNC RESUME` is valid only on a configured replica. It is idempotent when synchronization is not paused. When paused, it resumes connection attempts immediately.

Both forms return `OK` on success. The command is classified as administrative, dangerous, unavailable from scripts or transactions, and allowed while the replica is stale or synchronously loading an RDB.

## Replication state

Add `REPL_STATE_PAUSED` to the replica replication states. This state means that `primary_host` remains configured but replication cron must not connect. It is distinct from `REPL_STATE_NONE`, which represents an instance without active replication configuration.

Add a `repl_sync_paused` flag that records the requested policy while cancellation may still be unwinding. Once cancellation completes, the state becomes `REPL_STATE_PAUSED`; the flag remains set until resume. Changing or removing the configured primary clears the flag so a topology operation cannot inherit a stale local pause.

`ROLE` reports the replica link state as `paused`. `INFO replication` adds:

```text
master_sync_paused:0|1
```

## Cancellation paths

For connecting, handshake, and transfer states where no RDB load is active, pause calls the existing `cancelReplicationHandshake(0)` cleanup and then leaves the replica in `REPL_STATE_PAUSED`.

For synchronous or swapdb diskless RDB loading, the command must not close the replication connection from the nested event loop. It sets the pause flag and calls `rioCloseASAP(server.loading_rio)`. The next RDB read fails normally, existing RDB cleanup discards the partial or temporary dataset, and `cancelReplicationHandshake()` completes connection cleanup. Cancellation observes the pause flag and transitions to `REPL_STATE_PAUSED` without reconnecting, even if its caller requested an immediate reconnect.

For disk-based transfer handled by the BIO thread, the existing cancellation path signals and drains the worker before cleaning up the temporary RDB. Its existing maximum wait of `repl_syncio_timeout` remains unchanged.

## Resume path

Resume clears the pause flag. If the state is `REPL_STATE_PAUSED`, it changes the state to `REPL_STATE_CONNECT` and invokes `connectWithPrimary()` immediately. If an RDB abort is still unwinding, clearing the flag allows the existing failure path to reconnect after cleanup.

## Error handling

- A primary returns an error for both subcommands.
- Pause returns an error when the replica is already online.
- Invalid subcommands or arity return the standard syntax or arity error.
- Repeated pause and resume operations are idempotent.
- Normal timeout and connection-error cancellation behavior is unchanged when no pause was requested.

## Files and generated command metadata

- `src/server.h`: replication state and pause field.
- `src/server.c`: initialization and INFO output.
- `src/replication.c`: command handler, pause-aware cancellation, resume logic, role output, and topology-reset behavior.
- `src/commands/replsync.json`: public command metadata and flags.
- `src/commands/role.json`: paused-state reply metadata.
- Generated command tables updated through the repository's normal command-generation target.
- `tests/integration/replication.tcl`: command and state-machine coverage.

## Testing

Integration tests cover:

1. Rejection on a primary.
2. Pausing a replica during handshake or full-sync transfer.
3. Verifying no reconnect occurs while paused.
4. Verifying `ROLE` and `INFO replication` expose the paused state.
5. Resuming and reaching the online state.
6. Pausing during diskless RDB loading and confirming safe cleanup without a crash or automatic retry.
7. Idempotent pause and resume behavior.
8. Existing replication tests to confirm ordinary failure-driven reconnect behavior is unchanged.

Targeted tests run first, followed by the relevant replication integration file and a no-optimization TLS build.
