# Design Document: Sync From Replica

## Overview

A full sync is expensive for the node that serves it. That node forks, pays
copy-on-write memory, and produces and sends an RDB. Today that node is always
the primary, so adding a replica disturbs the node that serves the writes.

With `cluster-prefer-sync-from-replica yes`, a replica that joins a shard takes
its snapshot from an existing replica of the same primary. It then attaches to
the primary with a partial resync. The primary does not fork or produce an RDB
for the new replica.

The option is off by default and best effort. When the new replica cannot use
another replica, it does a normal full sync from the primary.

This document uses three names:

- **P**: the primary of the shard.
- **S**: the donor, an existing replica of P. The code and the logs call it the
  sibling.
- **N**: the new replica.

## How it works

```text
        N                            S                            P
        |                            |<==== replication stream ===|
        |                            |                            |
  CLUSTER REPLICATE P                |                            |
  (topology: N is a replica of P)    |                            |
        |                            |                            |
        |-- REPLCONF sibling-sync -->|                            |
        |-- PSYNC ------------------>|                            |
        |<-- +FULLRESYNC, RDB -------|  (S forks)                 |
        |<-- P's stream, forwarded --|                            |
        |                            |                            |
  (N has caught up)                  |                            |
        |-- close ------------------>|                            |
        |-- PSYNC <replid> <offset> ----------------------------->|
        |<-- +CONTINUE, stream -----------------------------------|
```

1. **Join.** `CLUSTER REPLICATE <P>` makes N a replica of P in the cluster
   topology, as it does today. Only N's replication connection goes to S. The
   topology shows N as a replica of P from start to end. S never appears in it
   as N's primary.
2. **Snapshot from S.** N chooses S and runs an ordinary full sync from it.
   It is the same full sync a replica runs from a primary, and it uses
   dual-channel replication when N and S both have it on.
3. **Catch-up through S.** S forwards P's replication stream to N, as a replica
   does for any replica attached to it. N applies the stream until it reaches
   the offset it knew for P when the snapshot finished loading.
4. **Switch to P.** N closes the connection to S and sends `PSYNC` to P with
   the replication ID and offset it now has. P continues from its backlog.

In step 3, N also waits for at least one command after the snapshot. On an idle
shard that command is P's periodic ping, so the switch can come up to
`repl-ping-replica-period` after the load.

## Design decisions

### The switch to P is a partial resync

S forwards P's stream byte for byte, so S has P's replication ID and the same
offsets. N takes both from S. When N sends `PSYNC` to P, P sees its own
replication ID and an offset that is in its backlog, and continues from there.

### N catches up through S before it switches

P keeps taking writes while the snapshot is produced, sent and loaded. Those
writes have to be kept somewhere until N can apply them. Only two places can
keep them: P's backlog, or S's stream to N.

If N switched right after the load, P's backlog would have to cover the whole
snapshot window. On a busy primary with the default backlog size it does not,
and P would then do a full sync, after S already did one.

So N stays on S until it has caught up. S keeps the writes for N the way any
node does for a replica it is syncing, under the existing limits such as
`client-output-buffer-limit replica`. P's backlog only has to cover the switch
itself.

### The connection to S is temporary

N ends as a regular replica of P and does not stay behind S. P only counts and
drives the replicas that are connected to it: `WAIT`, `min-replicas-to-write`
and manual failover all use that connection.

### The cluster is not told that N syncs from S

The cluster topology records which primary a replica belongs to. For N that is
P from the join to the end, and it never changes. Which node N's replication
connection goes to during the sync is the state of that connection. The cluster
bus does not carry such state today either: a replica in a full sync from its
primary is gossiped as a normal replica of that primary.

Only two nodes act on the sync. N starts it, and S is asked in the replication
handshake. No other node has a decision to make from it, so there is nothing
for gossip to deliver. Telling the cluster could take two forms:

- **Gossip N as a replica of S.** This states that N belongs to S, which is
  not true, and the cluster acts on it. `CLUSTER REPLICATE` rejects a replica
  as the target, and a node whose primary turns into a replica moves itself to
  that node's primary. Every node also saves the relation in `nodes.conf`, so
  an N that restarts in the middle of the sync comes back as a replica of S.
  The move back to P would be a second topology change for the whole cluster.
- **Add a "syncing from" field to the cluster bus.** N stays a replica of P,
  and the field names S. Every node has to carry and parse a field that none
  of them uses.

Keeping it on N means the state lives only in N's memory. Nothing is written
to `nodes.conf`, no other node has state to clean up when the sync fails, and
nodes on older versions see an ordinary replica of P.

The cost is that `CLUSTER NODES` does not show the sync. It is visible in
`INFO replication`: on N through the two new fields, and on S, which lists N
among its connected replicas.

### Nothing new on P

P gets no new command and keeps no state for N. N does not replicate from P
until the switch, and the switch is an ordinary `PSYNC`. All of the new logic
runs on N and S.

## Choosing a donor

N chooses the donor, and the donor has to agree.

**N chooses.** When it handles `CLUSTER REPLICATE`, N looks at P's replicas in
its own view of the cluster. It skips the nodes flagged `FAIL` or `PFAIL` and
the nodes with replication offset 0. From the rest it takes the one with the
highest replication offset. The choice is deterministic, so replicas that join
together normally choose the same donor.

**S agrees.** Replication offsets cannot show that a replica's data differs
from P's. A writable replica can take local writes, and a replica that enforces
`maxmemory` can evict keys. So N asks in the handshake, with
`REPLCONF sibling-sync <node ID of P>`. S accepts only when all of these hold:

- S is a replica of that P, and its link to P is up.
- S has `replica-read-only yes` and `replica-ignore-maxmemory yes`.
- S got its own dataset from a full sync, from P or from a donor that passed
  this same check, while those two settings were on.
- S is not itself in the middle of a sync from a donor.

S stops being a donor when one of the two settings is turned off, when its
replication target changes, and when it restarts. Turning a setting off also
disconnects the replicas that S is serving as a donor. Only a new full sync
makes S a donor again. Turning the settings back on does not, and neither does
a partial resync.

When S refuses, or runs a version that does not know the option, N does a full
sync from P. N does not try a second donor.

## Failures

Until the switch, a failure sends N to a normal full sync from P, which is what
happens without this feature.

| Event | Result |
|---|---|
| No donor is found, S refuses, S fails, or the link to S drops | N does a full sync from P. |
| N gets a different primary, is promoted, or is reset | N drops the sync from S and follows the new topology. |
| P fails while N syncs from S | N stays on S. After the failover, N syncs from the new primary. |
| At the switch, P's backlog no longer holds N's offset | P does a full sync. |
| N restarts | The state is not persisted. N does a normal sync from P. |

N cannot become a primary while it syncs from S, because its dataset is not
complete. It does not start an election, and `CLUSTER FAILOVER` returns an
error, also with `FORCE` and `TAKEOVER`.

## User-visible interfaces

- **Configuration:** `cluster-prefer-sync-from-replica yes|no`. The default is
  `no`, and it can be changed at runtime. It is set on the joining replica. A
  donor needs no configuration. The option has an effect only in cluster mode.
- **`INFO replication` on N:** `sync_from_replica_in_progress` is 1 during the
  sync from S. `sync_from_replica_phase` is one of `handshake`,
  `rdb_transfer`, `rdb_loading`, `stream_catchup` and `none`.
- **`CLUSTER FAILOVER` on N:** returns an error during the sync from S.

## Compatibility

- **P:** needs no change. It receives an ordinary handshake and `PSYNC`.
- **S on an older version:** rejects the unknown handshake option, and N does a
  full sync from P.
- **Chained replication:** a replica that replicates from another replica
  outside this feature keeps its handshake and its behavior.
- **With the option off:** one thing changes. A dual-channel RDB connection no
  longer joins a disk-based BGSAVE that is already running. It waits for the
  next one. This makes the offset it gets always match its snapshot.

## Performance

P, S, N and the load generator each run in their own pod on a dedicated
c7g.2xlarge node (8 vCPU, 16 GiB) on AWS EKS. P holds 5 million keys with 1 KB
values. The load is 1 million `SET` commands with 1 KB values from 50 clients.
N starts its sync after a 10 second warm-up, so the fork falls inside the
measured window. Each case ran 3 times, with fresh pods for each run.

| | N syncs from P | N syncs from S |
|---|---:|---:|
| Average `SET` rate before the sync | 100,855/s | 105,511/s |
| Average `SET` rate during the sync | 95,082/s | 104,702/s |
| Max `SET` latency during the sync, median run | 89.1 ms | 4.1 ms |
| Max `SET` latency during the sync, worst run | 90.6 ms | 8.2 ms |
| Forks on P for N | 1 in each run | 0 |
| Forks on S for N | 0 | 1 in each run |

The numbers are from an earlier revision of this change. The results of each
run are in
[this comment](https://github.com/valkey-io/valkey/issues/2767#issuecomment-4198194426).

## References

- **Issue:** [#2767](https://github.com/valkey-io/valkey/issues/2767) has the
  requirements, the alternatives that were considered and the follow-up work.
- **Code:** [replication.c](../src/replication.c) has the sync from S, the
  switch to P and the donor check.
  [cluster_legacy.c](../src/cluster_legacy.c) has the donor choice in
  `CLUSTER REPLICATE` and the failover checks.
- **Corner cases:** read the test cases in
  [sync-from-replica.tcl](../tests/unit/cluster/sync-from-replica.tcl).
