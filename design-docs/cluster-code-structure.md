# Cluster Code Structure

This document describes the structure of the cluster subsystem, which is
designed to support multiple cluster bus protocol implementations through
a vtable-based dispatch mechanism.

## Layering

```
┌──────────────────────────────────────────────────────────────────┐
│                        server.c / commands                       │
│              (calls clusterInit, clusterCron, etc.)              │
└──────────────────────────────┬───────────────────────────────────┘
                               │
┌──────────────────────────────▼───────────────────────────────────┐
│                          cluster.c                               │
│         Command dispatch, vtable dispatchers, common init,       │
│         CLUSTER SHARDS/SLOTS, state evaluation, RDB aux fields   │
├─────────────┬───────────────┬───────────────┬────────────────────┤
│cluster_nodes│ cluster_state │ cluster_link  │cluster_migrateslots│
│  .c / .h    │   .c / .h     │   .c / .h     │    .c / .h         │
│             │               │               │                    │
│ nodes.conf  │ clusterNode,  │ Link lifecycle│ Atomic slot        │
│ load/save,  │ clusterState, │ accept/connect│ migration          │
│ CLUSTER     │ slot assign,  │ read/write,   │ (SYNCSLOTS)        │
│ NODES text, │ node creation,│ send blocks,  │                    │
│ aux fields  │ iterators,    │ buffer mgmt   │                    │
│             │ dict types    │               │                    │
└──────┬──────┴───────────────┴───────┬───────┴────────────────────┘
       │                              │
       │    ┌─────────────────────────▼──────────┐
       │    │         cluster_bus.h              │
       │    │   clusterBusType vtable interface  │
       │    │   (validateMessageHeader,          │
       │    │    processMessage, postConnect,    │
       │    │    slotChange, forgetNode, ...)    │
       │    └─────────────┬──────────────────────┘
       │                  │
┌──────▼──────────────────▼───────────────────────────────────────┐
│                    cluster_legacy.c                             │
│                                                                 │
│  Gossip protocol implementation (clusterLegacyBus):             │
│  - Message building & processing (PING, PONG, MEET, FAIL, ..)   │
│  - Failure detection (PFAIL/FAIL voting)                        │
│  - Epoch management (configEpoch, currentEpoch)                 │
│  - Replica migration, manual failover state machine             │
│  - clusterLegacyState, clusterNodeLegacyData                    │
│  - BUMPEPOCH, SET-CONFIG-EPOCH (via specialCommand)             │
└─────────────────────────────────────────────────────────────────┘
```

## File Overview

### Common layer (protocol-agnostic)

- **cluster.c / cluster.h** — Command dispatch (`clusterCommand`), common
  initialization, cluster state evaluation, CLUSTER SHARDS/SLOTS reply
  helpers, open slot RDB aux field encoding, and vtable dispatchers for
  lifecycle operations (init, cron, beforeSleep, shutdown).

- **cluster_bus.h** — Defines `clusterBusType`, the vtable interface that
  protocol implementations must provide. The global `clusterCurrentBus`
  pointer selects the active implementation.

- **cluster_state.c / cluster_state.h** — Low-level cluster state
  management: node struct (`clusterNode`), node accessors, node creation,
  slot assignment (`clusterAddSlot`, `clusterDelSlot`), shard management,
  dict types, `ClusterNodeIterator`, and `clusterUpdateState` evaluation.
  Also defines the global `myself` pointer.

- **cluster_nodes.c / cluster_nodes.h** — Node serialization and
  persistence: `nodes.conf` loading/saving/locking, `CLUSTER NODES` text
  representation (`clusterGenNodeDescription`, `clusterGenNodesDescription`),
  aux field handlers for node properties, slot info pair generation, and
  `representClusterNodeFlags`.

- **cluster_link.c / cluster_link.h** — Cluster transport layer: link
  lifecycle (`createClusterLink`, `freeClusterLink`), connection handlers
  (`clusterAcceptHandler`, `clusterLinkConnectHandler`, `clusterReadHandler`),
  write handler, send block management (`clusterLinkSendBlock`,
  `clusterMsgSendBlockDecrRefCount`), buffer limit enforcement, and
  `nodeIp2String`. The read handler dispatches to vtable callbacks for
  message validation and processing.

- **cluster_migrateslots.c / cluster_migrateslots.h** — Atomic slot
  migration (SYNCSLOTS protocol). Operates over regular client connections,
  not the cluster bus.

- **cluster_slot_stats.c / cluster_slot_stats.h** — Per-slot CPU and
  network statistics.

### Legacy gossip protocol implementation

- **cluster_legacy.c** — Implementation of the gossip-based cluster bus
  protocol. Provides the `clusterLegacyBus` vtable instance. Contains
  gossip message building and processing, failure detection, epoch
  management, replica migration, manual failover state machine, and all
  gossip-specific state (`clusterLegacyState`, `clusterNodeLegacyData`).

## vtable Interface (`clusterBusType`)

The vtable is organized into groups:

### Lifecycle

- `init` — Allocate protocol-specific state (`protocol_data`).
- `initLast` — Start listening for cluster bus connections.
- `cron` — Periodic protocol tasks (heartbeats, failure detection).
- `beforeSleep` — Deferred actions before the event loop sleeps.
- `handleServerShutdown` — Graceful shutdown (save config, unlock).

### Message handling (cluster_link.c)

- `validateMessageHeader` — Validate the first bytes of an incoming
  message and return the total message length (0 if invalid).
- `processMessage` — Process a complete message from `link->rcvbuf`.
- `postConnect` — Called after an outbound link connection is established
  (e.g. send initial PING).

### Config updates

- `updateMyselfFlags`, `updateMyselfIp`, `updateMyselfHostname`,
  `updateMyselfAnnouncedPorts`, `updateMyselfHumanNodename`,
  `updateMyselfClientIpV4`, `updateMyselfClientIpV6`,
  `updateMyselfAvailabilityZone` — Called when server config changes.

### Message propagation

- `propagatePublish` — Propagate pub/sub messages to peers.
- `sendModuleMessage` — Send module messages to a target node.

### Failover

- `manualFailoverTimeLimit` — Return the manual failover deadline.
- `resetManualFailoverState` — Clean up manual failover state.
- `resetAutomaticFailoverState` — Reset election state.

### Info and stats

- `getConnectionsCount` — Number of active cluster bus connections.
- `resetStats` — Reset protocol-specific statistics.
- `appendInfoFields` — Append protocol-specific fields to CLUSTER INFO.
- `getFailureReportsCount` — Failure report count for a node.

### Node serialization (cluster_nodes.c)

- `getNodePingPongEpoch` — Return per-node ping/pong/epoch for
  CLUSTER NODES output and nodes.conf.
- `setNodePingPongEpoch` — Set per-node timing during config load.
- `setNodeFailed` — Mark a node as failed during config load.
- `appendVarsLine` — Append protocol-specific vars to nodes.conf.
- `parseVarsLine` — Parse a protocol-specific variable during load.
- `postLoad` — Post-load fixups (e.g. epoch consistency).
- `initNodeData` — Allocate protocol-specific per-node data.

### Slot ownership

- `slotChange` — Assign or unassign slots with a completion callback.
  The callback is invoked when the change is applied (synchronously
  for gossip, potentially after consensus for other protocols).

### Node management commands

These use a completion callback pattern: the vtable performs the
protocol-specific action and calls the callback when done. The callback
receives `void *ctx` (typically the client) and `const char *error`
(NULL on success). This allows consensus-based implementations to block
the client until the change is committed.

- `forgetNode` — Remove a node from the cluster.
- `setReplicaOf` — Set replication target or promote to primary.
- `failover` — Initiate manual failover.
- `meet` — Initiate a handshake with a new node.
- `resetCluster` — Reset the cluster (soft or hard).

### Protocol-specific commands

- `specialCommand` — Handle protocol-specific CLUSTER subcommands
  (e.g. BUMPEPOCH, SET-CONFIG-EPOCH for gossip). Returns 1 if handled,
  0 if not recognized.

## Protocol-specific state

Both `clusterState` and `clusterNode` contain a `void *protocol_data`
field that protocol implementations use to attach their own state.

```
clusterState (cluster_state.h)              clusterNode (cluster_state.h)
┌──────────────────────────┐                ┌──────────────────────────┐
│ myself                   │                │ name, shard_id, flags    │
│ state, fail_reason, size │                │ slots[], numslots        │
│ nodes (dict)             │                │ ip, ports, hostname      │
│ shards (dict)            │                │ replicaof, replicas      │
│ migrating_slots_to       │                │ link, inbound_link       │
│ importing_slots_from     │                │ repl_offset              │
│ slots[CLUSTER_SLOTS]     │                │ ...                      │
│ slot_stats[]             │                │                          │
│ stat_cluster_links_...   │                │ void *protocol_data ─────┼──┐
│ before_sleep_handle_...  │                └──────────────────────────┘  │
│ ...                      │                                              │
│ void *protocol_data ─────┼──┐             ┌──────────────────────────┐  │
└──────────────────────────┘  │             │ clusterNodeLegacyData    │◄─┘
                              │             │ (cluster_legacy.c)       │
┌──────────────────────────┐  │             │                          │
│ clusterLegacyState       │◄─┘             │ configEpoch              │
│ (cluster_legacy.c)       │                │ ping_sent, pong_received │
│                          │                │ fail_time, meet_sent     │
│ currentEpoch             │                │ fail_reports (rax)       │
│ safe_to_join             │                │ orphaned_time            │
│ todo_before_sleep        │                └──────────────────────────┘
│ nodes_black_list         │
│ failover_auth_*          │
│ mf_end, mf_replica       │
│ stats_bus_messages_*     │
│ owner_not_claiming_slot  │
└──────────────────────────┘
```

The legacy protocol accesses its data through macros defined in
cluster_legacy.c:

- `LEGACY_STATE()` — casts `server.cluster->protocol_data` to
  `clusterLegacyState *`.
- `LEGACY_DATA(node)` — casts `node->protocol_data` to
  `clusterNodeLegacyData *`.

The `initNodeData` vtable callback is responsible for allocating and
initializing the per-node protocol data when `createClusterNode` is
called.

## Adding a new protocol

To implement a new cluster bus protocol:

1. Create a new source file (e.g. `cluster_raft.c`).
2. Define a `clusterBusType` instance with all required callbacks.
3. Set `clusterCurrentBus` to point to the new instance (selected at
   startup based on configuration).
4. The common layer handles initialization, config persistence, command
   dispatch, slot migration, and the transport layer. The protocol only
   needs to implement the vtable callbacks.
