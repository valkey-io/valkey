--------------------------- MODULE ValkeyRaftBase ------------------------------
(***************************************************************************)
(* Protocol-level model of Valkey cluster-v2 Raft (src/cluster_raft.c):     *)
(* shared base.                                                             *)
(* v0 action bodies (G1, G2, G4, faults) are written against the refs;      *)
(* staged ones are stubs (FALSE). Invariants are in ValkeyRaft.             *)
(* Correspondence notes, per action, sit above each body: "code:" what it   *)
(* copies, "abstr:" what it leaves out or over-approximates.                *)
(*                                                                          *)
(* Refs: function names in src/cluster_raft.c (cluster-v2 at f482dea,       *)
(* 2026-11-20) and section titles in design-docs/cluster-raft.md, written   *)
(* design "Title". No line numbers: they drift.                             *)
(* Base: ongardie/raft.tla. Primer: claude/valkey-raft-operations-primer.md *)
(* Lean model review: claude/valkey-lean-model-review.md                    *)
(*                                                                          *)
(* NODES ARE BLACK BOXES. A node's interface is:                            *)
(*   inputs:  received messages, admin commands, timeouts, crash/restart;   *)
(*   outputs: sent messages, and the state the invariants read (term,       *)
(*            vote, log, commit index, voter set, role).                    *)
(* Each action is one handler plus its beforeSleep flush, taken             *)
(* atomically. The event loop, todo_* flags, late-bound payloads and IO     *)
(* threads are NOT modelled. Bugs that live there (B-01: a success ACK      *)
(* built at flush time) are invisible here by construction.                 *)
(*                                                                          *)
(* REFINEMENT TARGET. The later node-level spec splits each action here     *)
(* into Handle / Flush / Cron steps with todo_* flags. It should refine     *)
(* this spec under a mapping that hides the flags: every message it sends   *)
(* must be one some action here could send. Keep this spec's variables      *)
(* and message records stable so that mapping stays simple.                 *)
(*                                                                          *)
(* Scope (v0): G1 election, G2 replication, G4 voter changes. All servers   *)
(* known from the start (no MEET/JOINER); learners are Server \ voters. No  *)
(* time (leases and freshness step-down nondeterministic). No log trimming  *)
(* or INSTALL_SNAPSHOT. No PROPOSE forwarding: the leader appends directly. *)
(*                                                                          *)
(* MODULE LAYOUT. ValkeyRaftBase (this file): constants, flags, conventions, *)
(* message kinds, variables, helpers, apply logic, shared state-change      *)
(* operators, Init, TypeOK. One module per group, each EXTENDS only this    *)
(* base and exports NextGn: RaftElection (G1, incl. leader transfer),       *)
(* RaftReplication (G2, incl. proposals and snapshots), RaftJoin (G3),      *)
(* RaftVoters (G4), RaftFailover (G5), RaftFaults. ValkeyRaft (main) EXTENDS *)
(* them all and defines Next, Spec and the invariants. Rule: anything used  *)
(* by two groups lives here, so imports stay a tree (SANY rejects cycles).  *)
(* Staged parts sit in their group's module under STAGED. Their variables   *)
(* are declared here already (group tuples below), so every action states   *)
(* a full UNCHANGED now and enabling a group is one line in Next.           *)
(* Not modelled at all: HELLO/HI (link identity; channels here are          *)
(* authenticated, msource is the sender), FAILOVER_PREPARE/FAILOVER_PAUSED  *)
(* and REPL_OFFSETS (primary-replica data plane, not Raft).                 *)
(***************************************************************************)
EXTENDS Naturals, FiniteSets, Sequences, TLC

\* Server ids are TYPED model values: names of the form s_<id> (TLC: a name
\* "T_..." with one letter T gives the value type T). TLC raises an error
\* when an expression compares one with anything but a server or an
\* ordinary model value, so a server mixed up with an index, a term or a
\* string fails loudly instead of comparing FALSE. Value and Nil stay
\* ORDINARY model values (v1, Nil): entry args mix Value and Server, and
\* typed values of two different types may not be compared at all.
CONSTANTS
    Server,       \* SUBSET of typed model values: node ids, {s_1, s_2, ...}
    Value,        \* SUBSET of ordinary model values: opaque client entries
                  \* (stand-in for SLOT_CHANGE etc.), e.g. {v1}
    InitVoters,   \* SUBSET Server: voters in the initial applied state; the
                  \* rest of Server are learners
    InitLeader,   \* Server \cup {Nil}: a voter that starts as leader of term
                  \* 1 (shortens calibration traces; matches the Lean model's
                  \* init), or Nil for a leaderless start
    Nil           \* a model value outside Server and Value: "no node"

\* Bounds for TLC, used by StateConstraint.
CONSTANTS MaxTerm, MaxLogLen   \* Nat, Nat
\* Bound on messages in flight (sum of bag counts), used by StateConstraint.
\* Leaders may send AE at any time, so without it the bag is unbounded.
CONSTANT MaxMessages           \* Nat

(***************************************************************************)
(* Deviation flags. Each switches one difference from paper Raft. The       *)
(* CODE value is the behaviour at f482dea; the PAPER value is the fix.      *)
(* All are black-box observable: they change message contents, handler      *)
(* outcomes or the applied state, not event-loop scheduling.                *)
(*                                                                          *)
(*  flag                 code   paper  what                                 *)
(*  AckLastIndex         TRUE   FALSE  AE_ACK reports raftLogLastIndex      *)
(*                                     (clusterRaftSendAppendEntries-       *)
(*                                     Response), not prev + Len(entries).  *)
(*                                     B-10, Lean A1                        *)
(*  FollowerCommitLast   TRUE   FALSE  follower commit = min(leaderCommit,  *)
(*                                     lastIndex) (clusterRaftProcess-      *)
(*                                     AppendEntries), not the last new     *)
(*                                     entry. B-04, Lean A1                 *)
(*  ConfigOnAppend       FALSE  TRUE   quorum from the applied voter set    *)
(*                                     (clusterRaftQuorum) vs the latest    *)
(*                                     config in the log (dissertation      *)
(*                                     4.1). Lean A2                        *)
(*  ConfigGuard          "None" (see   when the leader may propose a voter  *)
(*                       below)        change. B-09, #4392, #3926           *)
(*  NoopOnElection       TRUE   TRUE   new leader appends NOOP              *)
(*                                     (clusterRaftBecomeLeader). FALSE     *)
(*                                     reproduces Lean F1 / pre-#4015       *)
(*  DelVoterReadsLeader  TRUE   FALSE  DEL_VOTER apply is skipped when the  *)
(*                                     target is the applier's rs->leader   *)
(*                                     (clusterRaftApplyDelVoter). Lean A4, *)
(*                                     primer G4                            *)
(*  LeaderSelfCount      TRUE   FALSE  the AE_ACK commit scan counts the    *)
(*                                     leader even when it is not in its    *)
(*                                     own voter set ("matches = 1",        *)
(*                                     clusterRaftProcessAppendEntries-     *)
(*                                     Response). Reachable with            *)
(*                                     ConfigOnAppend or with               *)
(*                                     DelVoterReadsLeader = FALSE          *)
(*                                                                          *)
(* ConfigGuard modes:                                                       *)
(*   "None"     code at f482dea: no guard; any number of voter changes in   *)
(*              flight.                                                     *)
(*   "NoPending" refuse while the leader's log holds a voter change beyond  *)
(*              its commit index (the Lean model's oneConfigInFlight).      *)
(*              With ConfigOnAppend it misses Ongaro's 2015 case: a new     *)
(*              leader that never received the old leader's uncommitted     *)
(*              change sees none, while followers holding it already use    *)
(*              it. Under on-apply that change never takes effect unless    *)
(*              committed, so whether it matters there is for TLC (C9).     *)
(*   "Etcd"     etcd/raft: on becoming leader, pendingConfIndex := last log *)
(*              index (before the no-op); after accepting a change,         *)
(*              pendingConfIndex := its index. Refuse while                 *)
(*              pendingConfIndex > applied (= commitIndex here). Blocks     *)
(*              until everything the leader held when elected is applied.   *)
(*              Proposed in #3926 discussion: on-apply + old-quorum commit  *)
(*              + this guard.                                               *)
(*                                                                          *)
(* Deferred, not black-box or needs time: B-05 (needs a timer               *)
(* abstraction; liveness only), late-bound ACK B-01 (node spec). The vote   *)
(* lease is a nondeterministic choice in HandleRequestVote.                 *)
(***************************************************************************)
CONSTANTS AckLastIndex, FollowerCommitLast, ConfigOnAppend,
          ConfigGuard, NoopOnElection, DelVoterReadsLeader, LeaderSelfCount

\* ConfigGuard modes (enum). The cfg sets ConfigGuard to one of these
\* strings; ASSUME checks it. Everywhere else, use the names.
GuardNone      == "None"
GuardNoPending == "NoPending"
GuardEtcd      == "Etcd"
ConfigGuards == {GuardNone, GuardNoPending, GuardEtcd}

ASSUME /\ InitVoters \subseteq Server /\ InitVoters /= {}
       /\ Nil \notin Server \cup Value
       /\ InitLeader \in InitVoters \cup {Nil}
       /\ MaxTerm \in Nat /\ MaxLogLen \in Nat /\ MaxMessages \in Nat
       \* Vacuity guards: bounds below these make elections or appends
       \* unreachable, and every invariant then passes for nothing.
       \* MaxTerm >= 2: every election raises the term. MaxMessages: one
       \* pre-vote or vote broadcast to the other voters must fit.
       /\ MaxTerm >= 2 /\ MaxLogLen >= 1
       /\ MaxMessages >= Cardinality(InitVoters) - 1
       /\ \A f \in {AckLastIndex, FollowerCommitLast, ConfigOnAppend,
                    NoopOnElection, DelVoterReadsLeader, LeaderSelfCount} :
              f \in BOOLEAN
       /\ ConfigGuard \in ConfigGuards

\* rs->role (enum raftRole). Joiner is staged (G3): a singleton that stepped
\* down to be added to another cluster (clusterRaftSingletonStepDown, design
\* "Roles").
Joiner       == "Joiner"
Learner      == "Learner"
Follower     == "Follower"
PreCandidate == "PreCandidate"
Candidate    == "Candidate"
Leader       == "Leader"
Roles == {Joiner, Learner, Follower, PreCandidate, Candidate, Leader}

(***************************************************************************)
(* ENUMS. TLA+ has no enum type: each kind of tag is a set of distinct     *)
(* strings, and every use goes through a name defined here (roles above,   *)
(* guard modes above). A typo in a name is an undefined symbol that SANY    *)
(* rejects; a typo in a raw string is a comparison that is never true, so  *)
(* the action silently never fires. So string literals appear only in a    *)
(* definition of the form Name == "literal".                               *)
(* Values stay strings so TLC traces read like the wire protocol.          *)
(***************************************************************************)

\* Entry types (enum raftEntryType). v0 uses the first four. Staged:
\* NODE_JOIN split by mode, since the mode decides whether it changes quorum.
\* SLOT_CHANGE, SET_REPLICA_OF, FAILOVER, NODE_INFO, NODE_FAIL, NODE_RECOVER
\* stay folded into EntData until application state exists (G5 below).
EntNoop        == "Noop"
EntData        == "Data"
EntAddVoter    == "AddVoter"
EntDelVoter    == "DelVoter"
EntJoinLearner == "NodeJoinLearner"
EntJoinVoter   == "NodeJoinVoter"
EntForget      == "NodeForget"
EntryTypes == {EntNoop, EntData, EntAddVoter, EntDelVoter,
               EntJoinLearner, EntJoinVoter, EntForget}
\* Quorum-changing entries (design "Membership changes"). Staged additions
\* when G3 is enabled: EntJoinVoter (self-join only) and EntForget OF A
\* VOTER. A NodeForget of a learner is not a config change, so membership in
\* this set will depend on the target, not only the type.
ConfigTypes == {EntAddVoter, EntDelVoter}

\* Message types: the wire names clusterRaftProcessMessage dispatches on.
MsgPreVoteReq       == "PRE_VOTE_REQ"
MsgPreVote          == "PRE_VOTE"
MsgVoteReq          == "VOTE_REQ"
MsgVote             == "VOTE"
MsgAE               == "AE"
MsgAEAck            == "AE_ACK"
MsgTimeoutNow       == "TIMEOUT_NOW"
MsgMeet             == "MEET"             \* staged from here on
MsgAddMe            == "ADD_ME"
MsgWelcome          == "WELCOME"
MsgMeetRejected     == "MEET_REJECTED"
MsgPropose          == "PROPOSE"
MsgReject           == "REJECT"
MsgInstallSnapshot  == "INSTALL_SNAPSHOT"

\* REJECT reasons (REJECT_WIRE_* tokens in cluster_raft.c). Staged.
RejConflict == "conflict"
RejRejected == "rejected"
RejTransfer == "transfer"
RejectReasons == {RejConflict, RejRejected, RejTransfer}

\* Argument of an entry: a Value for Data, a Server for membership and voter
\* entries, Nil for Noop. Shard epochs (NODE_FORGET's second field) are app
\* state, omitted.
EntryArgs == Value \cup Server \cup {Nil}

\* raftLogEntry: a log entry. Record; always build with all three fields.
Entries == [term : Nat, type : EntryTypes, arg : EntryArgs]

\* An entry before it is in any log (PROPOSE, REJECT, pending proposals):
\* no term yet. The leader adds the term when it appends.
Proposals == [type : EntryTypes, arg : EntryArgs]

(***************************************************************************)
(* STATE SHAPES. Conventions every variable, message and action follows.    *)
(* Each VARIABLE below states its type, its Init value and whether it       *)
(* survives Restart.                                                        *)
(*                                                                          *)
(* S1. Per-server state is a total function [Server -> T], allocated in     *)
(*     Init for EVERY server, members or not (G3 adds non-members with a    *)
(*     fresh value, not a missing key). Read X[i] as "i's copy". Replicated *)
(*     state (voters, members, app state) is one copy per server; copies    *)
(*     agree only through the log. Never keep a single global copy.         *)
(* S2. Leader bookkeeping is [Server -> [Server -> T]]: X[i][j] is leader   *)
(*     i's record of peer j. Allocated for all i and j; meaningful only     *)
(*     while role[i] = Leader; reset by BecomeLeader. Invariants that read  *)
(*     it guard with role[i] = Leader. X[i][i] exists but is not read (the  *)
(*     leader counts itself separately).                                    *)
(* S3. "No node" is Nil, type Server \cup {Nil} (votedFor, leader,          *)
(*     transferTarget). "No nodes" is {}, type SUBSET Server. Never put     *)
(*     Nil inside a set; never use {} for an optional single node.          *)
(* S4. Collections: SUBSET T when unordered with no duplicates; Seq(T) when *)
(*     ordered (1-based, see LOG INDEXING CONVENTION); a bag [T -> Nat]     *)
(*     with counts >= 1 only for `messages`. Where the code keeps a list    *)
(*     but order and duplicates do not matter (pending proposals, deferred  *)
(*     meets), use a set and say so at the variable.                        *)
(* S5. Records have one fixed field set per kind: Entries, Proposals, and   *)
(*     one record set per message kind (Network section). Build them with   *)
(*     every field: a record missing a field is a different value, so it    *)
(*     fails TypeOK and never matches in equality or set membership.        *)
(* S6. Vote tallies are Nat counters, as in the code (B-07). raft.tla uses  *)
(*     SUBSET Server, which cannot double-count. With no duplicating        *)
(*     network the two agree; switch to sets if duplication is added.       *)
(* S7. History (ghost) variables, e.g. `committed`, are written by actions  *)
(*     and read only by invariants, never by an action's guard.             *)
(* S8. Sets of sets appear only as SUBSET Server values inside a function   *)
(*     (voters[i], members[i]) or a message (mmembers, mvoters). A quorum   *)
(*     of i is a Q \subseteq EffVoters(i) with Cardinality(Q) >=            *)
(*     QuorumSize(i). QuorumOverlap will quantify over SUBSET SUBSET        *)
(*     Server, so keep Server small (<= 5) when it is enabled.              *)
(***************************************************************************)

(***************************************************************************)
(* LOG INDEXING CONVENTION. Every action and invariant follows these rules. *)
(*                                                                          *)
(* 1. Raft indices are 1-based. Index 0 means "no entry": empty log,        *)
(*    nothing committed, prevLogIndex before the first entry. Terms start   *)
(*    at 1; term 0 means "no entry / unknown" (design "Conceptual           *)
(*    model"; raftLogGet returns NULL for 0).                               *)
(* 2. log[i][k] IS the entry at Raft index k. Never re-index, never store   *)
(*    a window. The code stores a window, rs->log[k - base] with            *)
(*    base = rs->log[0]->index (raftLogGet); that offset is a code detail and *)
(*    stays out. Trimming will be a firstIndex[i] watermark over the full   *)
(*    sequence (see Staged variables), so this rule survives it.            *)
(* 3. LastIndex = Len(log[i]); LastTerm(<< >>) = 0. Code falls back to      *)
(*    last_applied / snapshot_term when its window is empty                 *)
(*    (raftLogLastIndex, raftLogLastTerm); with rule 2 the full sequence    *)
(*    gives the same values.                                                *)
(* 4. Term at index k: TermAt(log[i], k), 0 for k = 0. Use it for           *)
(*    prevLogTerm; never write log[i][k].term where k may be 0. Code:       *)
(*    raftLogTermAt also answers at last_applied (snapshot_term)            *)
(*    and at log_trim_index (log_trim_term), and 0 elsewhere off-window.    *)
(* 5. commitIndex[i] = last_applied = count of applied entries; entries     *)
(*    1..commitIndex[i] are applied. Must satisfy commitIndex <= Len(log),  *)
(*    checked by CommitWithinLog. Invariants are written TOTAL: every       *)
(*    index they read is bounded by Len(log) too, so a broken bound shows   *)
(*    up as CommitWithinLog failing, never as an evaluation error, whatever *)
(*    order the cfg lists them in.                                          *)
(* 6. Leader bookkeeping: nextIndex[i][j] <= Len(log[i]) + 1 and            *)
(*    matchIndex[i][j] <= Len(log[i]), checked by IndexBounds. nextIndex    *)
(*    can be 0 (after Restart, never reset; see nextIndex). An AE sent      *)
(*    to j has mprevLogIndex = nextIndex[i][j] - 1,                         *)
(*    mprevLogTerm = TermAt(log[i], mprevLogIndex), and its entries start   *)
(*    at index mprevLogIndex + 1.                                           *)
(* 7. Every index in a message and in `committed` is an absolute Raft       *)
(*    index (rule 2), never a position in a window or a batch.              *)
(* 8. Ranges are inclusive: a..b is empty when a > b. FoldVoters and        *)
(*    ConfigInFlight rely on (commitIndex + 1)..Len being empty when        *)
(*    everything is committed.                                              *)
(*                                                                          *)
(* Hot spots in the code where these conventions are easy to break: the     *)
(* window offset (raftLogGet), the trim boundary (log_trim_index vs         *)
(* last_applied, raftLogTermAt), snapshot index = last_applied (design      *)
(* "Conceptual model"), and AE_ACK's mlastIndex (AckLastIndex): a           *)
(* follower whose log is longer than the leader's reports an index past     *)
(* the leader's last entry, which IndexBounds catches directly.             *)
(***************************************************************************)

--------------------------------------------------------------------------------
\* Network (the black-box boundary)

\* Bag of messages (Message -> count), as in raft.tla. Unordered and lossy;
\* separate request and response messages. This is wider than the Lean
\* model, whose AE and ACK are one atomic step and never reordered.
\* No duplication action: Valkey runs over TCP, and its vote tallies are
\* counters, so a duplicating network would produce false positives.
\* Message kinds; mtype is the wire name dispatched by
\* clusterRaftProcessMessage. Every message has mtype, msource, mdest; Raft
\* messages also have mterm. Builder function in cluster_raft.c in brackets.
\* [clusterRaftSendPreVoteRequest, clusterRaftSendRequestVote]
VoteReqMsgs ==
    [mtype : {MsgPreVoteReq, MsgVoteReq}, msource : Server, mdest : Server,
     mterm : Nat, mlastLogIndex : Nat, mlastLogTerm : Nat]
\* [clusterRaftSendPreVoteResponse, clusterRaftSendVoteResponse]
VoteRespMsgs ==
    [mtype : {MsgPreVote, MsgVote}, msource : Server, mdest : Server,
     mterm : Nat, mgranted : BOOLEAN]
\* [clusterRaftSendAppendEntries]. mentries: Seq(Entries), possibly empty
\* (heartbeat); its first entry sits at index mprevLogIndex + 1.
AEMsgs ==
    [mtype : {MsgAE}, msource : Server, mdest : Server, mterm : Nat,
     mprevLogIndex : Nat, mprevLogTerm : Nat, mentries : Seq(Entries),
     mcommitIndex : Nat]
\* [clusterRaftSendAppendEntriesResponse]. mlastIndex = AckIndex(...): the
\* follower's Len(log) if AckLastIndex, else mprevLogIndex + Len(mentries)
\* (raft.tla's mmatchIndex). Also the reply to INSTALL_SNAPSHOT.
AEAckMsgs ==
    [mtype : {MsgAEAck}, msource : Server, mdest : Server, mterm : Nat,
     msuccess : BOOLEAN, mlastIndex : Nat]
\* [clusterRaftMaybeSendTimeoutNow, clusterRaftPrepareShutdown]
TimeoutNowMsgs ==
    [mtype : {MsgTimeoutNow}, msource : Server, mdest : Server, mterm : Nat]

MessagesV0 == VoteReqMsgs \cup VoteRespMsgs \cup AEMsgs \cup AEAckMsgs
                  \cup TimeoutNowMsgs

\* Staged kinds. MEET, the bare replies, PROPOSE and REJECT carry no term.
\* [clusterRaftSendMeet]
MeetMsgs ==
    [mtype : {MsgMeet}, msource : Server, mdest : Server, msingleton : BOOLEAN]
\* [clusterRaftSendBare]
BareMsgs ==
    [mtype : {MsgAddMe, MsgWelcome, MsgMeetRejected},
     msource : Server, mdest : Server]
\* [clusterRaftSendProposal]
ProposeMsgs ==
    [mtype : {MsgPropose}, msource : Server, mdest : Server,
     mentry : Proposals]
\* [clusterRaftProcessPropose]
RejectMsgs ==
    [mtype : {MsgReject}, msource : Server, mdest : Server,
     mreason : RejectReasons, mentry : Proposals]
\* [clusterRaftSendInstallSnapshot]. mlastIndex/mlastTerm: the sender's
\* commitIndex and the term there. mmembers, mvoters: the sender's applied
\* topology (its node lines). mprefix: GHOST, the sender's log[1..mlastIndex],
\* not on the wire; it keeps rule 2 of the indexing convention true after
\* install.
SnapshotMsgs ==
    [mtype : {MsgInstallSnapshot}, msource : Server, mdest : Server,
     mterm : Nat, mlastIndex : Nat, mlastTerm : Nat,
     mmembers : SUBSET Server, mvoters : SUBSET Server,
     mprefix : Seq(Entries)]

MessagesStaged == MeetMsgs \cup BareMsgs \cup ProposeMsgs \cup RejectMsgs
                      \cup SnapshotMsgs

\* Kinds TypeOK accepts: all of them, since staged variables are declared.
Messages == MessagesV0 \cup MessagesStaged

(***************************************************************************)
(* CONSTRUCTORS. Build every entry, proposal and message through these,    *)
(* never with a record literal. A record with a                            *)
(* missing or misspelt field is a different value: it equals nothing,      *)
(* matches no handler, and only TypeOK notices, and only once reached (S5).*)
(* Arguments are positional in the order of the record sets above.         *)
(***************************************************************************)
MkEntry(t, ty, a)    == [term |-> t, type |-> ty, arg |-> a]
MkProposal(ty, a)    == [type |-> ty, arg |-> a]

\* ty \in {MsgPreVoteReq, MsgVoteReq}
VoteReqMsg(ty, i, j, t, lastIdx, lastTerm) ==
    [mtype |-> ty, msource |-> i, mdest |-> j, mterm |-> t,
     mlastLogIndex |-> lastIdx, mlastLogTerm |-> lastTerm]
\* ty \in {MsgPreVote, MsgVote}
VoteRespMsg(ty, i, j, t, granted) ==
    [mtype |-> ty, msource |-> i, mdest |-> j, mterm |-> t,
     mgranted |-> granted]
AEMsg(i, j, t, prevIdx, prevTerm, ents, commit) ==
    [mtype |-> MsgAE, msource |-> i, mdest |-> j, mterm |-> t,
     mprevLogIndex |-> prevIdx, mprevLogTerm |-> prevTerm,
     mentries |-> ents, mcommitIndex |-> commit]
AEAckMsg(i, j, t, ok, lastIdx) ==
    [mtype |-> MsgAEAck, msource |-> i, mdest |-> j, mterm |-> t,
     msuccess |-> ok, mlastIndex |-> lastIdx]
TimeoutNowMsg(i, j, t) ==
    [mtype |-> MsgTimeoutNow, msource |-> i, mdest |-> j, mterm |-> t]
\* Staged kinds.
MeetMsg(i, j, single) ==
    [mtype |-> MsgMeet, msource |-> i, mdest |-> j, msingleton |-> single]
\* ty \in {MsgAddMe, MsgWelcome, MsgMeetRejected}
BareMsg(ty, i, j) == [mtype |-> ty, msource |-> i, mdest |-> j]
ProposeMsg(i, j, p) == [mtype |-> MsgPropose, msource |-> i, mdest |-> j,
                        mentry |-> p]
RejectMsg(i, j, reason, p) ==
    [mtype |-> MsgReject, msource |-> i, mdest |-> j, mreason |-> reason,
     mentry |-> p]
SnapshotMsg(i, j, t, lastIdx, lastTerm, mems, vots, prefix) ==
    [mtype |-> MsgInstallSnapshot, msource |-> i, mdest |-> j, mterm |-> t,
     mlastIndex |-> lastIdx, mlastTerm |-> lastTerm, mmembers |-> mems,
     mvoters |-> vots, mprefix |-> prefix]

\* type: bag of Messages, i.e. a function from a finite subset of Messages
\*       to Nat with every count >= 1 (S4). Init: the empty function.
\* Written through Step (AddAll, WithoutMessage) or Discard; never directly.
VARIABLE messages

--------------------------------------------------------------------------------
\* Per-server state that survives Restart (persisted in nodes.conf).

\* rs->current_term.
\* type: [Server -> Nat]. Init: 1 everywhere. Never decreases (step-down on a
\* higher term, StartElection, JoinerTimeout raise it).
VARIABLE currentTerm
\* rs->voted_for. All zeros in the code = Nil here.
\* type: [Server -> Server \cup {Nil}] (S3). Init: InitLeader for voters
\* when InitLeader /= Nil, else Nil. Cleared to Nil on a term change.
VARIABLE votedFor
\* rs->log plus the trimmed prefix, indexed by Raft index (see LOG INDEXING
\* CONVENTION). Trimming not modelled.
\* type: [Server -> Seq(Entries)]. Init: << >> everywhere. Changed only by
\* appending at the end or truncating a suffix (and, staged, by
\* SingletonStepDown and HandleInstallSnapshot replacing it whole).
VARIABLE log
\* rs->commit_index, which equals rs->last_applied after every handler:
\* raftApplyCommitted runs in the same handler that advances the commit index,
\* so one variable covers both. On Restart it keeps the value of the last full
\* rewrite; voter changes force one (clusterRaftApplyAddVoter,
\* clusterRaftApplyDelVoter).
\* type: [Server -> Nat], with commitIndex[i] <= Len(log[i])
\* (CommitWithinLog). Init: 0. Can DECREASE on Restart: the vars line
\* (lastApplied) is written only on a full rewrite, so a restart resumes
\* from the last rewrite and re-applies what followed (design
\* "Persistence"). Also reset to 0 by SingletonStepDown.
VARIABLE commitIndex
\* Applied voter set in this server's view: nodes without CLUSTER_NODE_LEARNER
\* (nodeIsVoter); server.cluster->size = Cardinality(voters[i]).
\* Explicit, not derived from the log: with DelVoterReadsLeader the applied
\* config is not a function of the log alone.
\* type: [Server -> SUBSET Server] (a set per server, S1, S8). Init:
\* InitVoters for every server. Changed only by applying entries
\* (ApplyTo, one ApplyEntry per entry); read through Eff, never directly,
\* by quorum and vote-filter code.
VARIABLE voters

serverVars == <<currentTerm, votedFor, log, commitIndex, voters>>

--------------------------------------------------------------------------------
\* Per-server state reset by Restart (except `leader`, see below).

\* rs->role.
\* type: [Server -> Roles]. Init: Leader for InitLeader, Follower for other
\* InitVoters, Learner for the rest. Invariant to keep in mind: role[i] =
\* Learner <=> i \notin voters[i] for roles other than Joiner: the code
\* asserts it every cron tick (clusterRaftAssertLearnerRole), so a model
\* state that breaks it is a crash or a modelling error. See
\* LearnerRoleMatches.
VARIABLE role
\* rs->leader. Nil = unknown. Read by CanGrantVote's lease check and by
\* DEL_VOTER apply. Also persisted as `raftLeader` on each full rewrite
\* (clusterRaftAppendVarsLine, design "Persistence"), so after Restart it
\* holds the value from the last rewrite, not Nil. Restart sets it to Nil
\* or leader[i] (only its Nil-ness is read before it is overwritten; see
\* Restart).
\* type: [Server -> Server \cup {Nil}] (S3). Init: InitLeader everywhere.
VARIABLE leader
\* rs->pre_votes_received / rs->votes_received. Counters, as in the code
\* (B-07), not sets as in raft.tla (S6). Self vote included.
\* type: [Server -> Nat] each. Init: 0. Reset to 1 when a round starts;
\* meaningful only in PreCandidate / Candidate respectively.
VARIABLE preVotes, votes
\* Leader only: RAFT_NODE(n)->next_index / match_index.
\* type: [Server -> [Server -> Nat]] each (S2): nextIndex[i][j] is leader
\* i's next index to send to j. Init: 1 and 0 for every pair. BecomeLeader
\* sets both for its members other than i (Len(log[i]) + 1 and 0), unless
\* it is the only voter (clusterRaftBecomeLeader returns early). Restart
\* sets both to 0 (zcalloc'd node data), so a leader that never ran the
\* reset has nextIndex 0. Bounds: IndexBounds.
VARIABLE nextIndex, matchIndex
\* Leader only: RAFT_NODE(n)->replicating (#5500, the cluster-v2 head
\* commit). TRUE after a successful AE_ACK from n; while TRUE, each AE sent
\* to n advances nextIndex past the entries it carries (entries in flight
\* are not resent); a NACK clears it.
\* type: [Server -> [Server -> BOOLEAN]] (S2). Init: FALSE for every pair.
\* BecomeLeader resets it for its members; Restart clears it.
VARIABLE replicating
\* Leader only, ConfigGuard = "Etcd" only: etcd's pendingConfIndex. Not in
\* Valkey today; the index below which a new voter change must wait.
\* type: [Server -> Nat]. Init: 0. Written by BecomeLeader and by
\* ProposeAddVoter / ProposeDelVoter; read only by ConfigAllowed.
VARIABLE pendingConfIndex

volatileVars == <<role, leader, preVotes, votes, nextIndex, matchIndex,
                  replicating, pendingConfIndex>>

(***************************************************************************)
(* Staged variables: declared below and kept at their Init values until     *)
(* their group's actions are in Next. P = persisted. App state (G5) is      *)
(* NOT declared yet: it needs a CONSTANT Shard and a decision on how much   *)
(* of slots/replicas to model.                                              *)
(*                                                                          *)
(* members         [Server -> SUBSET Server]                          P     *)
(*   server.cluster->nodes minus JOINING (clusterRaftHasPeerMembers).       *)
(*   i's applied topology; learners become members[i] \ voters[i] (not      *)
(*   Server \ voters[i]). Init "Formed": Server; "Fresh": {i}. G3.          *)
(* deferredMeets   [Server -> SUBSET Server]                                *)
(*   rs->deferred_meets, a list in code; a set suffices (a duplicate only   *)
(*   repeats an idempotent invite). Init: {}. G3.                           *)
(* pending         [Server -> SUBSET Proposals]                             *)
(*   rs->pending_proposals, in memory only: Restart empties it. Set, not    *)
(*   list: two identical proposals collapse, which hides per-client         *)
(*   completion but not protocol behaviour. Init: {}. Proposals.            *)
(* transferTarget  [Server -> Server \cup {Nil}]                            *)
(*   rs->transfer_target. Init: Nil. Transfer.                              *)
(* transferSent    [Server -> BOOLEAN]                                      *)
(*   rs->transfer_sent. Init: FALSE. rs->transfer_term is not needed if     *)
(*   every step-down clears transferTarget and transferSent (the code       *)
(*   instead compares transfer_term with current_term).                     *)
(* firstIndex      [Server -> Nat], 1 <= firstIndex[i] <= commitIndex[i] + 1 *)
(*   rs->log[0]->index (or last_applied + 1 when the window is empty).      *)
(*   Init: 1. Restart sets it to commitIndex[i] + 1: on load the code keeps *)
(*   only entries after last_applied (clusterRaftPostLoad).  P (derived)    *)
(* App state (G5), one copy per server (S1):                          P     *)
(*   failed      [Server -> SUBSET Server]          FAIL flags              *)
(*   primaryOf   [Server -> [Server -> Server \cup {Nil}]]                  *)
(*               primaryOf[i][x]: x's primary in i's view, Nil if x is      *)
(*               a primary                                                  *)
(*   shardEpoch  [Server -> [Shard -> Nat]]      needs CONSTANT Shard       *)
(*   Slots stay abstract (Data entries).                                    *)
(*                                                                          *)
(* Trimming without re-indexing: keep log[i] as the full sequence and add   *)
(* firstIndex[i]; entries below it are "trimmed" (unreadable by actions,    *)
(* still readable by invariants). INSTALL_SNAPSHOT then sets log[i] to the  *)
(* ghost mprefix. Every invariant in ValkeyRaft keeps working unchanged.    *)
(* The cost: actions must never read below firstIndex, which a mutant       *)
(* can check.                                                               *)
(*                                                                          *)
(* Second Init needed for G3: every server a fresh singleton (role Leader,  *)
(* term 1, empty log, CLUSTER_NODE_JOINING set and size 0, as in            *)
(* clusterRaftInitLast): members = {i}, voters = {} until its self-join     *)
(* applies. Select with a new constant, e.g. InitMode \in {"Formed",        *)
(* "Fresh"}. The current Init is "Formed".                                  *)
(***************************************************************************)

\* G3. server.cluster->nodes minus JOINING: i's applied topology.
\* type: [Server -> SUBSET Server]. Init: Server (the "Formed" Init). P.
VARIABLE members
\* G3. rs->deferred_meets (a list in code; a set suffices).
\* type: [Server -> SUBSET Server]. Init: {}. Emptied by Restart.
VARIABLE deferredMeets
\* Proposals. rs->pending_proposals, in memory only.
\* type: [Server -> SUBSET Proposals]. Init: {}. Emptied by Restart.
VARIABLE pending
\* Transfer. rs->transfer_target / rs->transfer_sent.
\* type: [Server -> Server \cup {Nil}] / [Server -> BOOLEAN]. Init: Nil /
\* FALSE. Cleared by every step-down and by Restart.
VARIABLE transferTarget, transferSent
\* Trimming. First index still readable by actions (rule 2 watermark).
\* type: [Server -> Nat], 1 <= firstIndex[i] <= commitIndex[i] + 1. Init: 1.
\* Restart sets it to commitIndex'[i] + 1. P (derived).
VARIABLE firstIndex

joinVars     == <<members, deferredMeets>>
proposalVars == <<pending>>
transferVars == <<transferTarget, transferSent>>
trimVars     == <<firstIndex>>
stagedVars   == <<joinVars, proposalVars, transferVars, trimVars>>

--------------------------------------------------------------------------------
\* History (not in the implementation; used by invariants).

\* Every entry a leader has marked committed (ghost, S7).
\* type: SUBSET [index : Nat, entry : Entries]. Init: {}. Only grows; added
\* to when a leader advances its commitIndex, one record per newly
\* committed index.
VARIABLE committed

vars == <<messages, serverVars, volatileVars, committed, stagedVars>>

--------------------------------------------------------------------------------
\* Helpers

LastIndex(xlog) == Len(xlog)
LastTerm(xlog)  == IF Len(xlog) = 0 THEN 0 ELSE xlog[Len(xlog)].term

\* Term of the entry at index k (indexing rule 4): 0 for k = 0 or past the end.
TermAt(xlog, k) == IF k = 0 \/ k > Len(xlog) THEN 0 ELSE xlog[k].term

\* Effect of applying entry e on voter set vs. ldr = the applier's rs->leader
\* at apply time (in the AE handler: the value just set from the sender).
\* Apply-time checks; a rejected entry is a no-op (and forces no rewrite):
\*   AddVoter(x): vs \cup {x}; no-op if x is already a voter
\*     (clusterRaftApplyAddVoter rejects a node that is not a learner)
\*   DelVoter(x): vs \ {x} if x \in vs and Cardinality(vs) > 1; skipped if
\*     DelVoterReadsLeader and x = ldr (clusterRaftApplyDelVoter)
\* Role changes of the applier itself are in ApplyEntry.
ApplyVoters(vs, ldr, e) ==
    CASE e.type = EntAddVoter -> vs \cup {e.arg}
      [] e.type = EntDelVoter /\ e.arg \in vs /\ Cardinality(vs) > 1
                             /\ ~(DelVoterReadsLeader /\ e.arg = ldr)
                             -> vs \ {e.arg}
      [] OTHER -> vs

\* Fold ApplyVoters over xlog[from..to] in order, with a fixed ldr.
RECURSIVE FoldVoters(_, _, _, _, _)
FoldVoters(vs, ldr, xlog, from, to) ==
    IF from > to THEN vs
    ELSE FoldVoters(ApplyVoters(vs, ldr, xlog[from]), ldr, xlog, from + 1, to)

WithMessage(m, msgs) ==
    IF m \in DOMAIN msgs THEN [msgs EXCEPT ![m] = @ + 1]
    ELSE msgs @@ (m :> 1)
WithoutMessage(m, msgs) ==
    IF m \in DOMAIN msgs THEN
        IF msgs[m] <= 1 THEN [x \in DOMAIN msgs \ {m} |-> msgs[x]]
        ELSE [msgs EXCEPT ![m] = @ - 1]
    ELSE msgs
\* Add a set of messages (one broadcast) to bag msgs.
RECURSIVE AddAll(_, _)
AddAll(S, msgs) ==
    IF S = {} THEN msgs
    ELSE LET m == CHOOSE x \in S : TRUE
         IN AddAll(S \ {m}, WithMessage(m, msgs))
\* The only writers of `messages`: Step (node actions) and Discard (loss).
Discard(m) == messages' = WithoutMessage(m, messages)

\* Messages in flight, counting duplicates (StateConstraint, MaxMessages).
RECURSIVE BagSum(_, _)
BagSum(msgs, D) ==
    IF D = {} THEN 0
    ELSE LET m == CHOOSE x \in D : TRUE IN msgs[m] + BagSum(msgs, D \ {m})
InFlight == BagSum(messages, DOMAIN messages)

Min(s) == CHOOSE x \in s : \A y \in s : x <= y
Max(s) == CHOOSE x \in s : \A y \in s : x >= y

--------------------------------------------------------------------------------
\* Node state as one record: the inside of the black box. An action reads
\* Local(i), composes the handler's steps as record -> record transforms in
\* code order, and hands the result to Step, which runs the beforeSleep
\* flush and writes it back with SetLocal. SetLocal writes every per-server
\* variable of i and nothing of any other server, so per-server frames are
\* right by construction; the action only states messages (via Step) and
\* the staged variables.
\* Fields: term, votedFor, log, commit, voters (serverVars); role, leader,
\* preVotes, votes, next, match, repl, pci (volatileVars). next, match and
\* repl are i's rows: [Server -> Nat] / [Server -> BOOLEAN].
Local(i) ==
    [term |-> currentTerm[i], votedFor |-> votedFor[i], log |-> log[i],
     commit |-> commitIndex[i], voters |-> voters[i], role |-> role[i],
     leader |-> leader[i], preVotes |-> preVotes[i], votes |-> votes[i],
     next |-> nextIndex[i], match |-> matchIndex[i],
     repl |-> replicating[i], pci |-> pendingConfIndex[i]]

SetLocal(i, s) ==
    /\ currentTerm'      = [currentTerm      EXCEPT ![i] = s.term]
    /\ votedFor'         = [votedFor         EXCEPT ![i] = s.votedFor]
    /\ log'              = [log              EXCEPT ![i] = s.log]
    /\ commitIndex'      = [commitIndex      EXCEPT ![i] = s.commit]
    /\ voters'           = [voters           EXCEPT ![i] = s.voters]
    /\ role'             = [role             EXCEPT ![i] = s.role]
    /\ leader'           = [leader           EXCEPT ![i] = s.leader]
    /\ preVotes'         = [preVotes         EXCEPT ![i] = s.preVotes]
    /\ votes'            = [votes            EXCEPT ![i] = s.votes]
    /\ nextIndex'        = [nextIndex        EXCEPT ![i] = s.next]
    /\ matchIndex'       = [matchIndex       EXCEPT ![i] = s.match]
    /\ replicating'      = [replicating      EXCEPT ![i] = s.repl]
    /\ pendingConfIndex' = [pendingConfIndex EXCEPT ![i] = s.pci]

\* The voter set a node in state s counts quorums against: nodeIsVoter for
\* every node, itself included. The code reads node flags, i.e. the applied
\* set; under ConfigOnAppend the flags would follow the latest config, so
\* every voter check reads Eff (quorum, vote and pre-vote filters,
\* pre-validation, "am I a voter").
\*   ConfigOnAppend = FALSE (code): the applied set.
\*   ConfigOnAppend = TRUE (4.1):   applied set plus every config entry in the
\*                                  unapplied tail, applied without the
\*                                  rs->leader check (Nil never matches).
Eff(s) ==
    IF ConfigOnAppend
    THEN FoldVoters(s.voters, Nil, s.log, s.commit + 1, Len(s.log))
    ELSE s.voters

\* server.cluster->size outside apply. Apply itself reads the applied set
\* being folded (ApplyVoters).
Size(s) == Cardinality(Eff(s))

\* clusterRaftQuorum: size/2 + 1.
Quorum(s) == Size(s) \div 2 + 1

\* Per-server views, for invariants.
EffVoters(i)  == Eff(Local(i))
QuorumSize(i) == Quorum(Local(i))

\* A voter change in s's log beyond its commit index.
ConfigInFlight(s) ==
    \E k \in (s.commit + 1)..Len(s.log) : s.log[k].type \in ConfigTypes

\* May a leader in state s propose a voter change now? (ConfigGuard modes.)
\* Valkey applies everything it commits in the same handler, so etcd's
\* "applied" is the commit index here.
ConfigAllowed(s) ==
    CASE ConfigGuard = GuardNone      -> TRUE
      [] ConfigGuard = GuardNoPending -> ~ConfigInFlight(s)
      [] ConfigGuard = GuardEtcd      -> s.pci <= s.commit

\* Election restriction, the last part of clusterRaftCanGrantVote.
LogUpToDate(s, mLastTerm, mLastIndex) ==
    IF mLastTerm /= LastTerm(s.log) THEN mLastTerm > LastTerm(s.log)
    ELSE mLastIndex >= LastIndex(s.log)

\* clusterRaftCanGrantVote without the lease: not a Joiner, i itself not a
\* learner (nodeIsLearner(myself)), candidate's log up to date. The lease
\* ("heard from rs->leader within node_timeout") needs time; see
\* HandleRequestVote for where it is kept as a nondeterministic choice.
CanGrantVote(i, s, mLastIndex, mLastTerm) ==
    /\ s.role /= Joiner
    /\ i \in Eff(s)
    /\ LogUpToDate(s, mLastTerm, mLastIndex)

\* What a successful AE_ACK reports (see message fields above).
AckIndex(newLog, m) ==
    IF AckLastIndex THEN Len(newLog) ELSE m.mprevLogIndex + Len(m.mentries)

\* Follower commit after accepting AE m; s.log is the merged log.
\* Code (clusterRaftProcessAppendEntries): if leaderCommit > commit then
\* commit = min(leaderCommit, raftLogLastIndex()). Paper: cap at the last
\* new entry instead (also capped at Len, which only matters if the merge
\* stopped early). Never lowers the commit index.
FollowerCommit(s, m) ==
    LET cap == IF FollowerCommitLast THEN Len(s.log)
               ELSE Min({m.mprevLogIndex + Len(m.mentries), Len(s.log)})
        c   == Min({m.mcommitIndex, cap})
    IN IF c > s.commit THEN c ELSE s.commit

--------------------------------------------------------------------------------
\* Initial state: an established cluster at term 1 with empty logs, either
\* leaderless or with InitLeader already elected (all voters voted for it).

Init ==
    /\ messages    = [m \in {} |-> 0]
    /\ currentTerm = [i \in Server |-> 1]
    /\ votedFor    = [i \in Server |->
                        IF InitLeader /= Nil /\ i \in InitVoters THEN InitLeader ELSE Nil]
    /\ log         = [i \in Server |-> << >>]
    /\ commitIndex = [i \in Server |-> 0]
    /\ voters      = [i \in Server |-> InitVoters]
    /\ role        = [i \in Server |->
                        CASE i = InitLeader     -> Leader
                          [] i \in InitVoters   -> Follower
                          [] OTHER              -> Learner]
    /\ leader      = [i \in Server |-> InitLeader]
    /\ preVotes    = [i \in Server |-> 0]
    /\ votes       = [i \in Server |-> 0]
    /\ nextIndex   = [i \in Server |-> [j \in Server |-> 1]]
    /\ matchIndex  = [i \in Server |-> [j \in Server |-> 0]]
    /\ replicating = [i \in Server |-> [j \in Server |-> FALSE]]
    /\ pendingConfIndex = [i \in Server |-> 0]
    /\ committed   = {}
    \* staged
    /\ members        = [i \in Server |-> Server]
    /\ deferredMeets  = [i \in Server |-> {}]
    /\ pending        = [i \in Server |-> {}]
    /\ transferTarget = [i \in Server |-> Nil]
    /\ transferSent   = [i \in Server |-> FALSE]
    /\ firstIndex     = [i \in Server |-> 1]

TypeOK ==
    /\ currentTerm \in [Server -> Nat]
    /\ votedFor    \in [Server -> Server \cup {Nil}]
    /\ log         \in [Server -> Seq(Entries)]
    /\ commitIndex \in [Server -> Nat]
    /\ voters      \in [Server -> SUBSET Server]
    /\ role        \in [Server -> Roles]
    /\ leader      \in [Server -> Server \cup {Nil}]
    /\ preVotes    \in [Server -> Nat]
    /\ votes       \in [Server -> Nat]
    /\ nextIndex   \in [Server -> [Server -> Nat]]
    /\ matchIndex  \in [Server -> [Server -> Nat]]
    /\ replicating \in [Server -> [Server -> BOOLEAN]]
    /\ pendingConfIndex \in [Server -> Nat]
    /\ DOMAIN messages \subseteq Messages
    /\ \A m \in DOMAIN messages : messages[m] \in Nat \ {0}
    /\ committed   \subseteq [index : Nat, entry : Entries]
    \* staged
    /\ members        \in [Server -> SUBSET Server]
    /\ deferredMeets  \in [Server -> SUBSET Server]
    /\ pending        \in [Server -> SUBSET Proposals]
    /\ transferTarget \in [Server -> Server \cup {Nil}]
    /\ transferSent   \in [Server -> BOOLEAN]
    /\ firstIndex     \in [Server -> Nat]

(***************************************************************************)
(* APPLY FEEDBACK. Applying a committed entry can change the state the      *)
(* protocol itself runs on, not just the application state. Standard Raft   *)
(* has this for configuration entries only; here several types do it.       *)
(*                                                                          *)
(*  entry               protocol state it changes on apply                  *)
(*  ADD_VOTER/DEL_VOTER voters (quorum, who may vote, who counts); role of  *)
(*                      the target (Learner <-> Follower)                   *)
(*  NODE_JOIN learner   members (AE targets, vote filters); Joiner ->       *)
(*                      Learner when it is the applier's own join           *)
(*  NODE_JOIN voter     members and voters (self-join, size 0 -> 1)         *)
(*  NODE_FORGET         members, voters if a voter; on the target itself,   *)
(*                      role Learner and an emptied topology                *)
(*                      (clusterRaftForgetMyself)                           *)
(*  NODE_INFO           addresses, so links (liveness only)                 *)
(*  FAILOVER, NODE_FAIL proposals by replicas (clusterRaftCron schedules    *)
(*  SET_REPLICA_OF      failovers from applied FAIL flags): feedback        *)
(*  SLOT_CHANGE         through new proposals, not through quorum           *)
(*                                                                          *)
(* Rules this spec follows:                                                 *)
(*  A1. Apply is a function of (applied state, entry) only. Anything else   *)
(*      it reads is a deviation and gets a flag: rs->leader                 *)
(*      (DelVoterReadsLeader), time, link state, `myself` beyond the        *)
(*      branches below.                                                     *)
(*  A2. `myself`-dependent branches (NODE_FORGET of self, own NODE_JOIN)    *)
(*      are intended divergence. Once G3 is enabled, ConfigAgreement        *)
(*      must compare only servers in each other's topology.                 *)
(*  A3. One apply step per entry, in index order, inside the handler that   *)
(*      advances commitIndex (raftApplyCommitted). A config change at k     *)
(*      takes effect for entry k+1, never within entry k.                   *)
(*  A4. Commit decisions use the voter set as it was before the batch is    *)
(*      applied (see HandleAppendEntriesResponse). Whether that is safe is  *)
(*      exactly the on-apply question (C1, C8).                             *)
(*  A5. Apply-time rejections (stale shard epoch etc.) are no-ops on        *)
(*      protocol state. Until app state exists they never fire.             *)
(*  A6. Follower-side apply also happens in HandleInstallSnapshot (whole    *)
(*      topology replaced at once) and after Restart (clusterRaftPostLoad   *)
(*      recomputes size from the persisted node lines).                     *)
(***************************************************************************)

--------------------------------------------------------------------------------
\* Shared node-state transforms (record -> record). Used by more than one
\* group, so they live here (MODULE LAYOUT). Each is pure: the action that
\* uses it writes the result back through Step.

\* clusterRaftStepDown: role Follower unless Learner or Joiner, votes = 0,
\* votedFor = Nil, leader = Nil (and a full rewrite, see Restart).
\* preVotes is not reset. The code also defers pending proposals and
\* leaves the transfer to expire via transfer_term (staged, untouched here).
\* Run without a term change by LoseFreshness, which therefore clears
\* votedFor within a term (B-06).
StepDownS(s) ==
    [s EXCEPT !.role     = IF @ \in {Learner, Joiner} THEN @ ELSE Follower,
              !.votes    = 0,
              !.votedFor = Nil,
              !.leader   = Nil]

\* clusterRaftMaybeStepDown(t): on a higher term, adopt it and step down.
MaybeStepDownS(s, t) ==
    IF t > s.term THEN StepDownS([s EXCEPT !.term = t]) ELSE s

\* raftLogApply for one entry e: voter set (ApplyVoters, reading s.leader)
\* and the applier's own role when the entry changed the set and targets
\* it (clusterRaftApplyAddVoter: Follower; clusterRaftApplyDelVoter:
\* Learner, even if i is the leader, which only DelVoterReadsLeader stops).
ApplyEntry(i, s, e) ==
    LET vs == ApplyVoters(s.voters, s.leader, e)
    IN [s EXCEPT !.voters = vs,
                 !.role   = IF vs = s.voters \/ e.arg /= i THEN @
                            ELSE IF e.type = EntAddVoter THEN Follower
                            ELSE Learner]

\* rs->commit_index = c, then raftApplyCommitted: apply s.log[s.commit+1..c]
\* one entry at a time, in order (APPLY FEEDBACK A3). commit and last_applied
\* are one field (see commitIndex). Callers pass c <= Len(s.log), where
\* s.log is the log after the handler's append/truncate.
RECURSIVE ApplyTo(_, _, _)
ApplyTo(i, s, c) ==
    IF s.commit >= c THEN s
    ELSE ApplyTo(i, [ApplyEntry(i, s, s.log[s.commit + 1]) EXCEPT
                         !.commit = @ + 1], c)

\* The part of clusterRaftBeforeSleep that changes protocol state: a leader
\* with size <= 1 commits its whole log (quorum 1, after persistence) and
\* applies it. The rest of the flush is sends, which each action builds
\* itself (they read the same state: this commit changes no field a flushed
\* message carries).
FlushS(i, s) ==
    IF s.role = Leader /\ Size(s) <= 1 /\ Len(s.log) > s.commit
    THEN ApplyTo(i, s, Len(s.log))
    ELSE s

\* End of every node-local v0 action: flush, write i's state back, update
\* the network and the `committed` history. msgs is the bag after removing
\* the handled message (or `messages` for a timer or admin action); out is
\* the set of messages i sends.
\* History: an entry enters `committed` when a leader commits it, either in
\* the AE_ACK scan (ldrCommit = TRUE: everything from i's old commit index)
\* or in the singleton flush (from the pre-flush commit index). Follower
\* commits (HandleAppendEntries) and Restart are not leader decisions.
\* Staged variables stay at their Init values in v0.
Step(i, s, ldrCommit, out, msgs) ==
    LET f  == FlushS(i, s)
        lo == IF ldrCommit THEN commitIndex[i] + 1 ELSE s.commit + 1
    IN /\ SetLocal(i, f)
       /\ committed' = committed \cup
                          {[index |-> k, entry |-> f.log[k]] :
                              k \in lo..f.commit}
       /\ messages' = AddAll(out, msgs)
       /\ UNCHANGED stagedVars

\* STAGED (proposals). clusterRaftPropose. Leader: pre-validate and append.
\* Otherwise add e to pending[i] and, if leader[i] /= Nil, send PROPOSE.
\* Joiners cannot propose (design "Roles"): defer. Callers: RaftReplication,
\* RaftJoin (Invite, AdminForget), RaftFailover.
\* writes (primed): log, pending, messages, pendingConfIndex; the caller
\*   supplies UNCHANGED for the rest.
Propose(i, e) == FALSE

\* STAGED (transfer). clusterRaftRequestLeaderTransfer. Leader picks a target:
\* a voter, not i, not in handshake. Sets transferTarget[i], transferSent[i] =
\* FALSE. Callers: RaftJoin (AdminForget of the leader), RaftReplication
\* (HandlePropose of NodeForget(leader)).
\* writes (primed): transferVars; the caller supplies UNCHANGED for the rest.
StartTransfer(i) == FALSE

================================================================================
