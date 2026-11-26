----------------------------- MODULE RaftFaults --------------------------------
(***************************************************************************)
(* Faults: crash/restart and message loss.                                  *)
(***************************************************************************)
EXTENDS ValkeyRaftBase

\* Crash and restart (nodes.conf load, clusterRaftInit, clusterRaftPostLoad).
\* What the file holds: node lines and the vars line (currentTerm,
\* lastApplied, votedFor, raftLeader) as of the last FULL rewrite
\* (clusterSaveConfigOrDie), plus log lines appended since.
\* code:
\*   - currentTerm, votedFor: exact. Every change forces a full rewrite in
\*     the same flush (StepDown, StartElection, a granted vote).
\*   - log: exact. Appends are written in the flush, before any ACK;
\*     truncation forces a full rewrite.
\*   - commitIndex = lastApplied of the last rewrite, so it can go back.
\*     Rewrites happen on term/vote changes, truncation, an EFFECTIVE voter
\*     change (clusterRaftApplyAdd/DelVoter set todo_save_config only after
\*     their rejection checks), on load itself, and every
\*     RAFT_LOG_REWRITE_THRESHOLD applied entries.
\*   - voters: from the node lines of that same rewrite.
\*   - role: Follower (clusterRaftInit), Learner if i's node line has the
\*     learner flag (clusterRaftPostLoad).
\*   - everything else in rs, and next/match/replicating, zeroed.
\* model: commitIndex' = any c in lo..commitIndex[i], voters unchanged, where
\*   lo is the highest config entry at or below commitIndex[i] that may
\*   have changed voters. A DEL_VOTER whose target is still in voters[i]
\*   (with no possibly-effective config entry after it) was certainly
\*   rejected, so the rollback may pass it, and the replay re-applies it,
\*   possibly with a different rs->leader (DelVoterReadsLeader).
\*   leader' \in {Nil, leader[i]}. The saved value is Nil, leader[i], or a
\*   leader of an earlier term (StartElection keeps rs->leader and
\*   rewrites; BecomeLeader then changes it without one). Before any apply
\*   the AE handler overwrites it, so the only reader is the vote lease,
\*   which tests Nil-ness only: {Nil, leader[i]} covers every behaviour.
\* abstr: c < the real rewrite point is an over-approximation (it only
\*   re-applies Data/Noop entries and rejected DEL_VOTERs). Rolling back
\*   past an AddVoter of a voter or a DelVoter of a non-voter (rejected at
\*   apply, so possible in the code) is not allowed: an under-approximation.
\*   In-flight messages survive (TCP would drop those on i's links; drops
\*   are DropMessage). The code's log window restarts at lastApplied + 1
\*   (firstIndex, staged): a leader with next below it would send
\*   INSTALL_SNAPSHOT, where v0 sends the entries.
Restart(i) ==
    LET s  == Local(i)
        lo == Max({0} \cup
                  {k \in 1..s.commit :
                      /\ s.log[k].type \in ConfigTypes
                      /\ ~(/\ s.log[k].type = EntDelVoter
                            /\ s.log[k].arg \in s.voters)})
    IN \E c \in lo..s.commit, ldr \in {Nil, s.leader} :
          Step(i, [s EXCEPT !.commit   = c,
                            !.role     = IF i \in s.voters THEN Follower
                                         ELSE Learner,
                            !.leader   = ldr,
                            !.preVotes = 0,
                            !.votes    = 0,
                            !.next     = [j \in Server |-> 0],
                            !.match    = [j \in Server |-> 0],
                            !.repl     = [j \in Server |-> FALSE],
                            !.pci      = 0],
               FALSE, {}, messages)

DropMessage(m) ==
    /\ Discard(m)
    /\ UNCHANGED <<serverVars, volatileVars, committed, stagedVars>>

--------------------------------------------------------------------------------
NextFaults ==
    \/ \E i \in Server : Restart(i)
    \/ \E m \in DOMAIN messages : DropMessage(m)

================================================================================
