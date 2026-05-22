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
HELLO <node-id> <address>
    First message on every outbound link. Identifies the sender.

HI <node-id> <address>
    Immediate reply to HELLO, sent on the same inbound link.
    Completes the identification handshake.

MEET singleton|cluster
    Sent after HELLO on CLUSTER MEET-initiated connections. Declares
    the sender's cluster status. The receiver decides based on its
    own status (see Cluster Formation below).

ADD_ME
    Reply to MEET: "I stepped down to joiner, please add me to your
    cluster." Sent by a singleton that received MEET.

WELCOME
    Reply to MEET: "I'm in a cluster and will add you." Sent by a
    cluster member that received MEET from a singleton. The receiver
    steps down to joiner on receiving WELCOME.

MEET_REJECTED
    Reply to MEET: both sides are already in a non-singleton cluster.
    Merging clusters is not supported. The CLUSTER MEET client
    receives an error.

AE <leader-id> <term> <prev-log-idx> <prev-log-term> <commit> <count>
    AppendEntries. Carries log entries from leader to followers.
    Followed by <count> entry lines: <term> <type> <data>.
    With zero entries, serves as a heartbeat.

AE_ACK <term> <success> <last-log-index> <repl-offset>
    Response to AE. Reports whether entries were accepted, the
    follower's last log index for matchIndex tracking, and the
    node's replication offset.

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

- **Joiner**: A singleton that stepped down to join another cluster.
  Cannot vote, propose, or form a cluster. Waits for AE from the
  leader to receive its NODE_JOIN entry. Reverts to singleton leader
  after 3× cluster-node-timeout if no AE arrives.
- **Follower**: Participates in elections and counts for quorum.
  Promoted from joiner when the node's NODE_JOIN entry is applied.
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

SET_REPLICA_OF <replica-id> <primary-id-or-dash> <shard-id>
    Set a node as replica of a primary (CLUSTER REPLICATE). A dash as
    primary means promote to primary. The shard-id is the target shard:
    for promotion, a new random id; for assignment, the primary's
    current shard-id (used as a guard against concurrent changes).

FAILOVER <replica-id> <primary-id>
    The replica takes over the primary's slots and becomes primary.
    The old primary becomes a replica of the new primary.

NODE_INFO <node-id> <address-string> <flags>
    Update node address and self-set flags. The address-string uses
    the nodes.conf format. Flags is "nofailover" or "noflags".

NODE_FAIL <node-id>
    Mark a node as failed. Proposed by the leader when a peer exceeds
    the node timeout without responding to heartbeats.

NODE_RECOVER <node-id>
    Clear the FAIL flag. Proposed by the leader when it receives
    AE_ACK from a previously failed node.
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

## Link Identification (HELLO/HI)

HELLO is sent on every new outbound connection. HI is the reply on
the same link. This exchange identifies both sides and occurs on
every connection: initial joins, reconnections, and new links to
nodes learned via AE.

After HI completes, the leader sends AE to bring the peer up to
date. A follower retries any deferred proposals to the leader.

## Cluster Formation (MEET/ADD_ME/WELCOME)

CLUSTER MEET is synchronous from the client's perspective: the command
blocks until the target node has stepped down (can't form a competing
cluster) or until the node is committed in the raft log.

MEET is sent immediately after HELLO on CLUSTER MEET-initiated
connections. The reply is either ADD_ME or WELCOME:

- **MEET → ADD_ME**: The receiver is a singleton leader. It steps
  down to joiner and replies ADD_ME. The sender invites it.
- **MEET → WELCOME**: The receiver is in a cluster (size > 1). It
  replies WELCOME immediately and invites the sender. The sender
  steps down to joiner on receiving WELCOME.

### Unblock conditions for CLUSTER MEET

The client is unblocked when any of these occur:
- **ADD_ME received**: The peer stepped down to joiner — it can't
  form a competing cluster. Safe to proceed with the next MEET.
- **WELCOME received**: We stepped down to joiner — we'll be added
  by the peer's cluster.
- **NODE_JOIN applied** (fallback): When the target node's NODE_JOIN
  entry is applied locally (e.g., for deferred meets forwarded via
  a follower).

### Why early unblock is safe

The blocking requirement prevents disjoint non-singleton clusters
from forming during chained MEET commands (A→B, B→C, C→D). Early
unblock (on ADD_ME/WELCOME) is safe because once a node steps down
to joiner, it cannot:
- Win an election (no voters know it)
- Commit entries (not a leader)
- Form a cluster independently

The worst case on crash is liveness failure (orphaned joiners), not
safety violation (disjoint clusters). Joiners revert to singleton
leader after 3× cluster-node-timeout.

### Deferred MEET

When a joiner receives MEET (e.g., in reverse star formation where
multiple nodes MEET the same target), the MEET is deferred until the
joiner promotes to follower (applies its own NODE_JOIN). At that
point, the node has size > 1 and processes deferred meets by sending
WELCOME and inviting the sender.

### Two singletons meeting

```
  Admin                   Node A               Node B
  client               singleton leader     singleton leader
    |                       |                     |
    |--- CLUSTER MEET B --->|                     |
    |                       |--- HELLO ---------->|
    |                       |<------- HI ---------|
    |                       |--- MEET singleton ->|
    |                       |                     |  Step down to joiner
    |                       |<------ ADD_ME ------|
    |<------- OK -----------|                     |
    |                       |                     |
    |             Propose NODE_JOIN(A) + NODE_JOIN(B)
    |             Commit (quorum=1)               |
    |             Apply: size=2                   |
    |                       |                     |
    |                       |--- AE ------------->|  Apply: promote
    |                       |<-- AE_ACK ----------|  to follower
```

### Singleton joining an existing cluster

```
  Admin                   Node A               Node B
  client               singleton leader       in cluster
    |                       |                     |
    |--- CLUSTER MEET B --->|                     |
    |                       |--- HELLO ---------->|
    |                       |<------- HI ---------|
    |                       |--- MEET singleton ->|
    |                       |                     |  size > 1
    |                       |<----- WELCOME ------|  Invites A
    |                       |                     |
    |              Step down to joiner            |
    |<------- OK -----------|                     |
    |                       |                     |  Leader commits
    |                       |                     |  NODE_JOIN(A)
    |                       |<---- AE ------------|
    |                       |                     |
    |              Apply NODE_JOIN(A)             |
    |              promotes A to follower         |

```

### AE suppression during joining

AE and VOTE_REQ are not sent to nodes with the CLUSTER_NODE_MEET
flag (not yet in the raft log). This prevents a race where AE
arrives before MEET on the same link, causing the receiver to step
down prematurely and defer the MEET.

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
and health. On followers, remote replicas start with `repl_offset = 0`
(hidden from CLUSTER SLOTS) until the leader broadcasts their real
offset via REPL_OFFSETS. The broadcast is triggered immediately when
a node's offset transitions from 0 to non-zero, and periodically
every 10 seconds.

## Persistence

TODO: The Raft log and state (currentTerm, votedFor) will be persisted
in nodes.conf using the vars section for Raft state and additional
lines for uncommitted log entries.

## Shard Epoch (not yet implemented)

A shard-epoch is a per-shard monotonically increasing counter, bumped
on topology changes within the shard (FAILOVER, SET_REPLICA_OF,
SLOT_CHANGE). Entries that modify shard topology include the current
shard-epoch at proposal time. On apply, if the shard-epoch has
advanced, the entry is stale and becomes a no-op.

This prevents stale entries from causing inconsistencies when
concurrent operations race in the log. Example:

```
Slot migration racing with failover:

1. Atomic slot migration starts: keys transferred from shard A to B.
2. Primary of shard A fails. FAILOVER entry is proposed.
3. Migration is rolled back (keys stay on shard A's new primary).
4. SLOT_CHANGE entry (assigning slot to shard B) was proposed before
   the failover and appears after FAILOVER in the log.
5. Without shard-epoch: SLOT_CHANGE applies, slot moves to shard B,
   but keys are on shard A. Data and ownership are out of sync.
6. With shard-epoch: FAILOVER bumped shard A's epoch. SLOT_CHANGE
   carries the old epoch, so it's a no-op. Slot stays on shard A.
```

Entries that should carry a shard-epoch:
- FAILOVER (bumps epoch of the shard)
- SET_REPLICA_OF (bumps epoch when changing shard membership)
- SLOT_CHANGE (checked against source and target shard epochs)

Entries that don't need a shard-epoch:
- NODE_FAIL / NODE_RECOVER (liveness, not topology)
- NODE_INFO / NODE_JOIN / NODE_FORGET (node-level, not shard-level)

## Future Work

- Pre-vote protocol to avoid term inflation from partitioned nodes.
- Log compaction / snapshotting for lagging followers.
- Persistence of Raft log to disk.
- Permanent learners: in large clusters, some nodes may stay as
  non-voting learners to reduce election and commit overhead. The
  NODE_JOIN entry would include the intended role (follower or learner).
- Leader transfer, in particular on graceful shutdown.
- Cluster merging via MEET: when two independently configured clusters
  are joined via CLUSTER MEET, the joining node's state (slots,
  replicas) should be carried in the HELLO/HI handshake and
  incorporated into the leader's log. This makes raft compatible with
  the existing admin workflow of configuring each node independently
  (ADDSLOTS, REPLICATE) and then connecting them with MEET.
- Non-blocking cluster commands: make MEET, ADDSLOTS, REPLICATE and
  other cluster mutation commands return OK immediately (like gossip)
  instead of blocking until committed. This enables MULTI/EXEC
  compatibility and works with existing admin tools (valkey-cli
  --cluster, third-party tools). Convergence is checked externally
  via CLUSTER INFO / CLUSTER NODES. Optionally, blocking variants
  can be offered for scripts that want commit confirmation.
- Automatic replica migration: when a primary becomes orphaned (no
  replicas), a replica from a shard with excess replicas should migrate
  to it. In raft, this is a leader-driven decision (the leader detects
  orphaned primaries and proposes SET_REPLICA_OF to move a replica).
