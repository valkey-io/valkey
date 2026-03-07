# Design Document: Atomic Slot Migration

## 1. Overview

Atomic Slot Migration (ASM) provides a seamless, atomic method for migrating
hash slots between nodes in a Valkey cluster. This mechanism replaces
`CLUSTER SETSLOT IMPORTING/MIGRATING` and `MIGRATE` for migrating slots between
nodes.

## 2. Core Mechanics

Rather than migrating data key-by-key, ASM operates at the slot level by
adapting existing replication and failover primitives:

- **Slot-Based Replication:** Physical data migration borrows from
  Primary-Replica replication mechanisms, but strictly scopes to the specific
  slots being moved.
- **Atomic Ownership Transfer:** ASM executes the final handover of slot
  ownership using a coordinated process similar to a Manual Failover, ensuring
  an atomic transfer.
- **Traffic Handling:** Throughout the migration process, the source node
  retains the data and continues to actively serve business requests. The system
  cleanly cuts over traffic to the target node only after completing the atomic
  transfer.

## 3. Implementation Details

### 3.1 High Level Overview

1. **Snapshot Transfer:** The source node transfers data to the target node. The
   source node forks a child process to iterate and serialize the slot's keys.

   The source transfers data in "AOF" (Append Only File) format to the target
   node. This format consists of a stream of commands. Consequently, the target
   primary and target replica replay these commands to restore the slot's state.

2. **Incremental Updates:** While transferring the initial snapshot, the source
   node serves business requests. The source node records any changes to the
   slot's keys during this time and sends them to the target node as incremental
   updates after completing the snapshot transfer.
3. **Pause:** After sending the incremental updates, the source node pauses.
   Consequently, the source node rejects any further business requests. This
   pause ensures the target node maintains the same state as the source node.
4. **Failover:** After the source node pauses, the target node becomes the
   primary node for the slot.
5. **Clean Up:** After the target node becomes the primary node for the slot,
   the source node receives this information via cluster gossip. The source node
   then unpauses, removes the keys from the slot, and completes the slot
   migration.

### 3.2 CLUSTER SYNCSLOTS

The source, target, and target replica use the `CLUSTER SYNCSLOTS` command to
coordinate the handover state:

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

Various scenarios cause slot migration failure:

1. Link between source and target disconnects
2. Source or target node crash, halt, or encounter a partition
3. A failover occurs on the source or target node
4. Out of memory error occurs on the target node
5. Client output buffer on the source node grows too large
6. An administrator executes `FLUSHDB` on the source or target node

In such cases, ASM automatically rolls back the migration.

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

The cluster automatically cleans up failed or cancelled slot migrations. The
primary is solely responsible for cleaning up unowned slots. Primaries demoted
during migration do not clean up previously active slot imports. The promoted
replica is responsible for both cleaning up the slot and sending a
`SYNCSLOTS FINISH`.

### 3.4 Key Containment

The system rejects any keyed command executed on a node that is not the primary
for that slot with `-MOVED` (e.g. `GET`, `SET`, `DEL`, `INCR`, etc).

Nodes filter unkeyed read commands, like `SCAN` and `KEYS`, to avoid exposing
importing slot data. Each node in the target shard tracks the slot migration job
state and hides writes to that slot from the end user until the migration
completes.

#### 3.4.1 Full Sync, Partial Sync, RDB

To ensure replicas resyncing during the import remain aware of the import,
Valkey serializes the slot import to an RDB aux field (`cluster-slot-imports`).
The encoding includes the job name, the source node name, and the slot ranges
being imported. Upon loading an RDB with the cluster-slot-imports aux field,
replicas begin tracking the migration.

Whenever the system loads an RDB file with the `cluster-slot-imports` aux field,
even from disk, it adds a new migration to track the import. If the Valkey node
is a primary after loading the RDB, it cancels the slot migration.

Loading this tracking state on primaries ensures that replicas partially syncing
to a restarted primary still get their `SYNCSLOTS FINISH` message in the
replication stream.

#### 3.4.2 AOF

Valkey propagates the `ESTABLISH` and `FINISH` commands to the AOF, ensuring
they replay properly on AOF load. Similar to RDB, if any pending `ESTABLISH`
commands lack a subsequent `FINISH` upon becoming primary, the system fails them
after loading.

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
