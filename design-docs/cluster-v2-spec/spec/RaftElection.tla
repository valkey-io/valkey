---------------------------- MODULE RaftElection -------------------------------
(***************************************************************************)
(* G1 Leader election (primer G1). Pre-vote, vote, BecomeLeader,            *)
(* TIMEOUT_NOW, pre-vote and freshness timeouts; staged: leader transfer.   *)
(***************************************************************************)
EXTENDS ValkeyRaftBase

\* Per action: "code:" names what it copies from cluster_raft.c, "abstr:"
\* what it leaves out. Each action is one handler plus its beforeSleep
\* flush (Step). Vote and pre-vote filters, quorum and "am I a voter" all
\* read Eff (see its comment).

\* ---- Messages ----

\* clusterRaftSendPreVoteRequest, to every other voter with a link. The
\* candidate term is current_term + 1; no term change.
PreVoteReqs(i, s) ==
    {VoteReqMsg(MsgPreVoteReq, i, j, s.term + 1, LastIndex(s.log),
                LastTerm(s.log)) : j \in Eff(s) \ {i}}

\* clusterRaftSendRequestVote, from the beforeSleep broadcast
\* (todo_broadcast_vote_request), so built from the post-handler state s.
VoteReqs(i, s) ==
    {VoteReqMsg(MsgVoteReq, i, j, s.term, LastIndex(s.log),
                LastTerm(s.log)) : j \in Eff(s) \ {i}}

\* ---- Transforms (election steps called from several handlers) ----

\* clusterRaftBecomeLeader.
\* code: role Leader, leader = i. Return early when size <= 1: no
\*   next/match/replicating reset and NO no-op (with peers present only as
\*   learners, their nextIndex keeps whatever it held: 0 after Restart, or
\*   a stale value from an earlier term; see IndexBounds). Otherwise, for
\*   every peer: next = last + 1, match = 0, replicating = 0; append
\*   NOOP(term) (NoopOnElection). X[i][i] is not touched (the code skips
\*   myself).
\* model: pci = last index before the no-op, written only under "Etcd"
\*   (the only reader), so other modes don't split states on it.
\* abstr: the immediate AE broadcast is left to AppendEntries (replicating
\*   is FALSE for every peer here, so an AE sent later carries the same
\*   entries and moves nothing). Pending-proposal retry is staged.
BecomeLeaderS(i, s) ==
    LET s1 == [s EXCEPT !.role   = Leader,
                        !.leader = i,
                        !.pci    = IF ConfigGuard = GuardEtcd
                                   THEN LastIndex(s.log) ELSE @]
    IN IF Size(s1) <= 1 THEN s1
       ELSE [s1 EXCEPT
               !.next  = [j \in Server |->
                            IF j = i THEN s1.next[j] ELSE LastIndex(s.log) + 1],
               !.match = [j \in Server |-> IF j = i THEN s1.match[j] ELSE 0],
               !.repl  = [j \in Server |-> IF j = i THEN s1.repl[j] ELSE FALSE],
               !.log   = IF NoopOnElection
                         THEN Append(@, MkEntry(s.term, EntNoop, Nil))
                         ELSE @]

\* clusterRaftStartElection.
\* code: no-op unless i is a voter. term + 1, role Candidate, votedFor = i,
\*   votes = 1, full rewrite. If 1 >= quorum (size <= 1), BecomeLeader at
\*   once. rs->leader is NOT cleared. VOTE_REQ goes out from beforeSleep
\*   (ElectionOut).
StartElectionS(i, s) ==
    IF i \notin Eff(s) THEN s
    ELSE LET s1 == [s EXCEPT !.term     = @ + 1,
                             !.role     = Candidate,
                             !.votedFor = i,
                             !.votes    = 1]
         IN IF s1.votes >= Quorum(s1) THEN BecomeLeaderS(i, s1) ELSE s1

\* Messages StartElectionS sends (VoteReqs to the other voters; none when
\* i is the only voter, which is also the only case that cascades into
\* BecomeLeaderS, so its state is what the flush sees).
ElectionOut(i, s) ==
    IF i \notin Eff(s) THEN {} ELSE VoteReqs(i, StartElectionS(i, s))

\* clusterRaftStartPreVote.
\* code: no-op unless i is a voter. role PreCandidate, preVotes = 1,
\*   PRE_VOTE_REQ sent at once (not deferred). If 1 >= quorum,
\*   StartElection at once.
StartPreVoteS(i, s) ==
    IF i \notin Eff(s) THEN s
    ELSE LET s1 == [s EXCEPT !.role = PreCandidate, !.preVotes = 1]
         IN IF s1.preVotes >= Quorum(s1) THEN StartElectionS(i, s1) ELSE s1

PreVoteOut(i, s) ==
    IF i \notin Eff(s) THEN {}
    ELSE PreVoteReqs(i, s) \cup
         (IF 1 >= Quorum(s)
          THEN ElectionOut(i, [s EXCEPT !.role = PreCandidate, !.preVotes = 1])
          ELSE {})

\* ---- Actions ----

\* clusterRaftCron, election timeout.
\* code: dictSize(nodes) > 1, i a voter, role Follower or Candidate:
\*   StartPreVote.
\* abstr: "now - last_heartbeat > election_timeout" is always possible.
Timeout(i) ==
    LET s == Local(i)
    IN /\ Cardinality(members[i]) > 1
       /\ i \in Eff(s)
       /\ s.role \in {Follower, Candidate}
       /\ Step(i, StartPreVoteS(i, s), FALSE, PreVoteOut(i, s), messages)

\* clusterRaftCron, pre-vote timeout.
\* code: dictSize(nodes) > 1, role PreCandidate: role Follower. preVotes is
\*   left as is (it is reset by the next StartPreVote).
\* abstr: "now - pre_vote_started > election_timeout" is always possible.
\* Without this action a PreCandidate that never reaches quorum could not
\* time out again (Timeout needs Follower or Candidate).
AbandonPreVote(i) ==
    LET s == Local(i)
    IN /\ Cardinality(members[i]) > 1
       /\ s.role = PreCandidate
       /\ Step(i, [s EXCEPT !.role = Follower], FALSE, {}, messages)

\* clusterRaftProcessPreVoteRequest.
\* code: no state change at i (no step-down: the term is hypothetical).
\*   Deny if the candidate is a learner in i's view; else grant iff
\*   m.mterm > current_term and CanGrantVote. Reply term: m.mterm when
\*   granted, else i's current_term.
\* abstr: the lease. A lease denial changes no state at i and carries a
\*   term <= the candidate's (m.mterm > i's term was required to get that
\*   far), so the candidate ignores it: the same as dropping the request,
\*   which DropMessage already covers.
HandlePreVoteRequest(i, j, m) ==
    LET s     == Local(i)
        grant == /\ j \in Eff(s)
                 /\ m.mterm > s.term
                 /\ CanGrantVote(i, s, m.mlastLogIndex, m.mlastLogTerm)
        resp  == VoteRespMsg(MsgPreVote, i, j,
                             IF grant THEN m.mterm ELSE s.term, grant)
    IN Step(i, s, FALSE, {resp}, WithoutMessage(m, messages))

\* clusterRaftProcessPreVoteResponse.
\* code: a DENIED response with a higher term steps down (a granted one
\*   carries the hypothetical term and must not). Then count only if role
\*   PreCandidate, m.mterm = current_term + 1, granted, and the sender is a
\*   voter in i's view. On quorum: StartElection.
\* note: responses are not tied to a round. A grant from an abandoned
\*   round with the same candidate term still counts, and preVotes is a
\*   counter (S6), so one voter can be counted twice across rounds.
HandlePreVoteResponse(i, j, m) ==
    LET s0    == Local(i)
        s1    == IF ~m.mgranted THEN MaybeStepDownS(s0, m.mterm) ELSE s0
        count == /\ s1.role = PreCandidate
                 /\ m.mterm = s1.term + 1
                 /\ m.mgranted
                 /\ j \in Eff(s1)
        s2    == IF count THEN [s1 EXCEPT !.preVotes = @ + 1] ELSE s1
        win   == count /\ s2.preVotes >= Quorum(s2)
    IN Step(i, IF win THEN StartElectionS(i, s2) ELSE s2, FALSE,
            IF win THEN ElectionOut(i, s2) ELSE {},
            WithoutMessage(m, messages))

\* clusterRaftProcessRequestVote.
\* code: MaybeStepDown(m.mterm) FIRST (clears votedFor and leader on a
\*   higher term). Stale term: deny. Candidate a learner in i's view: deny.
\*   Grant iff votedFor \in {none, candidate} and CanGrantVote; then
\*   votedFor = candidate, full rewrite, and the granted VOTE goes out
\*   from beforeSleep. Denials go out at once. Reply term: current_term.
\* abstr: the lease is a nondeterministic denial, possible only while i
\*   knows a leader (rs->leader set). Unlike the pre-vote case it is not
\*   the same as a drop: the step-down above has already happened.
HandleRequestVote(i, j, m) ==
    LET s0 == Local(i)
        s1 == MaybeStepDownS(s0, m.mterm)
    IN \E lease \in IF s1.leader /= Nil THEN BOOLEAN ELSE {FALSE} :
          LET grant == /\ m.mterm >= s1.term
                       /\ j \in Eff(s1)
                       /\ s1.votedFor \in {Nil, j}
                       /\ ~lease
                       /\ CanGrantVote(i, s1, m.mlastLogIndex, m.mlastLogTerm)
              s2    == IF grant THEN [s1 EXCEPT !.votedFor = j] ELSE s1
              resp  == VoteRespMsg(MsgVote, i, j, s2.term, grant)
          IN Step(i, s2, FALSE, {resp}, WithoutMessage(m, messages))

\* clusterRaftProcessRequestVoteResponse.
\* code: MaybeStepDown(m.mterm). Count only if role Candidate, m.mterm =
\*   current_term, granted, and the sender is a voter in i's view. On
\*   quorum: BecomeLeader.
HandleRequestVoteResponse(i, j, m) ==
    LET s0    == Local(i)
        s1    == MaybeStepDownS(s0, m.mterm)
        count == /\ s1.role = Candidate
                 /\ m.mterm = s1.term
                 /\ m.mgranted
                 /\ j \in Eff(s1)
        s2    == IF count THEN [s1 EXCEPT !.votes = @ + 1] ELSE s1
    IN Step(i, IF count /\ s2.votes >= Quorum(s2) THEN BecomeLeaderS(i, s2)
               ELSE s2,
            FALSE, {}, WithoutMessage(m, messages))

\* clusterRaftProcessMessage, TIMEOUT_NOW branch.
\* code: i a voter, m.mterm >= current_term, role Follower:
\*   StartElection (skips pre-vote). No step-down to m.mterm, and no check
\*   of who sent it (B-15).
\* note: no v0 action sends TIMEOUT_NOW (MaybeSendTimeoutNow and Shutdown
\*   are staged), so this never fires in v0.
HandleTimeoutNow(i, j, m) ==
    LET s  == Local(i)
        go == /\ i \in Eff(s)
              /\ m.mterm >= s.term
              /\ s.role = Follower
    IN Step(i, IF go THEN StartElectionS(i, s) ELSE s, FALSE,
            IF go THEN ElectionOut(i, s) ELSE {},
            WithoutMessage(m, messages))

\* clusterRaftCron, freshness check.
\* code: dictSize(nodes) > 1, role Leader, size > 1 (with size <= 1
\*   clusterRaftLeaderHasFreshQuorum is always true): clusterRaftStepDown
\*   with no term change, which clears votedFor (B-06).
\* abstr: "no AE_ACK from a quorum within node_timeout" is always possible.
LoseFreshness(i) ==
    LET s == Local(i)
    IN /\ Cardinality(members[i]) > 1
       /\ s.role = Leader
       /\ Size(s) > 1
       /\ Step(i, StepDownS(s), FALSE, {}, messages)

--------------------------------------------------------------------------------
NextG1 ==
    \/ \E i \in Server : Timeout(i)
    \/ \E i \in Server : AbandonPreVote(i)
    \/ \E i \in Server : LoseFreshness(i)
    \/ \E m \in DOMAIN messages :
          LET i == m.mdest
              j == m.msource
          IN \/ m.mtype = MsgPreVoteReq /\ HandlePreVoteRequest(i, j, m)
             \/ m.mtype = MsgPreVote     /\ HandlePreVoteResponse(i, j, m)
             \/ m.mtype = MsgVoteReq     /\ HandleRequestVote(i, j, m)
             \/ m.mtype = MsgVote         /\ HandleRequestVoteResponse(i, j, m)
             \/ m.mtype = MsgTimeoutNow  /\ HandleTimeoutNow(i, j, m)

(***************************************************************************)
(* STAGED. Stubs not in Next yet. Their variables already exist in the      *)
(* base, so enabling is one line: add NextG1Staged to Next in ValkeyRaft    *)
(* (with its own MC*.cfg).                                                  *)
(***************************************************************************)

\* ---- Leader transfer (design "Leader Transfer") ----
\* StartTransfer is shared (ValkeyRaftBase).

\* clusterRaftMaybeSendTimeoutNow. When matchIndex[i][t] = Len(log[i]): send
\* TIMEOUT_NOW to t, transferSent[i] = TRUE. While sent, i appends nothing:
\* ClientRequest, Propose*, HandlePropose all disabled
\* (clusterRaftTransferSent, clusterRaftProcessPropose).
MaybeSendTimeoutNow(i) ==
    /\ FALSE   \* TODO
    \* writes: messages, transferSent
    /\ UNCHANGED <<serverVars, volatileVars, committed, joinVars,
                  proposalVars, transferTarget, trimVars>>

\* clusterRaftCron (node timeout). Clear the transfer; i stays leader. The code
\* then retries held proposals; here RetryProposals (G2) is enabled whenever
\* pending[i] /= {}, so no call is needed (and none would be allowed: G1 does
\* not import G2).
AbortTransfer(i) ==
    /\ FALSE   \* TODO
    \* writes: transferVars
    /\ UNCHANGED <<messages, serverVars, volatileVars, committed, joinVars,
                  proposalVars, trimVars>>

\* clusterRaftPrepareShutdown. Graceful shutdown of the leader: TIMEOUT_NOW to
\* the voter with the highest matchIndex (no catch-up wait, unlike
\* MaybeSendTimeoutNow), then Restart(i). Compare with the forget path: this
\* one can elect a target that lacks the leader's last entries (election
\* restriction then decides; not a safety issue, but check liveness).
Shutdown(i) ==
    /\ FALSE   \* TODO
    \* writes: messages, commitIndex, volatileVars, deferredMeets,
    \*         proposalVars, transferVars, trimVars
    /\ UNCHANGED <<currentTerm, votedFor, log, voters, committed, members>>

NextG1Staged ==
    \/ \E i \in Server : MaybeSendTimeoutNow(i)
    \/ \E i \in Server : AbortTransfer(i)
    \/ \E i \in Server : Shutdown(i)

================================================================================
