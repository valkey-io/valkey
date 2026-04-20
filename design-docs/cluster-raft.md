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

```
HELLO <node-id> <address> <term> <role> <cluster-size>
    First message on every outbound link. Identifies the sender.

HI <node-id> <address> <term> <role> <cluster-size>
    Immediate reply to HELLO, sent on the same inbound link.
    Identifies the receiver back to the sender.

WELCOME <node-id>
    Sent on the outbound link after a NODE_JOIN entry commits.
    Fires the pending MEET callback on the target node.

AE <leader-id> <term> <prev-log-idx> <prev-log-term> <commit> <count>
    AppendEntries. Carries log entries from leader to followers.
    Followed by <count> entry lines: <term> <type> <data>.
    With zero entries, serves as a heartbeat.

AE_ACK <term> <success> <last-log-index>
    Response to AE. Reports whether entries were accepted and the
    follower's last log index for matchIndex tracking.

VOTE_REQ <candidate-id> <term> <last-log-index> <last-log-term>
    RequestVote. Sent by candidates during elections.

VOTE <term> <granted>
    Response to VOTE_REQ. Indicates whether the vote was granted.
    Always sent, even on deny, so the candidate can learn higher terms.

PROPOSE <entry>
    Forwards a proposal from a follower to the leader. The entry
    uses the same format as log entries (type followed by data).
```

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

## Typed Log Entries

Each log entry has a type and a text data field. The entry types and
their data formats:

```
NODE_JOIN <node-id> <address>
    Add a node to the cluster. Applied when a new node is discovered
    via MEET. The node starts as a learner and is promoted to follower
    when the entry is committed.

NODE_LEAVE <node-id>
    Remove a node from the cluster (CLUSTER FORGET). Not yet implemented.

SLOT_CHANGE <node-id-or-dash> <range> [<range> ...]
    Assign or remove slot ownership. A dash means "no owner" (delete
    slots). Ranges use the nodes.conf format: "0-5460" or "5461".

SET_REPLICA_OF <replica-id> <primary-id-or-dash>
    Set a node as replica of a primary (CLUSTER REPLICATE). A dash as
    primary means promote to primary.

FAILOVER <node-id>
    Initiate a manual failover. Not yet implemented.

NODE_META <node-id> <key>=<value> [<key>=<value> ...]
    Update node metadata (IP, port, hostname). Not yet implemented.

NODE_FAIL <node-id>
    Mark a node as failed. Proposed by the leader when a peer exceeds
    the node timeout without responding to heartbeats.
```

Ranges in SLOT_CHANGE use the same format as nodes.conf: `0-5460` or
`5461`. A dash as node-id means "no owner" (delete slots) or "no
primary" (promote to primary).

### Why typed entries instead of a key-value store?

An alternative design uses a generic key-value metadata store where
each log entry is a key-value mutation with Compare-And-Swap (CAS)
conflict detection.

We use typed entries instead because each entry type has domain-specific
apply logic that understands the semantics of the change. This
eliminates the need for CAS because the apply function acts as the
gatekeeper for conflicts.

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

## PROPOSE and Leader Validation

Followers forward proposals to the leader using the PROPOSE message.
The leader always accepts proposals without validation — it appends
them to the log and replicates them. Validation happens at apply time,
where the apply function can detect conflicts and treat them as no-ops.

This design simplifies the leader: it doesn't need to understand the
semantics of each entry type. The leader is a pure log replication
engine. The apply function on each node independently resolves
conflicts using the same deterministic logic, ensuring all nodes
converge to the same state.

If a follower doesn't have an outbound link to the leader, it uses
the inbound link (from the leader's connection) to send PROPOSE.

## Synchronous MEET (HELLO/HI/WELCOME Protocol)

CLUSTER MEET is synchronous from the client's perspective: the command
blocks until the new node is part of the cluster. This is achieved
using the BLOCKED_ASYNC mechanism, which defers the reply until the
NODE_JOIN entry is committed.

### Message flow

All messages are sent on outbound links, except HI which is sent as
a reply on the inbound link.

**HELLO** is the first message on every outbound link (sent by
postConnect). It carries the sender's node-id, address, term, role,
and cluster size.

**HI** is the immediate reply to HELLO, sent on the same inbound link.
It serves as identification so the receiver can bind the link to a
known node.

**WELCOME** is sent on the outbound link after a NODE_JOIN entry
commits. It fires the pending MEET callback, unblocking the client.

### Two singletons meeting

```
  Node A (MEET target)              Node B (MEET initiator)
  singleton leader                  singleton leader
       |                                 |
       |  <--- outbound link ---  HELLO  |  (B connects to A)
       |                                 |
  step down (unknown sender)             |
  set leader = B                         |
       |                                 |
       |  HI (on inbound link) --------> |
       |                                 |  propose NODE_JOIN(A)
       |                                 |  commit (single node)
       |                                 |  apply: A joins cluster
       |                                 |
       |  <--- AE (NODE_JOIN) ---------- |  (replicate to A)
  apply: A joins, promote to follower    |
       |  --- AE_ACK ------------------> |
       |                                 |
       |  <--- WELCOME on outbound ---   |  (NODE_JOIN committed)
  fire MEET callback                     |
```

### Step-down rules

- **Rule 1**: A singleton leader steps down on HELLO from an *unknown*
  node (this is the MEET target side). A HELLO from a known node is
  just a reconnect and doesn't trigger step-down.
- **Rule 2**: A singleton leader steps down on HI from a non-singleton
  (joining an existing cluster).

Both rules set `rs->leader` to the sender's name so the follower
knows where to send PROPOSE.

### Singleton joining an existing cluster

When a follower in an existing cluster receives HELLO from an unknown
singleton, it proposes NODE_JOIN to the leader. The leader commits it
and the new node receives the entry via AE replication.

## Failure Detection

The leader tracks `last_ack_time` per peer, updated on every AE_ACK
received. In the leader's cron, if a peer hasn't responded within
`cluster-node-timeout`, the leader proposes a NODE_FAIL entry.

On apply, the FAIL flag is set on the node and slot coverage is
rechecked, transitioning `cluster_state` to `fail` if a node with
slots is down.

When a failed node comes back and sends AE_ACK, the leader clears
the FAIL flag directly (no log entry needed, since the leader is the
authority on liveness).

### Election timeout

The election timeout is based on `cluster-node-timeout`:
`[T, 2T)` where T = max(cluster-node-timeout, 1000ms). The heartbeat
interval is `election_timeout / 10`, minimum 100ms.

## Blocking Async Commands

Commands that modify cluster metadata (MEET, ADDSLOTS, REPLICATE,
FORGET, FAILOVER) block the client using `blockClientAsync` before
calling the vtable callback. The callback receives an opaque handle
that wraps the client ID.

When the operation completes (synchronously on the leader for
single-node clusters, or asynchronously after raft commit), the
completion callback looks up the client by ID, sends the reply, and
calls `unblockClientAsync`.

The actual unblock is deferred to `blockedBeforeSleep` (not done
inline from timer or read handlers), matching the pattern used by
modules, WAIT, and all other blocking types. This ensures
`processUnblockedClients` runs in the same event loop iteration.

Key implementation details:
- `call()` leaves `executing_command=1` when a client blocks, so the
  sync/async distinction uses `server.current_client == c` instead.
- `c->duration` is not cleared when blocking (stats are deferred), so
  `updateStatsOnUnblock()` is called before `unblockClient()`.
- `BLOCKED_ASYNC` is allowed for replicated clients (the sync path
  completes immediately inside `call()`).

## Compatibility with CLUSTER SLOTS and CLUSTER SHARDS

`CLUSTER SLOTS` and `CLUSTER SHARDS` report node health and
replication offsets. In gossip, each node's replication offset is
piggybacked on ping/pong messages and stored in `node->repl_offset`.
This is not persisted in nodes.conf — it's purely ephemeral.

In raft, we don't propagate replication offsets between nodes.
`isNodeAvailable()` uses the offset to decide whether a replica is
"loading" (offset == 0) or "online". For remote replicas in raft,
we set `repl_offset = 1` when applying SET_REPLICA_OF so they appear
available in CLUSTER SLOTS output.

This is a known limitation. To achieve full compatibility:
- Propagate a loading/online flag when a replica finishes initial
  sync (one-time event, could be a log entry or piggybacked on
  heartbeats).
- Propagate approximate replication offsets periodically by
  piggybacking on AE/AE_ACK messages (no disk cost since heartbeats
  are not persisted). In large clusters, this could be done for a
  rotating subset of nodes per heartbeat to avoid O(N) message size.

## Persistence

TODO: The Raft log and state (currentTerm, votedFor) will be persisted
in nodes.conf using the vars section for Raft state and additional
lines for uncommitted log entries.

## Future Work

- Pre-vote protocol to avoid term inflation from partitioned nodes.
- Log compaction / snapshotting for lagging followers.
- Persistence of Raft log to disk.
- NODE_LEAVE (CLUSTER FORGET) through Raft log.
- Automatic failover through Raft log.
- Slot migration (SETSLOT MIGRATING/IMPORTING).
- Pub/sub and module message propagation over Raft links.
- Permanent learners: in large clusters, some nodes may stay as
  non-voting learners to reduce election and commit overhead. The
  NODE_JOIN entry would include the intended role (follower or learner).
