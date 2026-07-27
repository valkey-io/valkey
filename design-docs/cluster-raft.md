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
    Identifies the sender and completes the identification handshake.

MEET singleton|cluster
    Sent after HELLO on CLUSTER MEET-initiated connections. Declares
    the sender's cluster status: "singleton" means it's alone and
    "cluster" means that it's in a cluster with other nodes. The
    receiver decides based on its own status (see Cluster Formation
    below).

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

PRE_VOTE_REQ <candidate-id> <term> <last-log-index> <last-log-term>
    Request for speculative election. Sent by pre-candidates before
    incrementing currentTerm. The term is currentTerm+1.

PRE_VOTE <term> <granted>
    Response to PRE_VOTE_REQ. Indicates whether the node would grant
    a real vote in that term.

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

TIMEOUT_NOW <term>
    Sent by the leader to a follower to trigger immediate election
    (leader transfer). The recipient starts an election without
    waiting for the election timeout. Used during graceful shutdown.

PUBLISH <sharded> <chan_len> <msg_len>
    Propagates a Pub/Sub message to all cluster nodes. The channel
    and message data follow the header fields. <sharded> is 1 for
    shard channel messages, 0 for global.

MODULE <module_id> <type> <len>
    Propagates a module-to-module cluster message. The payload
    (binary, <len> bytes) follows on the next line. Delivered to
    module cluster message receivers.
```

The address string uses the nodes.conf format:
`ip:port@cport[,hostname][,aux=val]*`

## Roles

A new node starts up as leader of its own singleton cluster, meaning a
cluster with only one member. Singleton means that it's alone.

- **Joiner**: A singleton that stepped down to join another cluster.
  Cannot vote, propose, or form a cluster. Waits for AE from the
  leader to receive its NODE_JOIN entry. Reverts to singleton leader
  after 4× cluster-node-timeout if no AE arrives.
- **Follower**: Participates in elections and counts for quorum.
  Promoted from joiner when the node's NODE_JOIN entry is applied.
- **Pre-candidate**: Runs a pre-vote round without incrementing term.
  Reverts to follower if no quorum is reached within one election timeout
  (see [Election timeout](#election-timeout)).
- **Candidate**: Runs a formal election by sending VOTE_REQ after
  winning a pre-vote round.
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

NODE_FORGET <node-id> <shard-epoch>
    Remove a node from the cluster (CLUSTER FORGET). The epoch refers to
    leaving node's shard and guards against removing a node whose
    role changed (e.g., promoted to primary via a concurrent FAILOVER).

SLOT_CHANGE <source-node-id-or-dash> <source-epoch> <target-node-id-or-dash> <target-epoch> <range> [<range> ...]
    Assign or remove slot ownership. A dash means "no owner" (delete
    slots). Ranges use the nodes.conf format: "0-5460" or "5461".
    Carries two epochs (source and target shard). Shard epoch is not
    bumped because SLOT_CHANGE only moves slots, it doesn't change shard
    membership or leadership. Two slot migrations touching the same
    shard are safe to apply concurrently as they affect different slots.
    Bumping would serialize them unnecessarily. But validating is still
    needed to catch the case where a slot migration races against a
    failover that invalidated the shard's topology.

SET_REPLICA_OF <replica-id> <source-shard> <source-epoch> <primary-id-or-dash> <target-shard> <target-epoch>
    Set a node as replica of a primary (CLUSTER REPLICATE). A dash as
    primary means promote to primary. It carries two epochs because it
    involves two shards (source and target) and bumps the epoch of both
    shards on apply.

FAILOVER <replica-id> <primary-id> <shard-id> <shard-epoch>
    The replica takes over the primary's slots and becomes primary.
    The old primary becomes a replica of the new primary. This bumps the
    shard epoch on apply.

NODE_INFO <node-id> <address-string> <flags>
    Update node address and self-set flags. The address-string uses
    the nodes.conf format but excludes shard-id, which is not a
    property of the node but of the shard and is managed by NODE_JOIN
    and SET_REPLICA_OF. Flags is "nofailover" or "noflags". No shard epoch required here
    as it does not create any mutations.

NODE_FAIL <target-node-id> <proposer-node-id> <shard-epoch>
    Mark a node as failed. Proposed by a shard member via the
    replication-stream detector, or by the raft leader via AE_ACK
    for single-node shards and whole-shard-down fallback. The
    proposer is validated at propose time; the shard-epoch is
    validated at apply time to reject entries made stale by a
    concurrent shard change (see Failure Detection). Does not itself
    bump the shard epoch.

NODE_RECOVER <target-node-id> <proposer-node-id> <shard-epoch>
    Clear the FAIL flag. Proposed by the primary when a FAIL'd
    replica reappears in server.replicas, by the raft leader when
    a single-node shard sends AE_ACK, or by a replica when its
    FAIL'd primary's replication link is restored (only when
    failover is disabled). Validation mirrors NODE_FAIL.
```

Ranges in SLOT_CHANGE use the same format as nodes.conf: `0-5460` or
`5461`. A dash as source/target-id means "no owner" (delete slots) or "no
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
sent on the outbound link to the leader. The leader accepts
proposals with best effort pre-validations — it appends them to the log and
replicates them. Authoritative validation happens at apply time, where the apply
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

Pending proposals expire after 3× cluster-node-timeout. This covers
election detection (1–2× node-timeout), the election itself (~1×),
plus margin. If a proposal expires, the client receives a timeout
error.

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
leader after 4× cluster-node-timeout — intentionally longer than
the proposal expiry (3× cluster-node-timeout) to avoid a race: if
a re-proposed NODE_JOIN commits and AE is sent to the joiner, the
joiner must still be waiting. If it reverts first, it bumps its
term and rejects AE, potentially disrupting the cluster's leader.

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

Nodes are added and removed using NODE_JOIN and NODE_FORGET log
entries.

**Config takes effect on apply, not on append.** Quorum is computed
from `server.cluster->size`, which is updated when NODE_JOIN is
applied (after commit). This differs from Ongaro's single-server
approach (§4.1) where the new configuration takes effect when the
entry is appended to the log.

**Implications of config-on-apply:**

1. Multiple NODE_JOINs can be in flight simultaneously. The leader
   does not need to wait for one to commit before proposing the next.
   This enables fast cluster formation (all nodes invited in one
   batch).

2. NODE_JOIN is committed using the **old** quorum (the cluster size
   before the new node is added). For example, adding D to {A,B,C}
   requires 2 ACKs (majority of 3), not 3 ACKs (majority of 4).

3. If a leader election truncates uncommitted NODE_JOINs, no
   configuration rollback is needed — the config never took effect.
   Affected nodes time out as joiners and revert to singleton leaders.

4. Joint consensus is not needed. The quorum transitions atomically
   at apply time in a single step.

**Comparison to config-on-append (Ongaro §4.1):**

With config-on-append, the new quorum is used to commit the
membership entry itself. This guarantees the new node has the entry
before it takes effect, eliminating any window of reduced redundancy.
However, it adds complexity:

- Only one membership change can be in flight at a time (the leader
  must wait for commit before proposing the next change).
- If the entry is truncated after append but before commit, nodes
  must roll back to the previous configuration.
- The new node must be caught up before the entry is proposed
  (otherwise it can't ACK in time, stalling the commit).

**Safety of committing with old quorum — known limitation:**

After committing NODE_JOIN(F) to {A,B,C,D,E} with old quorum
(3 ACKs), the nodes that have the entry operate at size=6
(quorum=4), while nodes that don't have it still believe size=5
(quorum=3). This split in quorum views can violate safety:

Example (size 5→6): 3 nodes have the entry (size=6 view). The
other 3 nodes don't have it (size=5 view, quorum=3). If 2 of the
3 nodes with the entry crash, the 3 nodes without it can elect a
leader among themselves (they have quorum=3 under their view). That
leader overwrites the entry via log truncation. The committed
NODE_JOIN is lost.

For size 3→4: 2 nodes have the entry (size=4 view). The other 2
don't (size=3 view, quorum=2). If 1 node with the entry crashes,
the 2 without can elect a leader (quorum=2 under their view).

The root cause: nodes disagree on cluster size, leading to two
different quorum calculations coexisting. This is exactly what
Ongaro's config-on-append prevents — by making all nodes adopt the
new config immediately on append, they all agree on quorum.

In practice, this requires crashes within the brief window between
commit and AE delivery. The leader sends AE immediately after
commit, so the window is one network round-trip. Once AE is
delivered, all nodes agree on the new size and the split disappears.

**Worst case if this occurs:** The new node is orphaned — it stepped
down to joiner but its NODE_JOIN was lost. It reverts to singleton
leader after timeout. The CLUSTER MEET client already returned OK,
so the admin believes the node joined. No data loss or split brain
occurs (the node never had slots). The admin can detect the issue
via CLUSTER NODES and retry CLUSTER MEET.

**Accepted tradeoff:** We accept this limitation because:
- The window is extremely brief (one AE round-trip after commit).
- It enables fast cluster formation (multiple NODE_JOINs in flight,
  no need to catch up the new node before committing).
- It avoids the complexity of config-on-append or joint consensus.
- Kafka's KRaft uses the same approach.

**Future fix:** To eliminate this window entirely, use either
config-on-append (Ongaro §4.1, where the new configuration takes
effect on append and its quorum is used for commit) or joint
consensus (§4.3). Both require the new node to receive AE as a
non-voting member before its NODE_JOIN is committed, adding
complexity to the leader's replication logic.

## Failure Detection

There are two modes of failure detection, working in conjunction:

1. **Shard-level (replica ↔ primary):** shard members detect each
   other via the replication stream (the data-path), which is the
   authoritative health signal for a shard. Decentralized.

2. **Cluster-level (leader → whole shard):** the raft leader detects a
   shard via AE_ACK when no member of that shard is responsive.

The two modes are complementary: as long as *any* shard member is
alive, mode 1 owns detection for that shard (it has the real data-path
signal); mode 2 only fires when the whole shard has gone silent, where
the leader's AE_ACK is the only remaining signal.

### Cluster-level AE_ACK detector

The raft leader tracks `last_ack_time` per peer, updated on every
AE_ACK received. This serves two purposes:

1. **Quorum freshness** (`clusterRaftLeaderHasFreshQuorum()`): if a
   majority of peers have not ACK'd within `cluster-node-timeout`,
   the leader steps down to prevent split-brain writes.

2. **NODE_FAIL when a whole shard is silent**: if every member of a
   shard has failed to ACK within `cluster-node-timeout`, no member is
   alive to run the replication-stream detector, so the leader proposes
   NODE_FAIL for each timed-out member. A single-node shard also
   satisfies "every member is silent". When a node comes back and sends
   AE_ACK, the leader proposes NODE_RECOVER.

### Why replication-stream-based detection is also needed

For shard nodes, the actual liveness signal is replication stream
health. A primary and its replicas communicate via the replication
connection — if that connection is healthy, the shard is serving
traffic correctly regardless of whether the raft leader can reach
those nodes. This allows shard nodes to operate independently from
raft leader partitions and avoid false positives.

### Decentralized replication-stream detector

For shards with a primary and one or more replicas, failure is
detected via the data-path (replication stream). Asymmetric timeouts
between primary and replica is set to avoid failovers in case of partition
between primary and replica. This will also be later used as primaryship
lease for sync replication.

**Primary-side detection:** The primary iterates its cluster-state
replicas and checks their health via two paths:

1. *Connected replica (in `server.replicas`, state ONLINE)*: checks
   `repl_ack_time` freshness. Timeout = `cluster-node-timeout`.

2. *Disconnected replica (not in `server.replicas`)*: a crash causes
   immediate TCP RST and client removal. The primary tracks the first
   observation time (`repl_unconnected_since`) and proposes NODE_FAIL
   after `max(REPL_CONNECT_GRACE_PERIOD_MS - (5 seconds), server.node_timeout)`.
   The grace period allows a healthy replica time to complete its replication handshake after a FAILOVER when replicas connect to the new primary.

**Replica-side detection:** The replica checks `myself->replicaof`
from cluster state and monitors replication health via
`server.primary->last_interaction` (link up but silent) or
`server.repl_down_since` (link dropped). Timeout = `1.5 ×
cluster-node-timeout`. The asymmetric (longer) timeout ensures the
primary proposes NODE_FAIL first; the replica only proposes if the
primary failed to do so (e.g., the primary itself crashed).

**Timeout summary:**

| Path | Timeout | Rationale |
|------|---------|-----------|
| Primary: connected replica silent | cluster-node-timeout | Replica should ACK within this window |
| Replica: primary silent/down | 1.5 × node-timeout | Let primary propose first |

**Recovery:**
- Primary-side: proposes NODE_RECOVER when a FAIL'd replica is present
  in `server.replicas` and `repl_ack_time` is fresh.
- Replica-side: proposes NODE_RECOVER only when failover is disabled
  (`cluster-replica-no-failover`). Normally, a replica with a FAIL'd
  primary should failover, not recover.

### NODE_FAIL / NODE_RECOVER validation

A NODE_FAIL/NODE_RECOVER entry carries `<target> <proposer>
<shard-epoch>` and is validated in two places:

- **Propose time (leader):** the proposer must be legitimate — the raft
  leader, or the target's actual primary/replica peer.
  An already-FAIL'd proposer is a no-op. This check runs once,
  during leader pre-validation.

- **Apply time (NODE_FAIL only):** the shard epoch is re-checked (not
  bumped). A concurrent FAILOVER bumps the epoch, so a stale `NODE_FAIL` carrying the old epoch is rejected. Without this, after a node R fails over node P, the stale `NODE_FAIL R` from the old primary P could mark the freshly promoted R as FAILED.

- **Apply time (NODE_RECOVER):** no epoch check because clearing a FAIL flag is the safe direction as stale recovery is either a no-op or gets re-FAILed on the next detector tick. Epoch-gating it would only delay a legitimate recovery, an availability regression.

### Election timeout

The election timeout is based on `cluster-node-timeout`:
`[T, 2T)` where T = max(cluster-node-timeout, 1000ms). The heartbeat
interval is `election_timeout / 10`, minimum 100ms.

#### Pre-vote and term inflation

Ongaro's dissertation (§9.6) introduces pre-vote to reduce disruption
when a partitioned node rejoins, but it does not spell out the full
election-timeout state machine that production implementations need.
In classic Raft, every election timeout on a follower increments
`currentTerm` and sends RequestVote. A node isolated from the quorum
will therefore bump its term on every timeout even though it can never
win — when it rejoins, its higher term forces the real leader to step
down.

Pre-vote breaks this cycle by separating *connectivity probing* from
*term commitment*. A pre-vote round sends `PRE_VOTE_REQ` with a
*speculative* term of `currentTerm + 1` but does **not** change local
`currentTerm`, `votedFor`, or the on-disk vars line. Only after a
majority grants `PRE_VOTE` does the node call `StartElection()`, which
increments `currentTerm`, votes for itself, and persists the new term
before broadcasting `VOTE_REQ`.

Nodes deny `PRE_VOTE` and `VOTE_REQ` when they still consider the
current leader alive: `last_heartbeat` is younger than one election
timeout. The same log-up-to-date check applies to both message types.

#### Timeout state machine

```
                +----------+
                | FOLLOWER |
                +----------+
                   |    ^
  election timeout |    | pre-vote timeout
                   v    |
              +---------------+
              | PRE_CANDIDATE |
              +---------------+
                   |    ^
   quorum PRE_VOTE |    | election timeout
                   v    |
               +-----------+
               | CANDIDATE |
               +-----------+
                   |
       quorum VOTE |
                   v
               +--------+
               | LEADER |
               +--------+
```

The non-obvious transition is `CANDIDATE -> PRE_CANDIDATE`. After a
split vote, the node has already bumped `currentTerm`; immediately
starting another election would bump it again even if the node cannot
reach a majority. Re-entering pre-vote first confirms that a quorum is
reachable before committing to another term increment.

Pre-vote is an in-memory probe. It uses the speculative term
`currentTerm + 1`, but it does not update or persist `currentTerm` or
`votedFor`. Only `StartElection()` persists a new term and self-vote,
and only after a majority has granted pre-vote.

The leader lease check is shared by pre-vote and real vote requests:
nodes that have heard from the leader within one election timeout deny
both. This keeps a reachable leader stable while still allowing a node
whose pre-vote timed out to fall back to follower and retry later
without inflating its term.

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

Raft state is persisted in `nodes.conf`. The file has three sections:

1. **Node lines** — the cluster state snapshot (nodes, slots, replication
   topology) as of `lastApplied`.
2. **Vars line** — `currentTerm`, `lastApplied`, `votedFor`, `raftLeader`.
3. **Log lines** — entries with index > `lastApplied` appended at the end.

Only `currentTerm`, `votedFor`, and the log require persistence for
safety; `commitIndex` is rediscovered from the leader after restart
(Raft paper §5.2, Figure 2).

Log line format:

    log <index> <term> <type> <data>

### Full rewrite

A full rewrite (atomic write to temp file + rename + fsync) is triggered
when:

- `currentTerm` changes (step-down on higher term).
- `votedFor` changes (granting a vote or starting an election).
- An applied entry affects `myself` (SLOT_CHANGE, SET_REPLICA_OF,
  FAILOVER, NODE_JOIN promotion from learner).

A full rewrite updates the snapshot (node lines reflect the current
applied state), updates `lastApplied` in vars, and writes only
unapplied log entries in the tail.

### Append-only

When new log entries arrive (from AE or local proposals), they are
appended to the end of the file with a single batched write + fsync
(deferred to `beforeSleep`). No full rewrite is needed.

**Safety invariant:** The fsync must complete before the AE_ACK reaches
the leader. This is enforced by deferring the success AE_ACK: the AE
handler sets `todo_send_ae_ack` instead of sending immediately, and
`beforeSleep` sends the ACK only after persistence completes. If fsync
is ever moved to a background thread, the ACK must be deferred until
the background fsync completes.

### Startup

On load:

1. Node lines are parsed → cluster state restored (snapshot).
2. Vars line is parsed → `currentTerm`, `votedFor`, `lastApplied` restored.
   `commit_index` is set to `lastApplied`.
3. Log lines are parsed → entries added to the in-memory log.
4. If the node is a replica, replication is started.

The leader will send AE after reconnection, updating `commit_index`.
Entries between `lastApplied + 1` and the new `commit_index` are then
applied.

### Integrity checks

Each log line includes a CRC64 checksum as its first field:

    log <crc64hex> <index> <term> <type> <data...>

The CRC covers the payload (everything after the checksum field). On
load, each line is validated:

- **CRC matches** → accept the entry (regardless of trailing `\n`)
- **Invalid + no trailing `\n`** → discard (last line, short write from
  crash during append; safe because AE_ACK is sent only after fsync)
- **Invalid + trailing `\n`** → fatal (complete line is corrupted)
- **Index gap between valid entries** → fatal (missing entry)

The snapshot section (node lines + vars) is protected by a separate
`crc` line written between vars and log lines.

After loading, a full rewrite is always scheduled to produce a clean
file (removes any incomplete trailing line).

### File format

The node lines share the same format as the gossip protocol (code
reuse), but the vars and log lines are raft-specific. The file is not
compatible between protocols — switching from gossip to raft (or vice
versa) requires removing nodes.conf.

## Shard Epoch

Raft ensures entries are applied in a total order, but ordering alone
is not sufficient to prevent stale mutations from corrupting cluster
state. When concurrent operations target the same shard (e.g., a slot
migration racing with a failover), a committed entry may carry
assumptions about shard topology that are no longer true by the time
it is applied. Without additional application-level state to fence
against these stale updates, the apply logic can produce
inconsistencies — such as moving a slot to a node that no longer owns
the corresponding keys.

A shard-epoch is a per-shard monotonically increasing counter stored
in the raft protocol state. It is bumped each time membership or
leadership of the shard changes. Such entries include the shard's
current epoch at proposal time. Epoch is validated at prepare time
and at apply time. If the epoch has advanced past the value in the entry,
the entry is stale and is ignored.

### Example: slot migration racing with failover

```
1. Slot migration starts: keys transferred from shard A to shard B.
2. Primary of shard A fails. FAILOVER entry is proposed.
3. Migration is rolled back (keys stay on shard A's new primary).
4. SLOT_CHANGE entry (assigning slot to shard B) was proposed before
   the failover and appears after FAILOVER in the log.
5. Without shard-epoch: SLOT_CHANGE applies, slot moves to shard B,
   but keys are on shard A. Data and ownership are out of sync.
6. With shard-epoch: FAILOVER bumped shard A's epoch. SLOT_CHANGE
   carries the old epoch, so it's a no-op. Slot stays on shard A.
```

### Validation

Epoch validation happens at two points:

1. **Pre-validation on the leader** — before appending to the log.
   This is a best-effort optimization that rejects obviously stale
   proposals early, saving log space and replication bandwidth. It
   performs a read-only check without bumping the epoch.

2. **Apply-time validation** — the authoritative check. Each apply
   function validates the entry's epoch against the current shard
   epoch. On match (or epoch 0 for a new shard), the epoch is bumped
   and the entry is applied. On mismatch, the entry is a no-op and
   the error is propagated to the caller's callback.

### Retry on stale epoch

Proposals rejected due to a stale shard epoch are automatically retried
with a fresh epoch (up to 5 attempts):

- **SET_REPLICA_OF / NODE_FORGET / FAILOVER (force) / SLOT_CHANGE** —
  the proposal is rebuilt with current epoch(s) and re-submitted.

- **Automatic failover** — if the FAILOVER proposal is rejected, the
  failover is re-scheduled (via `todo_schedule_failover`) as long as
  the primary is still failed. The next attempt uses the current epoch.
  For automatic failover, no cap on retry attempt to avoid leaderless shard.

Only `STALE_SHARD_EPOCH_REJECTION_MSG` triggers retry. Other errors
(format errors, invalid state) are forwarded to the client immediately.

When the leader rejects a forwarded proposal at pre-validation, it sends
a `REJECT <reason> <type> <data...>` message back. The reason is a token:
`conflict` (stale epoch, retryable) or `syntax` (permanent error).

### Entries that don't carry an epoch

- NODE_FAIL / NODE_RECOVER 
- NODE_INFO, NODE_JOIN

## Leader Transfer

On graceful shutdown, the leader sends a `TIMEOUT_NOW` message to the
follower with the highest `match_index`. The message is flushed
synchronously to the socket before shutdown proceeds. The target
immediately starts a real election (skipping pre-vote) without waiting
for the election timeout.
(See Ongaro dissertation §3.10.)

Nodes in handshake or with the MEET flag are excluded as transfer
targets.

## Future Work

- Minority partition detection and leader step-down (prevents append
  entries, especially don't trigger primary/replica failovers in a
  minority partition).
- Log compaction / snapshotting for lagging followers.
- Learners (non-voting members): reduces the risk for split-vote for
  leader election in large clusters and reduces commit overhead.
- Leader transfer on CLUSTER FORGET where the target is the leader.
- Safety regarding membership changes (use new quorum).
- Cluster merging via MEET: when two independently configured clusters
  are joined via CLUSTER MEET, the clusters should merge, either
  atomically or not.
- `valkey-cli --cluster` tooling compatibility (uses MULTI internally in
  some cases, which doesn't work with blocking admin commands).
