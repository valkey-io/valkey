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

FAILOVER_PREPARE
    Sent by a replica to its primary to request a coordinated failover.
    The primary pauses writes so the replica can catch up. Not a raft
    log entry — just a direct message over the cluster link.

REPL_OFFSETS <node-id> <offset> [<node-id> <offset> ...]
    Sent by the leader to propagate replication offsets. Broadcast
    periodically (every 10s) and immediately when a replica's offset
    transitions from 0 to non-zero. Also sent before NODE_FAIL to
    provide replicas with sibling offsets for failover ranking.
    Recipients update node->repl_offset for CLUSTER SLOTS/SHARDS
    health reporting.
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

NODE_FORGET <node-id>
    Remove a node from the cluster (CLUSTER FORGET). Not yet implemented.

SLOT_CHANGE <node-id-or-dash> <range> [<range> ...]
    Assign or remove slot ownership. A dash means "no owner" (delete
    slots). Ranges use the nodes.conf format: "0-5460" or "5461".

SET_REPLICA_OF <replica-id> <primary-id-or-dash>
    Set a node as replica of a primary (CLUSTER REPLICATE). A dash as
    primary means promote to primary.

FAILOVER <node-id>
    Initiate a manual failover. Not yet implemented.

NODE_INFO <node-id> <key>=<value> [<key>=<value> ...]
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

Followers forward proposals to the leader using the PROPOSE message,
sent on the outbound link to the leader. The leader always accepts
proposals without validation — it appends them to the log and
replicates them. Validation happens at apply time, where the apply
function can detect conflicts and treat them as no-ops.

This design simplifies the leader: it doesn't need to understand the
semantics of each entry type. The leader is a pure log replication
engine. The apply function on each node independently resolves
conflicts using the same deterministic logic, ensuring all nodes
converge to the same state.

### Proposal retry on leader change

A pending proposal stays in the follower's list until the matching
entry is committed and applied. Sending PROPOSE does not remove it.
If the leader dies or a new leader is elected, the proposal is
retried automatically:

- If the follower becomes the new leader, it appends the pending
  proposals to its own log.
- If another node becomes leader, the follower forwards the pending
  proposals to the new leader.

This makes proposals resilient to leader changes. Even if a PROPOSE
was successfully sent but the leader died before committing, the
proposal is retried. Duplicate proposals are harmless because all
entry types are idempotent at apply time.

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
  Admin                   Node A               Node B
  client               singleton leader     singleton leader
    |                       |                     |
    |--- CLUSTER MEET B --->|                     |
    |                       |--- HELLO ---------->|
    |                       |                     |  Unknown sender
    |                       |                     |  Step down to learner
    |                       |<------- HI ---------|
    |                       |                     |
    |             Propose NODE_JOIN               |
    |             Commit (single node)            |
    |             Apply: B joins cluster          |
    |                       |                     |
    |<------- OK -----------|                     |
    |                       |---- WELCOME ------->|  No-op in this case
    |                       |                     |
    |                       |--- AppendEntries -->|  Apply: promote
    |                       |                     |  to follower
    |                       |<-- AppendEntriesAck-|
```

### Singleton joining an existing cluster

```
  Admin                   Node A               Node B          Node C
  client               singleton leader      follower          leader
    |                       |                     |               |
    |--- CLUSTER MEET B --->|                     |               |
    |                       |--- HELLO ---------->|               |
    |                       |                     |  Unknown sender
    |                       |<------- HI ---------|               |
    |                       |                     |               |
    |          Step down (rule 2: HI              |               |
    |          from non-singleton)                |               |
    |                       |          PROPOSE NODE_JOIN(A) ----->|  Append to Raft log
    |                       |                     |               |
    |                       |              +--------------------------+
    |                       |              |  NODE_JOIN(A) committed  |
    |                       |              |  via Raft (B+C quorum)   |
    |                       |              +--------------------------+
    |                       |                     |               |
    |                       |<------- WELCOME ----|               |
    |<------- OK -----------|                     |               |
    |                       |                     |               |  Eventually,
    |                       |<---- AppendEntries -----------------|  when link to A
    |              Apply: A joins                 |               |  is established
    |              Promote to follower            |               |
```

### Step-down rules

- **Rule 1**: A singleton leader steps down on HELLO from an *unknown*
  node (this is the MEET target side). A HELLO from a known node is
  just a reconnect and doesn't trigger step-down.
- **Rule 2**: A singleton leader steps down on HI from a non-singleton
  (joining an existing cluster).

Both rules set `rs->leader` to the sender's name so the follower
knows where to send PROPOSE.

### Membership changes

Nodes are added and removed one at a time using NODE_JOIN and
NODE_FORGET log entries. Adding or removing a single server at a
time is safe without joint consensus, as described in the Raft
dissertation (Ongaro, 2014, p. 51):
https://web.stanford.edu/~ouster/cgi-bin/papers/OngaroPhD.pdf

## Failure Detection

The leader tracks `last_ack_time` per peer, updated on every AE_ACK
received. In the leader's cron, if a peer hasn't responded within
`cluster-node-timeout`, the leader proposes a NODE_FAIL entry.

On apply, the FAIL flag is set on the node and slot coverage is
rechecked, transitioning `cluster_state` to `fail` if a node with
slots is down.

When a failed node comes back and sends AE_ACK, the leader proposes
a NODE_RECOVER entry to clear the FAIL flag on all nodes.

### Election timeout

The election timeout is based on `cluster-node-timeout`:
`[T, 2T)` where T = max(cluster-node-timeout, 1000ms). The heartbeat
interval is `election_timeout / 10`, minimum 100ms.

### Backdating on leader election

When a node wins an election, it backdates the old leader's
`last_ack_time` to the time of the last heartbeat received. This
allows the new leader to detect the old leader as failed almost
immediately, without waiting an additional `node_timeout`.

### Multiple simultaneous failures

When the raft leader and another primary fail simultaneously, the
failure detection timelines differ between gossip and raft:

```
Gossip protocol — parallel failure detection:

  Time 0     Leader (L) and Primary (P) both fail
             |
  T          All nodes mark L and P as PFAIL independently
             |
  T+few sec  Gossip propagates PFAIL → FAIL for both L and P
             Replicas of L and P start failover in parallel
             |
  ~T+3s      Both failovers complete

  Total: ~node_timeout + gossip propagation
```

```
Raft protocol — sequential through new leader:

  Time 0     Leader (L) and Primary (P) both fail
             |
  T..2T      Election timeout fires on fastest follower
             New leader (N) elected
             |
  ~T         N backdates L's last_ack_time → proposes NODE_FAIL(L)
             immediately. Replica of L starts failover.
             |
  ~2T        N's last_ack_time for P was set to election time.
             After node_timeout from election, N detects P as failed.
             Proposes NODE_FAIL(P). Replica of P starts failover.

  Total: ~2 * node_timeout for the second failure
```

The gossip protocol detects all failures in parallel after one
`node_timeout`. Raft detects the old leader quickly (backdating) but
other failures take an additional `node_timeout` from the election,
because the new leader has no prior heartbeat history for those nodes.

Note: in gossip, only one failover election can be in progress at a
time (per epoch). When multiple primaries fail simultaneously, their
replicas are staggered using a "failed primary rank" to avoid
collisions. This adds small delays but is still much faster than
raft's sequential detection, since the ranking delays are on the
order of seconds rather than a full `node_timeout`.

### Potential future improvements to simultaneous failure detection

* Heartbeat timestamps in VOTE: To close this gap, voters could include their
  last known `last_ack_time` for each peer in the VOTE response. The new leader
  would use the oldest `last_ack_time` across all voters for each peer, giving
  it a head start on failure detection for all unresponsive nodes — not just the
  old leader. However, this will bloat the heartbeat messages.

* Defer primary failure detection to each shard, using the replication protocol.
  Primaries send PING commands over the replication stream as heartbeats to the
  replicas. If a replica hasn't received one for enough time, it can propose its
  primary as failing to the raft leader.

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

The leader learns each follower's replication offset from AE_ACK
messages and updates `node->repl_offset` locally. To propagate
offsets to other nodes, the leader sends REPL_OFFSETS messages. This
is currently done before NODE_FAIL (for failover ranking) and is
planned for when a replica finishes initial sync (offset transitions
from 0 to non-zero).

On the leader, CLUSTER SLOTS and CLUSTER SHARDS show accurate offsets
and health. On followers, remote replicas use `repl_offset = 1` as a
placeholder (set when applying SET_REPLICA_OF) until REPL_OFFSETS
broadcast to all nodes is fully implemented.

## Persistence

TODO: The Raft log and state (currentTerm, votedFor) will be persisted
in nodes.conf using the vars section for Raft state and additional
lines for uncommitted log entries.

## Future Work

- Pre-vote protocol to avoid term inflation from partitioned nodes.
- Log compaction / snapshotting for lagging followers.
- Persistence of Raft log to disk.
- Slot migration (SETSLOT MIGRATING/IMPORTING).
- Broadcast REPL_OFFSETS to all nodes when a replica finishes sync.
- Pub/sub and module message propagation over Raft links.
- Permanent learners: in large clusters, some nodes may stay as
  non-voting learners to reduce election and commit overhead. The
  NODE_JOIN entry would include the intended role (follower or learner).
