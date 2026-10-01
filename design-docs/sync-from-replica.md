# Design Document: Sync From Replica

Issue: [valkey-io/valkey#2767](https://github.com/valkey-io/valkey/issues/2767)

## Problem

Full synchronization causes a large burst of resource consumption on the primary:
- `fork()` for BGSAVE - copy-on-write memory overhead, CPU spike
- RDB generation - disk I/O, CPU for serialization
- RDB transfer - network bandwidth from the primary

Operators want to add replicas without impacting the primary's ability to serve traffic.

## Goal

Allow a new replica to get its initial dataset from an **existing sibling replica** instead of the primary. The primary's only cost should be a lightweight partial resync from its existing replication backlog: no fork, no BGSAVE, and no RDB generation.

## Design Overview

The new node N performs standard replication from sibling S (chain: P->S->N). After syncing, N switches to the real primary P with a partial resync.

```text
                    ┌─────────────┐
                    │  Primary P  │
                    │  (no BGSAVE)│
                    └──────┬──────┘
                           │ replication stream
                    ┌──────▼──────┐
                    │  Sibling S  │---- standard replication ----> New Node N
                    │  (BGSAVE)   │     (RDB + stream forwarding)
                    └─────────────┘
```

**Phase 1 - Chain replication from S:**
1. N does `CLUSTER REPLICATE <P>` - cluster topology says N->P (gossip, shard_id, flags)
2. Internally, N redirects `server.primary_host` to S (not P)
3. N does standard replication from S: PSYNC handshake, RDB transfer, stream forwarding
4. S does BGSAVE, sends RDB snapshot to N. If both N and S have dual-channel
   replication enabled, this temporary N<->S sync can use the existing
   dual-channel full-sync protocol.
5. S forwards P's replication stream to N (built-in replica-of-replica behavior)
6. N loads the RDB and reaches CONNECTED state with S

**Phase 2 - Switch to primary:**
7. N records the offset from S's `+FULLRESYNC` and flushes its ACK to S
8. N keeps reading from S until S has streamed post-RDB commands and N reaches a drained command boundary
9. N caches S as primary (`replicationCachePrimary`) to preserve replid and applied offset
10. N redirects `server.primary_host` to P and reconnects
11. N PSYNCs to P using the cached replid (which is P's replid, inherited by S) and the current applied offset
12. P does partial resync from its backlog. No fork. Done.

## Why This Works

### Chain replication avoids full-sync fork impact on P (validated on EKS)

S is already reading P's replication stream during BGSAVE. Forwarding that stream to N is copying bytes to another socket. On isolated infrastructure (separate pods/VMs), this adds no measurable overhead to P.

**Validated:** A/B benchmark on AWS EKS (c7g.2xlarge, isolated ARM pods, 5GB dataset) showed SET max latency of 4.1ms median / 8.2ms worst during chain sync, compared with 89.1ms median / 90.6ms worst during direct sync from P. Across 3 runs, P did not fork for N in the chain-sync scenario; S handled the full-sync fork instead.

### Cluster/replication decoupling

The cluster layer and replication layer are independent:

| Layer | What it tracks | How it's set |
|---|---|---|
| Cluster | `myself->replicaof = P` (clusterNode pointer) | `clusterSetPrimary()` |
| Gossip | `hdr->replicaof = myself->replicaof->name` | Cluster bus header generation uses the cluster pointer |
| Replication | `server.primary_host = S` (IP string) | Redirected after `clusterSetPrimary` |

**No code in `cluster_legacy.c` compares `server.primary_host` with `myself->replicaof->ip`.** The cluster layer has zero awareness of where the replication TCP connection actually goes.

### Offset, replid, and backlog correctness

- S's `server.replid` is P's replid - copied during S's own sync
- When N syncs from S, N inherits P's replid from S
- After loading S's RDB, N must still apply the command stream S queued during the RDB transfer
- N delays the switch to P until it has applied post-RDB commands from S and reached a drained command boundary
- When N PSYNCs to P, P recognizes its own replid and finds the offset in its backlog
- P only needs to retain the residual handoff gap after N caught up through S, not the whole RDB-transfer window

### Sub-replica safeguard doesn't fire

The sub-replica safeguard checks the cluster graph: `myself->replicaof->replicaof != NULL`

Since `myself->replicaof = P` (a primary), `P->replicaof = NULL`. The condition is never true. The safeguard is a cluster-graph traversal; it does not inspect the replication TCP target.

### S-to-P switch

The switch is intentionally delayed after RDB load:

- `replicaAfterLoadPrimaryRDB()` records S's FULLRESYNC offset and flushes N's ACK to S
- When the temporary sync uses dual-channel replication, the same handoff is
  armed after the dual-channel RDB and main PSYNC channels have both completed
- S starts streaming commands queued during the RDB transfer after it receives N's ACK
- `replicationMaybeSwitchToPrimaryAfterSiblingSync()` waits for N to apply data beyond the recorded offset and reach a drained command boundary
- The drained-boundary check runs after direct reads and after I/O-thread read completion
- `replicationCachePrimary()` then caches S's replid and last applied offset
- `replicationHandlePrimaryDisconnection()` reconnects to P using the updated `server.primary_host`
- P resends any residual bytes from backlog via PSYNC

## Guard Flag

A runtime flag `server.cluster_syncing_from_sibling` protects against cluster events overriding the sibling connection while N is still using S.

### Where it's checked

**At the top of `clusterSetPrimary`** (single guard point for topology changes):
```c
/* Topology changes supersede the transient sibling sync target;
 * replicationSetPrimary() below establishes the new target. */
clusterDiscardSiblingSyncIfActive();
```

This covers gossip reconfiguration, sub-replica correction, replica migration,
slot migration, and operator-initiated `CLUSTER REPLICATE`. The same helper
runs on the `CLUSTER REPLICATE NO ONE` promotion path, which leaves replica
mode without going through `clusterSetPrimary`.

Failure paths (handshake error, sibling link drop) instead call
`replicationAbortSiblingSync()`, which redirects `server.primary_host` back to
the topology primary. If gossip has not yet provided a primary, the abort
returns `C_ERR` without clearing state: N keeps the sibling as its replication
target — and the guard flag stays accurate for INFO and the failover checks —
until a topology primary appears.

**Separate check in CLUSTER FAILOVER handler:**
```c
if (server.cluster_syncing_from_sibling) {
    addReplyError(c, "Node is syncing from sibling, cannot failover");
    return;
}
```
FORCE and TAKEOVER bypass `cluster-replica-no-failover` and must be blocked explicitly. N has incomplete data while it is still syncing from S; promotion would risk data loss.

### Properties
- Runtime only - not persisted. On crash and restart, N falls back to normal full sync from P.
- Auto-cleared when N finishes reading from S and switches to P.
- Set after `cancelReplicationHandshake` in the CLUSTER REPLICATE handler, because that call can clear the flag.

## Sibling Selection

N selects the best sibling from the primary's replica list in cluster state:

**Criteria:**
1. Not in FAIL or PFAIL state
2. Has a non-zero replication offset (not freshly added)
3. Highest repl_offset wins (closest to primary, lowest lag)

**Who picks:** N picks, using cluster gossip data available after CLUSTER MEET. Primary-side selection (P picks the best S) would be more accurate but requires new protocol and is deferred to a future enhancement.

**If no eligible sibling:** fall back to normal full sync from P. The feature is best-effort, not mandatory.

## rdb-only BGSAVE Piggybacking Bug

When a sibling has a BGSAVE already in progress and a new rdb-only client attaches to it, the offset from `+FULLRESYNC` is from the original BGSAVE trigger time. But rdb-only clients do not get the output buffer copy. The offset is stale because the RDB content is ahead of the offset, which can duplicate non-idempotent commands such as INCR, LPUSH, and SADD.

**Fix:** Never attach rdb-only sync requests to an existing BGSAVE. Force `WAIT_BGSAVE_START` and trigger a fresh BGSAVE. This ensures the offset in `+FULLRESYNC` matches the RDB content exactly.

## Failure Modes

| Failure | During | Result | Recovery |
|---|---|---|---|
| S dies mid-RDB transfer | Sibling sync | Connection drops | `cancelReplicationHandshake` -> `replicationAbortSiblingSync` -> fall back to P |
| S dies during stream catch-up | Sibling sync | Primary link drops after RDB load | `replicationHandlePrimaryDisconnection` -> `replicationAbortSiblingSync` -> fall back to P |
| S gets promoted (failover) | Sibling sync | `disconnectReplicas` kills N | Detect topology change, fall back to P |
| S's BGSAVE causes OOM on S | Sibling sync | S crashes or kills BGSAVE | N's connection drops, fall back to P |
| P fails | Sibling sync | Switch to P has no target | N stays on S, guard flag stays set; failover elects a new primary, `clusterSetPrimary` discards the sibling sync and N syncs from the new primary |
| P rejects PSYNC (backlog trimmed) | Switch to P | Residual handoff gap too large | Full sync from P (defeats purpose but safe) |
| N crashes during sibling sync | Any | Flag lost | Restart triggers normal sync from P |
| Operator runs CLUSTER FAILOVER FORCE | Sibling sync | Blocked by guard | Error returned to operator |
| Replica migration triggers | Sibling sync | `clusterSetPrimary` fires | Guard discards sibling sync, migration proceeds normally |
| Operator runs CLUSTER REPLICATE | Sibling sync | `clusterSetPrimary` fires | Guard discards sibling sync, new replication proceeds |
| Operator runs CLUSTER REPLICATE NO ONE | Sibling sync | Promotion path fires | Guard discards sibling sync, node becomes an empty primary |

**Every failure mode falls back safely.** Worst case is a normal full sync from P, which is the current behavior without this feature.

## Performance

### Benchmark (AWS EKS c7g.2xlarge, isolated ARM pods, 5GB dataset)

A/B comparison on AWS EKS using the PR image
`507286591552.dkr.ecr.us-east-1.amazonaws.com/valkey-sync-bench:ef0be19c5-arm64`.
Each run used fresh P/S/N/runner pods on c7g.2xlarge nodes because this workload
leaves little post-fork memory headroom for repeatedly reusing the same primary
on this instance size.

Setup: 5M sequential SETs with 1KB values loaded on P. S is fully synced before
the measurement starts. The measured workload is 1M SETs, 1KB values, 50
clients, 4 threads, and 10s warmup. N starts replication after the warmup, so
the full-sync fork lands inside the measured window.

**Per-run results:**

| Scenario | Run | Baseline SET rps | During-sync SET rps | During-sync max latency | Primary full-sync/forks | Sibling full-sync/forks |
|---|---:|---:|---:|---:|---:|---:|
| A: N syncs from P | 1 | 99,622 | 93,102 | 90.6ms | +1/+1 | 0/0 |
| A: N syncs from P | 2 | 100,241 | 95,338 | 89.1ms | +1/+1 | 0/0 |
| A: N syncs from P | 3 | 102,701 | 96,806 | 89.1ms | +1/+1 | 0/0 |
| B: N syncs from S | 1 | 103,242 | 101,235 | 3.8ms | 0/0 | +1/+1 |
| B: N syncs from S | 2 | 105,242 | 105,042 | 8.2ms | 0/0 | +1/+1 |
| B: N syncs from S | 3 | 108,050 | 107,829 | 4.1ms | 0/0 | +1/+1 |

**Summary:**

| Metric | A: N syncs from P | B: N syncs from S |
|---|---:|---:|
| Average baseline SET rps | 100,855 | 105,511 |
| Average during-sync SET rps | 95,082 (-5.7%) | 104,702 (-0.8%) |
| Median SET max latency during sync | 89.1ms | 4.1ms |
| Worst SET max latency during sync | 90.6ms | 8.2ms |
| P full-sync/fork deltas for N | +3/+3 | 0/0 |
| S full-sync/fork deltas for N | 0/0 | +3/+3 |

The direct-sync scenario forks on P in every run and produces an 89-91ms max
latency spike. The chain-sync scenario moves that fork to S in every run; P's
full-sync and fork counters stay flat while the measured SET workload remains
near baseline.

## Configuration and Observability

### New configuration

```conf
# Enable sync-from-replica optimization (default: no)
cluster-prefer-sync-from-replica yes
```

### Observability

**INFO replication additions:**
```text
sync_from_replica_in_progress:1
sync_from_replica_phase:handshake|rdb_transfer|rdb_loading|stream_catchup|none
```

**Log messages:**
```text
[NOTICE] Sync-from-replica: selected sibling <node-id> at offset <X> (primary at <Y>, gap <delta>)
[NOTICE] Sync-from-replica: redirecting replication to sibling <host>:<port>
[NOTICE] Sync-from-replica: sibling stream drained (replid=<id> offset=<X>), switching to primary <node-id>
[NOTICE] Sync-from-replica: aborting sibling sync, falling back to primary
```

## Relationship to Maintainer Questions

From the weekly meeting discussion on issue #2767:

> 1. The full-sync will always happen on the replica, but we could stream the ongoing change from the primary or the replica.

**Answer:** We stream from **the replica (S)**. N does standard chain replication from S. S already reads P's stream (normal replication) and forwards it to N. This is the simplest approach: no new protocol, no cross-server dual-channel, and no buffering on N. Validated on EKS: chain causes zero backpressure on P when pods are on isolated infrastructure.

> 2. How to integrate this into the cluster mode. It can either be a first-class component of the topology or it can just be a transient step.

**Answer:** Transient step. The cluster topology always shows N->P. The sibling connection is invisible to gossip. No new topology concepts or permanent chain replication are introduced. After sync, N is a normal replica of P.

Alternatives considered and future enhancements (primary-side sibling
selection, backlog pinning, shared BGSAVE for simultaneous joiners, automatic
backlog sizing) are tracked on issue
[#2767](https://github.com/valkey-io/valkey/issues/2767).
