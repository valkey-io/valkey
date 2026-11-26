-------------------------- MODULE RaftReplication ------------------------------
(***************************************************************************)
(* G2 Log replication (primer G2). Client appends, AE and AE_ACK; staged:   *)
(* PROPOSE forwarding, trimming and INSTALL_SNAPSHOT.                       *)
(***************************************************************************)
EXTENDS ValkeyRaftBase

\* Per action: "code:" names what it copies from cluster_raft.c, "abstr:"
\* what it leaves out. Each action is one handler plus its beforeSleep
\* flush (Step).

\* ---- Messages and log helpers ----

\* clusterRaftSendAppendEntriesResponse: AE_ACK <term> <success>
\* <raftLogLastIndex()>, built with AEAckMsg. mlastIndex is Len(log) for a
\* NACK (both flag values) and AckIndex(...) for a success.

\* The append loop of clusterRaftProcessAppendEntries. ents[1] belongs at
\* index k; la is last_applied (= commit index here). Per entry, in order:
\*   - an entry at k with a different term: truncate from k
\*     (raftLogTruncateFrom, full rewrite);
\*   - an entry at k (same term), or k <= la: skip;
\*   - k not contiguous with the log: stop (no holes);
\*   - else append.
\* Terms only are compared, as in the code (Log Matching makes that enough).
RECURSIVE Merge(_, _, _, _)
Merge(xlog, k, ents, la) ==
    IF ents = << >> THEN xlog
    ELSE LET e    == Head(ents)
             ylog == IF k <= Len(xlog) /\ xlog[k].term /= e.term
                     THEN SubSeq(xlog, 1, k - 1)
                     ELSE xlog
         IN IF k <= Len(ylog) \/ k <= la
            THEN Merge(ylog, k + 1, Tail(ents), la)
            ELSE IF k /= Len(ylog) + 1
                 THEN ylog
                 ELSE Merge(Append(ylog, e), k + 1, Tail(ents), la)

\* Votes for index k in the AE_ACK commit scan: other voters whose match
\* reaches k, plus i itself ("matches = 1", LeaderSelfCount; with the flag
\* FALSE, i counts only if it is in its own voter set).
Agree(i, s, k) ==
    (IF LeaderSelfCount \/ i \in Eff(s) THEN 1 ELSE 0)
    + Cardinality({p \in Eff(s) \ {i} : s.match[p] >= k})

\* The commit scan: the highest index above the commit index whose entry is
\* from the current term and has Agree >= quorum; else no change. Quorum
\* and voter set are those before the batch is applied (APPLY FEEDBACK A4).
CommitScan(i, s) ==
    LET C == {k \in (s.commit + 1)..Len(s.log) :
                 /\ s.log[k].term = s.term
                 /\ Agree(i, s, k) >= Quorum(s)}
    IN IF C = {} THEN s.commit ELSE Max(C)

\* ---- Actions ----

\* A leader-side proposal of a Data entry (stands in for any topology
\* proposal without a pre-validation in v0).
\* code: clusterRaftPropose with role Leader and no TIMEOUT_NOW pending
\*   (~transferSent: always TRUE until the transfer group runs): append
\*   [current_term, type, data]. Single-voter: commit happens in the flush
\*   (Step). Multi-node: broadcast left to AppendEntries.
\* abstr: the pending-proposal record (staged); clusterRaftSelfJoin on an
\*   empty log (the Formed Init stands for a cluster whose joins are
\*   already applied, so it is skipped).
ClientRequest(i, v) ==
    LET s == Local(i)
    IN /\ s.role = Leader
       /\ ~transferSent[i]
       /\ Step(i, [s EXCEPT !.log = Append(@, MkEntry(s.term, EntData, v))],
               FALSE, {}, messages)

\* clusterRaftSendAppendEntries(link, j), from a heartbeat, a broadcast or
\* the NACK resend.
\* code: every entry from next to the last; prev = next - 1 (0 if next is
\*   0); prevTerm = raftLogTermAt(prev), 0 past the end; commit =
\*   commit_index. #5500: if replicating[j], next += count (entries in
\*   flight are not resent). next < first available index sends
\*   INSTALL_SNAPSHOT instead (staged; firstIndex stays 1 in v0, so this
\*   only blocks next = 0, the after-Restart value: a singleton leader
\*   whose BecomeLeader skipped the reset cannot reach such a learner).
\* abstr: links (a missing link = a dropped message); the snapshot resend
\*   timer; JOINING targets (staged). Targets are all members but i.
AppendEntries(i, j) ==
    LET s    == Local(i)
        nxt  == s.next[j]
        prev == IF nxt > 0 THEN nxt - 1 ELSE 0
        ents == IF nxt > Len(s.log) THEN << >>
                ELSE SubSeq(s.log, nxt, Len(s.log))
        msg  == AEMsg(i, j, s.term, prev, TermAt(s.log, prev), ents, s.commit)
    IN /\ s.role = Leader
       /\ j \in members[i] \ {i}
       /\ nxt >= firstIndex[i]
       /\ Step(i, IF s.repl[j] THEN [s EXCEPT !.next[j] = nxt + Len(ents)]
                  ELSE s,
               FALSE, {msg}, messages)

\* clusterRaftProcessAppendEntries.
\* code:
\*   1. m.mterm < current_term: NACK at once, nothing else.
\*   2. MaybeStepDown(m.mterm). Role Follower unless Joiner or Learner (a
\*      same-term Leader or Candidate too). leader = j (clusterRaftSetLeader).
\*   3. prev > 0 and our term at prev is 0 or differs: NACK at once; the
\*      changes of step 2 stay.
\*   4. Merge; commit per FollowerCommit; apply (raftApplyCommitted, with
\*      rs->leader = j for DEL_VOTER); success AE_ACK from beforeSleep
\*      (todo_send_ae_ack) to rs->leader = j, carrying the flush-time
\*      term and AckIndex.
\* abstr: the forgotten-link case (no ACK; staged G3); pending-proposal
\*   retry on a leader change (staged).
HandleAppendEntries(i, j, m) ==
    LET s0   == Local(i)
        rest == WithoutMessage(m, messages)
    IN IF m.mterm < s0.term
       THEN Step(i, s0, FALSE,
                 {AEAckMsg(i, j, s0.term, FALSE, LastIndex(s0.log))}, rest)
       ELSE
         LET s1 == MaybeStepDownS(s0, m.mterm)
             s2 == [s1 EXCEPT !.role   = IF @ \in {Joiner, Learner} THEN @
                                         ELSE Follower,
                              !.leader = j]
             pt == TermAt(s2.log, m.mprevLogIndex)
         IN IF m.mprevLogIndex > 0 /\ (pt = 0 \/ pt /= m.mprevLogTerm)
            THEN Step(i, s2, FALSE,
                      {AEAckMsg(i, j, s2.term, FALSE, LastIndex(s2.log))}, rest)
            ELSE
              LET s3 == [s2 EXCEPT !.log = Merge(@, m.mprevLogIndex + 1,
                                                  m.mentries, s2.commit)]
                  s4 == ApplyTo(i, s3, FollowerCommit(s3, m))
              IN Step(i, s4, FALSE,
                      {AEAckMsg(i, j, s4.term, TRUE, AckIndex(s4.log, m))},
                      rest)

\* clusterRaftProcessAppendEntriesResponse.
\* code: MaybeStepDown(m.mterm). Ignore unless role Leader and m.mterm >=
\*   current_term (#5401).
\*   Success: match[j] = m.mlastIndex (not max'd, so a reordered ACK can
\*     lower it); next[j] = m.mlastIndex + 1 unless replicating[j] and
\*     next[j] is already past it (#5500); replicating[j] = TRUE. Then the
\*     commit scan (CommitScan) and apply (rs->leader = i). New commits go
\*     to `committed` (Step, ldrCommit).
\*   Failure: replicating[j] = FALSE; next[j] - 1 (if > 1); then down to
\*     m.mlastIndex + 1 if that is lower (while next > 1).
\* abstr: the NACK path's immediate resend is left to AppendEntries
\*   (replicating is FALSE then, so a later AE carries the same entries);
\*   the "commit advanced" broadcast likewise. last_ack_time, repl
\*   offsets, NODE_RECOVER (G5), transfer (staged), forgotten link (G3).
HandleAppendEntriesResponse(i, j, m) ==
    LET s0   == Local(i)
        s1   == MaybeStepDownS(s0, m.mterm)
        rest == WithoutMessage(m, messages)
    IN IF s1.role /= Leader \/ m.mterm < s1.term
       THEN Step(i, s1, FALSE, {}, rest)
       ELSE IF m.msuccess
       THEN LET s2 == [s1 EXCEPT
                         !.match[j] = m.mlastIndex,
                         !.next[j]  = IF ~s1.repl[j] \/ @ < m.mlastIndex + 1
                                      THEN m.mlastIndex + 1 ELSE @,
                         !.repl[j]  = TRUE]
            IN Step(i, ApplyTo(i, s2, CommitScan(i, s2)), TRUE, {}, rest)
       ELSE LET n1 == IF s1.next[j] > 1 THEN s1.next[j] - 1 ELSE s1.next[j]
                n2 == IF n1 > 1 /\ m.mlastIndex < n1 - 1
                      THEN m.mlastIndex + 1 ELSE n1
            IN Step(i, [s1 EXCEPT !.repl[j] = FALSE, !.next[j] = n2],
                    FALSE, {}, rest)

--------------------------------------------------------------------------------
NextG2 ==
    \/ \E i \in Server, v \in Value : ClientRequest(i, v)
    \/ \E i, j \in Server : AppendEntries(i, j)
    \/ \E m \in DOMAIN messages :
          LET i == m.mdest
              j == m.msource
          IN \/ m.mtype = MsgAE     /\ HandleAppendEntries(i, j, m)
             \/ m.mtype = MsgAEAck /\ HandleAppendEntriesResponse(i, j, m)

(***************************************************************************)
(* STAGED. Stubs not in Next yet. Their variables already exist in the      *)
(* base, so enabling is one line: add NextG2Staged to Next in ValkeyRaft    *)
(* (with its own MC*.cfg).                                                  *)
(***************************************************************************)

\* ---- Proposal forwarding (design "PROPOSE and Leader Validation") ----
\* Propose is shared (ValkeyRaftBase).

\* clusterRaftProcessPropose. Not Leader, or TIMEOUT_NOW already sent: Discard
\* (the proposer retries). Pre-validate: failure -> reply REJECT(reason,
\* entry); LEADER_TRANSFER -> StartTransfer, REJECT "transfer". Else append.
HandlePropose(i, j, m) ==
    /\ FALSE   \* TODO
    \* writes: log, messages, transferVars, pendingConfIndex
    /\ UNCHANGED <<currentTerm, votedFor, commitIndex, voters, role, leader,
                  preVotes, votes, nextIndex, matchIndex, replicating,
                  committed, joinVars, proposalVars, trimVars>>

\* clusterRaftProcessReject. "rejected": drop from pending[i]. "transfer":
\* keep pending. "conflict": rebuilt with a fresh epoch and resent (cap 5 in
\* design "Retry on stale epoch", 10 in code; primer G5). With epochs
\* abstracted, conflict = keep pending.
HandleReject(i, j, m) ==
    /\ FALSE   \* TODO
    \* writes: pending, messages
    /\ UNCHANGED <<serverVars, volatileVars, committed, joinVars,
                  transferVars, trimVars>>

\* clusterRaftRetryProposals. Triggers in code: HI on the leader link
\* (clusterRaftProcessHi), leader change (clusterRaftSetLeader), becoming
\* leader (clusterRaftBecomeLeader), transfer abort (clusterRaftCron).
\* Abstracted: enabled whenever pending[i] /= {}. Leader appends them; others
\* resend.
RetryProposals(i) ==
    /\ FALSE   \* TODO
    \* writes: messages, log, pendingConfIndex
    /\ UNCHANGED <<currentTerm, votedFor, commitIndex, voters, role, leader,
                  preVotes, votes, nextIndex, matchIndex, replicating,
                  committed, joinVars, proposalVars, transferVars, trimVars>>

\* clusterRaftCron (3x timeout, cap 10x). Drop some e \in pending[i].
\* (Quantify over pending[i] inside: Entries is infinite, TLC cannot
\* enumerate it.)
ExpireProposal(i) ==
    /\ FALSE   \* TODO
    \* writes: pending
    /\ UNCHANGED <<messages, serverVars, volatileVars, committed, joinVars,
                  transferVars, trimVars>>

\* Completion (clusterRaftCompletePendingProposal): when an applied entry
\* matches a pending one, drop it. Retries make duplicates normal, and design
\* "Proposal retry on leader change" says all types are idempotent at apply.
\* Candidate invariant: applying any committed entry twice leaves voters and
\* members unchanged (check AddVoter, DelVoter, NodeJoinVoter).

\* ---- Trimming and INSTALL_SNAPSHOT ----
\* design "Log compaction and snapshots"

\* raftLogTrimApplied. Raise firstIndex[i] to any value <= commitIndex[i] + 1
\* (code keeps 64 applied entries; any amount is an over-approximation).
TrimLog(i) ==
    /\ FALSE   \* TODO
    \* writes: firstIndex
    /\ UNCHANGED <<messages, serverVars, volatileVars, committed, joinVars,
                  proposalVars, transferVars>>

\* clusterRaftSendAppendEntries. Branch of AppendEntries: nextIndex[i][j] <
\* firstIndex[i] -> send INSTALL_SNAPSHOT(currentTerm, commitIndex, term at
\* commitIndex) instead.
SendInstallSnapshot(i, j) ==
    /\ FALSE   \* TODO
    \* writes: messages
    /\ UNCHANGED <<serverVars, volatileVars, committed, joinVars,
                  proposalVars, transferVars, trimVars>>

\* clusterRaftProcessInstallSnapshot. m.mterm < currentTerm: Discard.
\* Malformed (index or term 0, or term > mterm): Discard. Else step down on
\* higher term; role Follower unless Joiner or Learner; leader = j. Then:
\*   m.mlastIndex <= commitIndex[i]: ACK only.
\*   log has m.mlastIndex with term m.mlastTerm: keep the suffix, commit up to
\*     m.mlastIndex, apply, ACK (paper 7; design "InstallSnapshot" says
\*     "clearing", the code is more careful).
\*   sender not in mmembers: NACK.
\*   else: members, voters := mmembers, mvoters; log := mprefix;
\*     commitIndex = firstIndex - 1 = m.mlastIndex; Joiner -> Follower or
\*     Learner if i \in mmembers; ACK.
HandleInstallSnapshot(i, j, m) ==
    /\ FALSE   \* TODO
    \* writes: currentTerm, votedFor, role, leader, votes, transferVars, log,
    \*         commitIndex, voters, messages, members, firstIndex, pending
    /\ UNCHANGED <<preVotes, nextIndex, matchIndex, replicating,
                  pendingConfIndex, committed, deferredMeets>>

NextG2Staged ==
    \/ \E i \in Server : RetryProposals(i)
    \/ \E i \in Server : ExpireProposal(i)
    \/ \E i \in Server : TrimLog(i)
    \/ \E i, j \in Server : SendInstallSnapshot(i, j)
    \/ \E m \in DOMAIN messages :
          LET i == m.mdest
              j == m.msource
          IN \/ m.mtype = MsgPropose          /\ HandlePropose(i, j, m)
             \/ m.mtype = MsgReject           /\ HandleReject(i, j, m)
             \/ m.mtype = MsgInstallSnapshot /\ HandleInstallSnapshot(i, j, m)

================================================================================
