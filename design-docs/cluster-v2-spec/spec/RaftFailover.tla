---------------------------- MODULE RaftFailover -------------------------------
(***************************************************************************)
(* G5 Failure detection and failover (primer G5). All STAGED.               *)
(***************************************************************************)
EXTENDS ValkeyRaftBase

\* design "Failure Detection", "Shard Epoch"
\* Protocol-visible part only. The FAILOVER_PREPARE / FAILOVER_PAUSED
\* handshake and REPL_OFFSETS are data-plane and stay out.
\* Value here is low until app state exists: FAILOVER, NODE_FAIL and
\* NODE_RECOVER are ordinary entries to Raft. What is worth checking is that
\* their apply reads only replicated state (the DelVoterReadsLeader lesson).

\* clusterRaftDetectFailures. Leader proposes NODE_FAIL(x) for any member x
\* (time abstracted). NODE_RECOVER(x) on an AE_ACK from a failed x.
DetectFailure(i, x) ==
    /\ FALSE   \* TODO
    \* writes: log, pending, messages
    /\ UNCHANGED <<currentTerm, votedFor, commitIndex, voters, volatileVars,
                  committed, joinVars, transferVars, trimVars>>

\* clusterRaftCron. Replica of a failed primary proposes FAILOVER with the
\* shard's current epoch; on rejection, re-schedules
\* (clusterRaftAutoFailoverCallback), no cap.
AutoFailover(i) ==
    /\ FALSE   \* TODO
    \* writes: log, pending, messages
    /\ UNCHANGED <<currentTerm, votedFor, commitIndex, voters, volatileVars,
                  committed, joinVars, transferVars, trimVars>>

--------------------------------------------------------------------------------
NextG5 ==
    \/ \E i, x \in Server : DetectFailure(i, x)
    \/ \E i \in Server : AutoFailover(i)

================================================================================
