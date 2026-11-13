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
│         CLUSTER SHARDS/SLOTS, RDB aux fields                     │
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
└───▲─────────┴───────────────┴──┬────────────┴────────────────▲───┘
    │                            │                             │
    │   ┌────────────────────────▼─────────────────────────┐   │
    │   │                  cluster_bus.h                   │   │
    │   │     clusterBusType vtable, clusterCurrentBus     │   │
    │   │     (validateMessageHeader, processMessage,      │   │
    │   │      postConnect, slotChange, forgetNode, ...)   │   │
    │   └────────────────────────┬─────────────────────────┘   │
    │                            │                             │
    │           ┌────────────────┴────────────────┐            │
    │           │                                 │            │
┌───┴───────────▼───────────────┐ ┌───────────────▼────────────┴───┐
│        cluster_legacy.c       │ │         cluster_raft.c         │
│ clusterLegacyBus (gossip):    │ │ clusterRaftBus (Raft):         │
│ - PING, PONG, MEET, FAIL, ... │ │ - leader election              │
│ - PFAIL/FAIL detection        │ │ - replicated log of cluster    │
│ - configEpoch, currentEpoch   │ │   changes (slots, nodes,       │
│ - failover elections,         │ │   failovers, ...)              │
│   manual failover             │ │ - clusterRaftState             │
│ - clusterLegacyState,         │ │                                │
│   clusterNodeLegacyData       │ │ (added in a later change)      │
└───────────────────────────────┘ └────────────────────────────────┘
```

The common layer dispatches to the active implementation through
`clusterCurrentBus`; the implementations call back into the common layer
(node and slot state, nodes.conf, links), shown by the arrows on the sides.

## Design rules

- Common code calls protocol code only through `clusterCurrentBus`, and
  never reads protocol-specific data.
- Functions declared in `cluster.h` are implemented in the common layer. If
  the behavior is protocol-specific, the function dispatches through the bus.
- Each protocol owns its wire format. The link layer only frames incoming
  messages, using `validateMessageHeader`, and passes them to
  `processMessage`.
- The cluster topology (nodes, slots, shards) is kept in common state, but
  the active protocol decides when it changes. The gossip protocol applies a
  change directly; a consensus-based protocol such as Raft applies it when
  it is committed.
- Bus operations that change the topology report completion through a
  callback. A client running such a command is blocked with `BLOCKED_ASYNC`
  until the callback replies, so a change can wait for consensus without
  blocking the server.

## Protocol-specific state

`clusterState` and `clusterNode` each have a `void *protocol_data` pointer
where the active protocol keeps its own state:

```
┌───────────────────────────┐        ┌───────────────────────────┐
│ clusterState              │        │ clusterNode               │
├───────────────────────────┤        ├───────────────────────────┤
│ nodes, shards, slots[],   │        │ name, flags, slots[],     │
│ ...                       │        │ link, ...                 │
│ protocol_data ──┐         │        │ protocol_data ──┐         │
└─────────────────┼─────────┘        └─────────────────┼─────────┘
                  │                                    │
┌─────────────────▼─────────┐        ┌─────────────────▼─────────┐
│ clusterLegacyState        │        │ clusterNodeLegacyData     │
├───────────────────────────┤        ├───────────────────────────┤
│ currentEpoch, failover    │        │ configEpoch, ping_sent,   │
│ election, ...             │        │ fail_reports, ...         │
└───────────────────────────┘        └───────────────────────────┘
```

The legacy protocol reaches its data through the `LEGACY_STATE()` and
`LEGACY_DATA(node)` macros in cluster_legacy.c. Per-node data is allocated
and freed by the `initNodeData` and `freeNodeData` bus operations. The Raft
implementation keeps its own state in the same way and is described in its
own design document.
