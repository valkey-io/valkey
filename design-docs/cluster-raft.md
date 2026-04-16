# Cluster Raft Design

## Overview

Cluster Raft is an alternative cluster bus protocol for Valkey that uses
Raft consensus for all cluster metadata changes. It replaces the gossip
protocol's eventually-consistent model with strong consistency.

Enabled via `cluster-protocol raft` (mutually exclusive with gossip).

## Wire Format

Binary header (8 bytes): `"RAFT" + uint32 BE totlen`.
Payload: space-separated text. First token is the message type.
Multi-line messages use `\n` to separate the header line from entry lines.

Messages:
- `HELLO <node-id> <address> <term> <role> <cluster-size>`
- `JOIN <node-id> <address>`
- `AE <leader-id> <term> <prev-log-idx> <prev-log-term> <commit> <count>`
  followed by entry lines: `<term> <type> <data>`
- `AE_OK <term> <success>`
- `VOTE <candidate-id> <term> <last-log-index> <last-log-term>`
- `VOTE_OK <term> <granted>`

The address string uses the nodes.conf format:
`ip:port@cport[,hostname][,aux=val]*`

## Roles

- **Learner**: Receives log entries but doesn't vote or count for quorum.
  New nodes joining an existing cluster start as learners.
- **Follower**: Participates in elections and counts for quorum.
  Promoted from learner when the node's NODE_JOIN entry is committed.
- **Candidate**: Requesting votes after election timeout.
- **Leader**: Accepts proposals, replicates log, sends heartbeats.

The Raft leader is independent of the data primary/replica role. Any
node can be the Raft leader.

## Typed Log Entries vs Key-Value Store

An alternative design (see murphyjacob4's proposal) uses a generic
key-value metadata store where each log entry is a key-value mutation.
Conflict detection uses Compare-And-Swap (CAS) with per-key term and
index tracking.

We use typed log entries instead:

```
enum raftEntryType {
    RAFT_ENTRY_NODE_JOIN,
    RAFT_ENTRY_NODE_LEAVE,
    RAFT_ENTRY_SLOT_CHANGE,
    RAFT_ENTRY_SET_REPLICA,
    RAFT_ENTRY_FAILOVER,
    RAFT_ENTRY_NODE_META,
};
```

Each entry type has domain-specific apply logic that understands the
semantics of the change. This eliminates the need for CAS because the
apply function acts as the gatekeeper for conflicts.

### Why not CAS?

Consider two replicas proposing failover for the same shard:

With CAS (key-value model):
- Both proposals say "set shard primary, assuming current primary is X."
- The leader must reject the second proposal at proposal time using CAS.
- Without CAS, both get committed and both get applied, resulting in
  two primaries.

With typed entries:
- Both FAILOVER entries get committed in order.
- Apply FAILOVER for replica A: shard has no primary → A becomes primary.
- Apply FAILOVER for replica B: shard already has primary A → no-op.
- The apply logic has domain knowledge to detect the conflict.

This is simpler and more flexible:
- No per-key index/term tracking needed.
- Conflict resolution is in one place (the apply function).
- Apply functions are naturally idempotent.
- New entry types can define their own conflict rules.

The trade-off is that conflicting entries still consume log space even
when their effect is a no-op. This is acceptable because metadata
changes are infrequent.

## Cluster Formation

Each node boots as a single-member Raft group and immediately becomes
leader (term 1). Nodes join via CLUSTER MEET:

1. MEET creates a handshake node and establishes a link.
2. HELLO exchange identifies the peer and carries term/role/cluster-size.
3. The joining node steps down to learner.
4. The leader appends a NODE_JOIN entry to the Raft log.
5. Once committed, the learner is promoted to follower.

Constraints:
- At least one side of MEET must be a singleton.
- Merging two multi-node clusters is not supported.

When MEET is issued on a follower, the follower adds the node
optimistically and forwards a JOIN message to the leader.

## Log Replication

AppendEntries (AE) messages carry log entries from the leader to
followers. Each AE includes prev_log_index and prev_log_term for
consistency checking. Followers that detect a conflict truncate
divergent entries.

The leader tracks match_index per follower. When a majority has
replicated an entry, the leader advances commit_index. Committed
entries are applied in order.

## Persistence

TODO: The Raft log and state (currentTerm, votedFor) will be persisted
in nodes.conf using the vars section for Raft state and additional
lines for uncommitted log entries.

## Future Work

- Pre-vote protocol to avoid term inflation from partitioned nodes.
- Log compaction / snapshotting for lagging followers.
- Persistence of Raft log to disk.
- SLOT_CHANGE, SET_REPLICA, FAILOVER entry types.
- Pub/sub and module message propagation over Raft links.
- Permanent learners: in large clusters, some nodes may stay as
  non-voting learners to reduce election and commit overhead. The
  NODE_JOIN entry would include the intended role (follower or learner).
