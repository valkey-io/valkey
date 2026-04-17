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
