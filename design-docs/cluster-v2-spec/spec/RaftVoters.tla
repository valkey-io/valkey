----------------------------- MODULE RaftVoters --------------------------------
(***************************************************************************)
(* G4 Voter set (primer G4). Admin ADDVOTER / DELVOTER on the leader.       *)
(* Their apply is ApplyVoters / ApplyEntry in ValkeyRaftBase.               *)
(***************************************************************************)
EXTENDS ValkeyRaftBase

\* Admin CLUSTER ADDVOTER x, on the leader (clusterRaftPropose).
\* code: role Leader, no TIMEOUT_NOW pending. Pre-validation
\*   (clusterRaftApplyAddVoter, validate_only): x known, not JOINING, a
\*   learner in i's view; unless x = i, x has a link and match[x] >=
\*   raftLogLastIndex(). Append [term, ADD_VOTER, x]. A refused proposal
\*   is simply not enabled.
\* model: ConfigAllowed (ConfigGuard; "None" is the code). pci = the new
\*   index, written only under "Etcd".
\* abstr: the admin command on a non-leader (PROPOSE forwarding, staged).
ProposeAddVoter(i, x) ==
    LET s == Local(i)
    IN /\ s.role = Leader
       /\ ~transferSent[i]
       /\ x \in members[i]
       /\ x \notin Eff(s)
       /\ x = i \/ s.match[x] >= LastIndex(s.log)
       /\ ConfigAllowed(s)
       /\ Step(i, [s EXCEPT
                     !.log = Append(@, MkEntry(s.term, EntAddVoter, x)),
                     !.pci = IF ConfigGuard = GuardEtcd
                             THEN LastIndex(s.log) + 1 ELSE @],
               FALSE, {}, messages)

\* Admin CLUSTER DELVOTER x, on the leader (clusterRaftPropose).
\* code: role Leader, no TIMEOUT_NOW pending. Pre-validation
\*   (clusterRaftApplyDelVoter, validate_only): x known, not JOINING, a
\*   voter in i's view, size > 1, x /= rs->leader (= i on the leader; this
\*   check stays whatever DelVoterReadsLeader says, which only concerns
\*   apply). Append [term, DEL_VOTER, x].
\* model and abstr: as ProposeAddVoter.
ProposeDelVoter(i, x) ==
    LET s == Local(i)
    IN /\ s.role = Leader
       /\ ~transferSent[i]
       /\ x \in members[i]
       /\ x \in Eff(s)
       /\ Size(s) > 1
       /\ x /= s.leader
       /\ ConfigAllowed(s)
       /\ Step(i, [s EXCEPT
                     !.log = Append(@, MkEntry(s.term, EntDelVoter, x)),
                     !.pci = IF ConfigGuard = GuardEtcd
                             THEN LastIndex(s.log) + 1 ELSE @],
               FALSE, {}, messages)

--------------------------------------------------------------------------------
NextG4 ==
    \/ \E i, x \in Server : ProposeAddVoter(i, x)
    \/ \E i, x \in Server : ProposeDelVoter(i, x)

================================================================================
