------------------------------ MODULE RaftJoin ---------------------------------
(***************************************************************************)
(* G3 Joining and leaving (primer G3). All STAGED: NextG3 is not in Next.   *)
(* Enabling also needs the "Fresh" Init (ValkeyRaftBase).                   *)
(* design "Cluster Formation (MEET/ADD_ME/WELCOME)", "Membership changes"   *)
(***************************************************************************)
EXTENDS ValkeyRaftBase

\* Singleton predicate (clusterRaftIsSingleton): at most one voter and no
\* non-JOINING peers. IsSingleton(i) == Cardinality(voters[i]) <= 1 /\
\* members[i] = {i}

\* Admin CLUSTER MEET j on i (clusterRaftSendMeet, design "Cluster Formation
\* (MEET/ADD_ME/WELCOME)"). HELLO/HI abstracted. Send MEET with msingleton =
\* IsSingleton(i). The blocked client and its unblock conditions (design
\* "Unblock conditions for CLUSTER MEET") are not modelled.
AdminMeet(i, j) ==
    /\ FALSE   \* TODO
    \* writes: messages
    /\ UNCHANGED <<serverVars, volatileVars, committed, joinVars,
                  proposalVars, transferVars, trimVars>>

\* clusterRaftProcessMeet. Branches on (IsSingleton(i), m.msingleton, role[i]):
\*   both in a cluster          -> reply MEET_REJECTED
\*   i in a cluster, j single   -> reply WELCOME, Invite(i, j)
\*   i single and Leader        -> SingletonStepDown(i), reply ADD_ME
\*   i single, not Leader       -> add j to deferredMeets[i]
\* The last branch also catches a node after ForgetMyself (Learner, no
\* voters, no peers). Hypothesis to check: such a node defers every MEET
\* and, not being a Joiner, never reverts via JoinerTimeout.
HandleMeet(i, j, m) ==
    /\ FALSE   \* TODO
    \* writes: messages, deferredMeets, role, leader, votedFor, log,
    \*         commitIndex, pending
    /\ UNCHANGED <<currentTerm, voters, preVotes, votes, nextIndex,
                  matchIndex, replicating, pendingConfIndex, committed,
                  members, transferVars, trimVars>>

\* clusterRaftProcessAddme. Invite(i, j). (Also unblocks the MEET client: not
\* modelled.)
HandleAddMe(i, j, m) ==
    /\ FALSE   \* TODO
    \* writes: log, pending, messages
    /\ UNCHANGED <<currentTerm, votedFor, commitIndex, voters, volatileVars,
                  committed, joinVars, transferVars, trimVars>>

\* clusterRaftProcessWelcome. If i is a singleton Leader:
\* SingletonStepDown(i). Else no change.
HandleWelcome(i, j, m) ==
    /\ FALSE   \* TODO
    \* writes: role, leader, votedFor, log, commitIndex, messages
    /\ UNCHANGED <<currentTerm, voters, preVotes, votes, nextIndex,
                  matchIndex, replicating, pendingConfIndex, committed,
                  joinVars, proposalVars, transferVars, trimVars>>

\* clusterRaftProcessMeetRejected. Only effect is on the blocked client:
\* Discard(m).
HandleMeetRejected(i, j, m) ==
    /\ FALSE   \* TODO
    \* writes: messages
    /\ UNCHANGED <<serverVars, volatileVars, committed, joinVars,
                  proposalVars, transferVars, trimVars>>

\* clusterRaftProcessDeferredMeets. Once i has a peer member, reply WELCOME to
\* every deferred sender and Invite it; clear deferredMeets[i]. Code runs this
\* after applying i's own NODE_JOIN; here it is enabled whenever its guard
\* holds.
ProcessDeferredMeets(i) ==
    /\ FALSE   \* TODO
    \* writes: deferredMeets, log, pending, messages
    /\ UNCHANGED <<currentTerm, votedFor, commitIndex, voters, volatileVars,
                  committed, members, transferVars, trimVars>>

\* clusterRaftCron (4x node timeout, design "Why early unblock is safe").
\* Joiner reverts: role Leader, currentTerm + 1, votedFor = leader = i. Log is
\* empty (wiped by the step-down), so SelfJoin runs on the next append. The
\* term bump is why the design doc orders this after proposal expiry (design
\* "Why early unblock is safe").
JoinerTimeout(i) ==
    /\ FALSE   \* TODO
    \* writes: role, currentTerm, votedFor, leader
    /\ UNCHANGED <<messages, log, commitIndex, voters, preVotes, votes,
                  nextIndex, matchIndex, replicating, pendingConfIndex,
                  committed, joinVars, proposalVars, transferVars, trimVars>>

\* Admin CLUSTER FORGET x on i (design "Typed Log Entries",
\* clusterRaftApplyNodeForget): Propose(i, NodeForget x). Pre-validation on
\* the leader with x = i returns LEADER_TRANSFER: StartTransfer(i) and REJECT
\* "transfer" (or "rejected" if no eligible target, clusterRaftProcessPropose)
\* instead of appending (design "Leader Transfer").
AdminForget(i, x) ==
    /\ FALSE   \* TODO
    \* writes: log, pending, messages, transferVars
    /\ UNCHANGED <<currentTerm, votedFor, commitIndex, voters, volatileVars,
                  committed, joinVars, trimVars>>

\* Helpers (state-change operators, not actions):

\* clusterRaftSingletonStepDown. role Joiner, leader = votedFor = Nil, log =
\* << >>, commitIndex = 0, pending proposals deferred
\* (clusterRaftDeferPendingProposals). This DISCARDS a log, which Raft assumes
\* a server never does. Safe only if no other server counts i as a voter.
\* Candidate invariant: Joiner(i) => \A j /= i : i \notin voters[j].
\* writes (primed): role, leader, votedFor, log, commitIndex; the caller
\*   supplies UNCHANGED for the rest.
SingletonStepDown(i) == FALSE

\* clusterRaftInvitePeer. Propose(i, MkProposal(EntJoinLearner, x)) unless
\* a NODE_JOIN for x is already in i's in-memory log. After trimming, a
\* duplicate can be appended; apply treats a known node as an address update
\* (raftLogApply), so it must not change voters or members.
\* writes (primed): log, pending, messages; the caller supplies UNCHANGED for
\*   the rest.
Invite(i, x) == FALSE

\* clusterRaftSelfJoin. Singleton leader with an empty log appends
\* NodeJoinVoter(i) (and SLOT_CHANGE for its slots: folded into Data).
\* Quorum-changing: 0 -> 1.
\* writes (primed): log; the caller supplies UNCHANGED for the rest.
SelfJoin(i) == FALSE

\* Apply rules to add to ApplyVoters (and a new ApplyMembers) with G3:
\*   NodeJoinLearner(x): members \cup {x}; voters unchanged (raftLogApply).
\*   NodeJoinVoter(x):   members \cup {x}; voters \cup {x} ONLY on first join
\*                       (x new or JOINING, raftLogApply). Not idempotent in
\*                       the voter count if first_join is mis-tracked.
\*   NodeForget(x), x /= self: members \ {x}, voters \ {x}
\*     (clusterRaftApplyNodeForget).
\*   NodeForget(self): ForgetMyself (clusterRaftForgetMyself): role Learner,
\*                     members = {i}, voters = {}, leader = votedFor = Nil.
\* Rejections at apply (shard epoch, primary with slots/replicas) depend on
\* app state; until it exists, model them as never firing.

--------------------------------------------------------------------------------
NextG3 ==
    \/ \E i, j \in Server : AdminMeet(i, j)
    \/ \E i \in Server : ProcessDeferredMeets(i)
    \/ \E i \in Server : JoinerTimeout(i)
    \/ \E i, x \in Server : AdminForget(i, x)
    \/ \E m \in DOMAIN messages :
          LET i == m.mdest
              j == m.msource
          IN \/ m.mtype = MsgMeet          /\ HandleMeet(i, j, m)
             \/ m.mtype = MsgAddMe        /\ HandleAddMe(i, j, m)
             \/ m.mtype = MsgWelcome       /\ HandleWelcome(i, j, m)
             \/ m.mtype = MsgMeetRejected /\ HandleMeetRejected(i, j, m)

================================================================================
