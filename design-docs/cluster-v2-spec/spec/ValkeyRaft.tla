----------------------------- MODULE ValkeyRaft --------------------------------
(***************************************************************************)
(* Valkey cluster-v2 Raft, protocol-level model: the module TLC runs.       *)
(* See ValkeyRaftBase for scope, conventions, flags and MODULE LAYOUT.      *)
(*                                                                          *)
(* Next is the v0 scope: G1, G2, G4 and faults. G3 (NextG3), G5 (NextG5)    *)
(* and the staged parts of G1/G2 (NextG1Staged, NextG2Staged) parse but     *)
(* are not in Next. All their variables are declared (at Init values)       *)
(* in ValkeyRaftBase, so enabling a group is adding its NextG line          *)
(* here. One group at a time, each with its own MC*.cfg.                    *)
(*                                                                          *)
(* GUARDS AGAINST MODELLING MISTAKES, cheapest first:                       *)
(*  - typed model values: servers are s_<id>; comparing one with a number   *)
(*    or string is a TLC error, not FALSE.                                  *)
(*  - ASSUME vacuity bounds (ValkeyRaftBase).                               *)
(*  - StagedUntouched, MessagesWellFormed: sanity invariants in the cfg.    *)
(*  - make canary: behaviours that must be reachable (CANARIES below).      *)
(*  - invariants are total (indexing rule 5): no evaluation errors.         *)
(***************************************************************************)
EXTENDS RaftElection, RaftReplication, RaftVoters, RaftFaults,
        RaftJoin, RaftFailover

--------------------------------------------------------------------------------
Next ==
    \/ NextG1
    \/ NextG2
    \/ NextG4
    \/ NextFaults

Spec == Init /\ [][Next]_vars

\* TLC symmetry (SYMMETRY in the cfg). Servers that start in the same
\* position are interchangeable: the voters other than InitLeader, and the
\* learners; client values too. Safety only: symmetry is unsound for
\* liveness. TLC does not check that these really are symmetries. CHOOSE is
\* the one operator that can break it: AddAll and BagSum use CHOOSE only
\* to pick an order whose result does not depend on it. Any new CHOOSE over
\* servers must do the same, or this line must go.
Symmetry ==
    Permutations(InitVoters \ {InitLeader})
        \cup Permutations(Server \ InitVoters)
        \cup Permutations(Value)

\* TLC bound (CONSTRAINT in the cfg).
StateConstraint ==
    /\ \A i \in Server : currentTerm[i] <= MaxTerm
    /\ \A i \in Server : Len(log[i]) <= MaxLogLen
    /\ InFlight <= MaxMessages

--------------------------------------------------------------------------------
\* Invariants (primer: G1, G2, G4)

\* G1. At most one leader per term (paper Fig. 3). Current leaders only;
\* raft.tla's `elections` history would also catch deposed leaders.
ElectionSafety ==
    \A i, j \in Server :
        (role[i] = Leader /\ role[j] = Leader /\ currentTerm[i] = currentTerm[j])
            => i = j

\* G2. Same index and term => identical prefixes.
LogMatching ==
    \A i, j \in Server : \A k \in 1..Min({Len(log[i]), Len(log[j])}) :
        log[i][k].term = log[j][k].term =>
            SubSeq(log[i], 1, k) = SubSeq(log[j], 1, k)

\* Indexing rule 5.
CommitWithinLog ==
    \A i \in Server : commitIndex[i] <= Len(log[i])

\* Indexing rule 6. Expected to FAIL with AckLastIndex = TRUE once a
\* follower holds stale entries past the leader's last index (B-10); a
\* direct, cheap witness for that deviation (calibration C10). Can also
\* fail through a singleton leader's BecomeLeader early return (stale
\* nextIndex for learners, see BecomeLeaderS). X[i][i] is not checked.
IndexBounds ==
    \A i \in Server : role[i] = Leader =>
        \A j \in Server \ {i} :
            /\ nextIndex[i][j] <= Len(log[i]) + 1
            /\ matchIndex[i][j] <= Len(log[i])

\* Code assertion (clusterRaftAssertLearnerRole): the role and the applied
\* voter set agree. Joiner is excluded (staged).
LearnerRoleMatches ==
    \A i \in Server : role[i] /= Joiner =>
        (role[i] = Learner <=> i \notin voters[i])

\* G2. No two servers apply different entries at the same index. Total
\* (indexing rule 5): also bounded by both logs.
StateMachineSafety ==
    \A i, j \in Server :
        \A k \in 1..Min({commitIndex[i], commitIndex[j],
                        Len(log[i]), Len(log[j])}) :
            log[i][k] = log[j][k]

\* G2. Committed entries are in the log of every leader of a later term.
LeaderCompleteness ==
    \A c \in committed : \A i \in Server :
        (role[i] = Leader /\ currentTerm[i] > c.entry.term) =>
            (Len(log[i]) >= c.index /\ log[i][c.index] = c.entry)

\* G2. No two different entries are ever committed at the same index, by any
\* leader (the Lean model's "two different entries committed" monitor).
\* Catches losses even when no current leader is holding the bad log.
CommittedUnique ==
    \A c1, c2 \in committed : c1.index = c2.index => c1.entry = c2.entry

\* G4, Valkey-specific. Servers that have applied the same prefix agree on
\* the voter set. Targets DelVoterReadsLeader (primer G4, Lean A4).
ConfigAgreement ==
    \A i, j \in Server :
        commitIndex[i] = commitIndex[j] => voters[i] = voters[j]

\* Model sanity (not protocol properties).
\* v0 frame check: staged variables never leave their Init values. A
\* failure means some v0 action writes a staged variable. Drop it from the
\* cfg when a staged group is enabled.
StagedUntouched ==
    /\ members        = [i \in Server |-> Server]
    /\ deferredMeets  = [i \in Server |-> {}]
    /\ pending        = [i \in Server |-> {}]
    /\ transferTarget = [i \in Server |-> Nil]
    /\ transferSent   = [i \in Server |-> FALSE]
    /\ firstIndex     = [i \in Server |-> 1]

\* No node sends to itself (every send loops over peers, not over Server).
MessagesWellFormed ==
    \A m \in DOMAIN messages : m.msource /= m.mdest

\* G4 quorum overlap across configurations: TODO. Needs the set of configs
\* that can currently form a quorum (EffVoters over leaders and candidates),
\* then pairwise majority intersection. Lean Quorum.lean proves configs one
\* change apart always intersect and two apart need not.

(***************************************************************************)
(* CANARIES. Each states that something ordinary NEVER happens, so each    *)
(* must FAIL. A canary that passes means the bounds or a guard made that   *)
(* behaviour unreachable, and a clean run of the real invariants would be  *)
(* vacuous. `make canary` runs each one alone and expects a violation.     *)
(***************************************************************************)
CanaryLeaderChange ==
    ~\E i \in Server : role[i] = Leader /\ currentTerm[i] > 1
CanaryCommit == committed = {}
CanaryFollowerApply ==
    \A i \in Server : role[i] /= Leader => commitIndex[i] = 0
CanaryVoterChange == \A i \in Server : voters[i] = InitVoters

(***************************************************************************)
(* Calibration (run before trusting a clean result). Flags not listed keep  *)
(* their code value. InitLeader = s_1 for all rows.                         *)
(*                                                                          *)
(*  run  flags                                       expect                 *)
(*  C1   code defaults, 2 voter changes              CommittedUnique or     *)
(*                                                   LeaderCompleteness     *)
(*                                                   fails (Lean F2)        *)
(*  C2   ConfigOnAppend, ConfigGuard = "None"        fails (Lean F2b)       *)
(*  C3   ConfigGuard = "NoPending", on-apply         Lean found none; our   *)
(*                                                   reordering may differ  *)
(*  C4   NoopOnElection = FALSE, no voter changes    fails (Lean F1)        *)
(*  C5   C4 + AckLastIndex = FollowerCommitLast = FALSE  no violation       *)
(*  C6   DEL_VOTER of a voter, code defaults         ConfigAgreement fails? *)
(*       (hypothesis: needs the leader-change interleaving in primer G4)    *)
(*  C7   ONE voter change only, ConfigGuard any      checks the design      *)
(*       doc's 3->4 example; Lean found no violation                        *)
(*  C8   #3926 proposal: ConfigOnAppend = FALSE,     the open question.     *)
(*       ConfigGuard = "Etcd", AckLastIndex =        Watch for a follower   *)
(*       FollowerCommitLast = FALSE,                 two configs behind     *)
(*       DelVoterReadsLeader = FALSE, MaxTerm >= 3,  winning an election    *)
(*       >= 2 voter changes across a leader change                          *)
(*  C9   C8 with ConfigGuard = "NoPending"           if C8 passes and C9    *)
(*                                                   fails, the cross-leader*)
(*                                                   guard is what matters  *)
(*  C10  code defaults, no voter changes,            IndexBounds fails      *)
(*       MaxTerm >= 3                                (B-10). With           *)
(*                                                   AckLastIndex = FALSE:  *)
(*                                                   no violation           *)
(***************************************************************************)

================================================================================
