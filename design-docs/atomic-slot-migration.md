# Design Document: Atomic Slot Migration

## 1. Overview

Atomic Slot Migration (ASM) provides a seamless, atomic method for migrating
hash slots between nodes in a Valkey cluster. It provides an alternative
mechanism to `CLUSTER SETSLOT IMPORTING/MIGRATING` and `MIGRATE` for migrating
slots between nodes.

## 2. Core Mechanics

Rather than migrating data on a key-by-key basis, ASM operates at the slot level
by adapting existing replication and failover primitives at the slot level:

- **Slot-Based Replication:** The physical data migration borrows from
  Primary-Replica replication mechanisms, but is strictly scoped to the specific
  slots being moved.
- **Atomic Ownership Transfer:** The final handover of slot ownership is
  executed using a coordinated process similar to a Manual Failover, ensuring
  the transfer is atomic.
- **Traffic Handling:** Throughout the migration process, the source node
  retains the data and continues to actively serve business requests. Traffic is
  cleanly cut over to the target node only once the atomic transfer is complete.

## 3. Implementation Details

### 3.1 High Level Overview

1. **Snapshot Transfer:** The data transfer is performed by the source node to
   the target node. A child process is forked on the source node to perform the
   iteration and serialization of the slot's keys.

   Data is transferred in "AOF" (Append Only File) format to the target node.
   This means it is just a stream of commands. For this reason, they can be
   replayed on both the target primary and the target replica to restore the
   state of the slot.

2. **Incremental Updates:** While the initial snapshot is transferred, the
   source node continues to serve business requests. Any changes to the slot's
   keys during this time are recorded and sent to the target node as incremental
   updates once the snapshot transfer is complete.
3. **Pause:** Once the incremental updates are sent to the target node, the
   source node is paused. This means that the source node does not accept any
   more business requests. This is done to ensure that the target node has the
   same state as the source node.
4. **Failover:** Once the source node is paused, the target node is promoted to
   the primary node for the slot.
5. **Clean Up:** Once the target node is promoted to the primary node for the
   slot, the source node hears about this over cluster gossip. The source node
   then unpauses and removes the keys from the slot and the slot migration is
   complete.

### 3.2 CLUSTER SYNCSLOTS

The `CLUSTER SYNCSLOTS` command is used between the source, target, and target
replica to coordinate the state of the handover:

```
     Source                                          Target                         Target Replica
       |                                                |                                 |
       |------------ SYNCSLOTS ESTABLISH -------------->|                                 |
       |                                                |----- SYNCSLOTS ESTABLISH ------>|
       |<-------------------- +OK ----------------------|                                 |
       |                                                |                                 |
       |~~~~~~~~~~~~~~ snapshot as AOF ~~~~~~~~~~~~~~~~>|                                 |
       |                                                |~~~~~~ forward snapshot ~~~~~~~~>|
       |----------- SYNCSLOTS SNAPSHOT-EOF ------------>|                                 |
       |                                                |                                 |
       |<----------- SYNCSLOTS REQUEST-PAUSE -----------|                                 |
       |                                                |                                 |
       |~~~~~~~~~~~~ incremental changes ~~~~~~~~~~~~~~>|                                 |
       |                                                |~~~~~~ forward changes ~~~~~~~~~>|
       |--------------- SYNCSLOTS PAUSED -------------->|                                 |
       |                                                |                                 |
       |<---------- SYNCSLOTS REQUEST-FAILOVER ---------|                                 |
       |                                                |                                 |
       |---------- SYNCSLOTS FAILOVER-GRANTED --------->|                                 |
       |                                                |                                 |
       |                                            (performs takeover &                  |
       |                                             propagates topology)                 |
       |                                                |                                 |
       |                                                |------- SYNCSLOTS FINISH ------->|
 (finds out about topology                              |                                 |
  change & marks migration done)                        |                                 |
       |                                                |                                 |
```

See code comments in [cluster_migrateslots.c](../src/cluster_migrateslots.c) for
detailed state machines.

### 3.3 Automatic Rollback

Various scenarios may result in slot migration failure:

1. Link between source and target is destroyed or unresponsive
2. Source or target node crash, halt, or are partitioned
3. A failover occurs on the source or target node
4. Out of memory error occurs on the target node
5. Client output buffer on the source node grows too large
6. `FLUSHDB` is executed on the source or target node

In such cases, the slot migration is automatically rolled back.

```
     Source                                          Target                         Target Replica
       |                                                |                                 |
       |------------ SYNCSLOTS ESTABLISH -------------->|                                 |
       |                                                |----- SYNCSLOTS ESTABLISH ------>|
       |<-------------------- +OK ----------------------|                                 |
     ...                                              ...                               ...
       |                                                |                                 |
       |                                             <FAILURE>                            |
       |                                                |                                 |
       |                                      (performs cleanup)                          |
       |                                                | ~~~~~~ UNLINK <key> ... ~~~~~~~>|
       |                                                |                                 |
       |                                                | ------ SYNCSLOTS FINISH ------->|
       |                                                |                                 |
```

#### 3.3.1 Cleanup

Slot migrations are automatically cleaned up when the slot migration is failed
or cancelled. The primary is solely responsible for cleaning up unowned slots.
Primaries that are demoted do not clean up previously active slot imports. The
promoted replica is responsible for both cleaning up the slot and sending a
`SYNCSLOTS FINISH`.

### 3.4 Key Containment

Any keyed command that is executed on a node that is not the primary for that
slot are rejected with `-MOVED` (e.g. `GET`, `SET`, `DEL`, `INCR`, etc).

Unkeyed read commands, like `SCAN` and `KEYS`, are filtered to avoid exposing
importing slot data. Each node in the target shard tracks the state of the slot
migration job and ensures writes to that slot are not shown to the end user
until the slot migration is complete.

#### 3.4.1 Full Sync, Partial Sync, RDB

In order to ensure replicas that resync during the import are still aware of the
import, the slot import is serialized to an RDB aux field
(`cluster-slot-imports`). The encoding includes the job name, the source node
name, and the slot ranges being imported. Upon loading an RDB with the
cluster-slot-imports aux field, replicas begin tracking the migration.

Whenever we load an RDB file with the `cluster-slot-imports` aux field, even
from disk, we add a new migration to track the import. If after loading the RDB,
the Valkey node is a primary, it cancels the slot migration. We do not continue
slot migrations that were in progress at the time of save.

Having this tracking state loaded on primaries ensures that replicas partial
syncing to a restarted primary still get their `SYNCSLOTS FINISH` message in the
replication stream.

#### 3.4.2 AOF

We propagate the `ESTABLISH` and `FINISH` commands to the AOF, and ensure that
they can be properly replayed on AOF load to get to the right state. Similar to
RDB, if there are any pending `ESTABLISH` commands that don't have a `FINISH`
afterwards upon becoming primary, we fail those after loading.

## 4. External References

- **API & User Commands:**
  - [CLUSTER MIGRATESLOTS](https://valkey.io/commands/cluster-migrateslots/)
  - [CLUSTER GETSLOTMIGRATIONS](https://valkey.io/commands/cluster-getslotmigrations/)
  - [CLUSTER CANCELSLOTMIGRATIONS](https://valkey.io/commands/cluster-cancelslotmigrations/)
  - [CLUSTER SYNCSLOTS](https://valkey.io/commands/cluster-syncslots/)
- **Corner Cases:** Read the test cases in
  [cluster-migrateslots.tcl](../tests/unit/cluster/cluster-migrateslots.tcl)
- **PR References:** For further reading, refer to the following Pull Requests:
  - [PR #1949](https://github.com/valkey-io/valkey/pull/1949)
  - [PR #2755](https://github.com/valkey-io/valkey/pull/2755)
  - [PR #2593](https://github.com/valkey-io/valkey/pull/2593)
  - [PR #2635](https://github.com/valkey-io/valkey/pull/2635)
