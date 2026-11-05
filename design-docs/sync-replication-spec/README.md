# Acknowledged writes survive failover and restarts: a Lean proof for Valkey synchronous replication

Lean 4 proofs for the synchronous replication design in
[`design-docs/sync-replication.md`](../sync-replication.md), proposed in
[valkey#5286](https://github.com/valkey-io/valkey/pull/5286).

Readers new to machine-checked proofs: [Appendix A](#appendix-a-reading-a-lean-proof-as-a-systems-engineer)
explains what "proven" means here, what it does not mean, and the vocabulary
used below. [Appendix B](#appendix-b-the-tla-models-and-what-they-add) describes
the TLA+ models these proofs grew out of and the few results that rest on them
alone. Below, "design §N" refers to a section of the design doc.

## Summary

The design promises that acknowledged writes "survive as long as the primary
or at least one eligible sync replica survives" (Consistency and Durability
Properties). We prove this in Lean 4 for a model of the commit protocol, the
Cluster V2 shard configuration, failover and node restarts. The proof holds for
any number of nodes, any log length, any number of configuration changes and
restarts, and every value of `min-sync-replicas`.

The proof needs four things the design as written does not state. Three are
changes to the protocol, each closing a scenario in which an acknowledged or
committed write is lost. The fourth is a check the design implies but never
specifies. Each has a Lean counterexample: a concrete run of the model, without
it, that loses a write.

- **F1.** A new primary must reconcile against the log it inherited before it
  promotes replicas or serves writes.
- **F2.** A held reply may be released only while the primaryship that accepted
  the write is still current.
- **F5.** A primary that restarts with its data must rebuild its sync replica
  set from the shard configuration.
- **Incarnation check on primaryship.** Beyond the takeover compare-and-set, a
  node may act as primary only while its configuration records its current
  incarnation.

Separately, we prove the lease timing rules: exactly which clock-drift margin
makes the two leases safe (F11), and a sufficient condition for the "Lease
durations" rule, together with runs showing the design's own rule is not
sufficient (F12).

The proof is about safety of the replication log. It does not cover reads,
leases on the failover path, forced failover, or liveness (that a held reply is
ever released). Section 6 lists every exclusion and every assumption that is
stronger than the design.

Every theorem is checked by Lean's kernel. There is no `sorry`, and the only
axioms are Lean's standard three (`propext`, `Classical.choice`, `Quot.sound`).

## 1. The protocol as modelled

The model is `lean/RestartProof/Model.lean`. It follows the design's terms
(design §2, §4, §5, §6).

- **Roles.** A shard has a **primary**, **eligible** replicas (sync replicas
  that may be promoted) and **ineligible** replicas (async replicas and replicas
  still catching up).
- **Shard configuration.** Cluster V2 stores the primary, the eligible set and
  the **incarnation ID** recorded for each eligible replica. Every change
  increments the configuration version. Each node also keeps the configuration
  it last learned, which may be stale.
- **Replication.** A replica copies its primary's log in order and sends
  `REPLCONF ACK` for its replication offset.
- **Commit rule.** The primary's **committed offset** advances only past
  entries every member of its **sync replica set** has acknowledged, and only
  while at least `min-sync-replicas` of those members are eligible with a
  matching incarnation.
- **Promotion flow.** The primary first counts a caught-up replica's
  acknowledgements (step 2), then commits the replica as eligible to the
  cluster bus (step 4).
- **Demotion flow.** The primary commits the replica as ineligible and stops
  counting it, in one step.
- **Failover.** An eligible replica that knows the latest configuration takes
  over through the takeover compare-and-set. The compare-and-set checks that
  its incarnation matches the one recorded for it. The old primary is not
  stopped: it may keep acting on its stale configuration.
- **Full resync.** A replica whose log has diverged from its primary's is
  replaced by a prefix of the primary's log. This happens only after the
  replica is out of the sync replica set and ineligible.
- **Restarts.** A node may restart with its data, or without it. A restart
  without data gives the node a new incarnation ID. It rejoins as ineligible and
  becomes eligible again only through the promotion flow (design §6). Volatile
  state is lost in both cases: the committed offset, the sync replica set, the
  acknowledgements, and the client connections with their held replies.
- **Reply blocking.** A write's reply is held until it commits. It is then
  released as OK, or answered `UNCOMMITTED` (design §5, "Write behavior during
  faults").

Messages are not modelled as separate objects. A replica reads its primary's
log directly, and an acknowledgement updates the primary's view directly. A
delayed message corresponds to the same step taken later, and a lost
connection corresponds to the step never being taken. Steps interleave
arbitrarily, so any order of events across nodes is covered.

Each log entry records the **term** of the primary that wrote it: the version of
the shard configuration that made it primary. A write is identified by its
offset and term. This is what lets the model distinguish two primaries that
wrote different data at the same offset, which is exactly what the
counterexamples below do.

## 2. The guarantee

Two definitions turn the design's sentence into a statement.

- A node is **authoritative** if, in the latest shard configuration, it is the
  primary or an eligible replica, and the incarnation ID recorded for it is its
  current one. A node that restarted without data is therefore not
  authoritative, even if the configuration still lists it as eligible.
- The **committed history** is the sequence of entries committed so far. Each
  entry is the one at that offset on the first node to commit the offset. No
  node stores it. It is a proof device (a ghost variable) that records what any
  correct shard must keep.

**Survival condition.** Whenever a node restarts without its data, some other
authoritative node keeps its data. This is the design's "as long as the primary
or at least one eligible sync replica survives", required at each restart.

Under the survival condition, and with the F1, F2 and F5 fixes, every reachable
state satisfies:

| | Property | Lean theorem |
|---|---|---|
| **1** | **EligibleHoldsCommitted:** every authoritative node holds the whole committed history. | `RestartProof.safety` |
| **2** | **CommittedAgree:** every node's committed prefix is a prefix of the committed history. In particular, no two nodes commit different entries at the same offset. | `RestartProof.safety` |
| **3** | **AckedDurable:** every write released as OK is in the committed history, at its offset. | `RestartProof.ackSafety` |
| **4** | **AckedHeld:** every authoritative node holds every write released as OK, at its offset. | `RestartProof.ackedHeld` |
| **5** | With `min-sync-replicas ≥ 1`, when the current primary commits, the committed history is on at least two distinct authoritative nodes: the primary and an eligible sync replica. | `RestartProof.commit_two_copies` |

The committed history only grows, so property 3 says that no acknowledged write
is ever lost or replaced. Property 1 establishes the data half of the design's
failover argument, "any eligible replica is safe to promote, because each one
holds every committed write" (design §5, fault detection); the other half,
that the promoted replica then reconciles before serving, is F1.

With `min-sync-replicas = 0` the primary may commit with an empty sync replica
set. The history may then be on the primary only, which matches the design's
note that such writes "exist only on the primary". Property 5 is what
`min-sync-replicas ≥ 1` adds.

## 3. Four things the guarantee needs

Each item has a Lean counterexample: a run of the model with the item removed,
every guard checked by `decide`, ending in a state that violates the guarantee.
F1 and F5 use the model's own switches. F2 uses the held-reply layer's switch.
The incarnation check and the survival condition have no switch in the model, so
their counterexamples add one weakened action to the model's steps, named in the
theorem.

**F1: reconcile against the inherited log.** Promotion step 1 waits until the
replica has acknowledged up to the primary's committed offset. Right after a
failover, the new primary's committed offset can be behind writes the old
primary already committed. Scenario: n0 commits a write with n1 as its only sync
replica. n1 takes over with committed offset 0. n1 promotes n2, whose log is
empty. n2 is now eligible without the committed write.
Fix: until the new primary has committed its inherited log, promotion catch-up
and full resync are measured against the end of that log, and the primary
serves no writes. Counterexample: `CoreProof.noReconcile_unsafe` (11 steps),
`ReconProof.noReconcile_unsafe`.

**F2: fence held replies by primaryship.** Releasing a held reply once the
committed offset passes its offset is not enough, because the offset can be
reused by a later primaryship. Scenario: n0 writes at offset 1 and holds the
reply. n1 takes over with an empty log and n0 is fully resynced. n0 is promoted,
takes over again, writes different data at offset 1 and commits it. The old
reply is then released as OK for a write that was replaced.
Fix: release OK only while the node is still primary with the term the write
was tagged with. Otherwise answer `UNCOMMITTED`.
Counterexample: `AckProof.noReplyFence_unsafe` (14 steps).

**F5: a restarted primary rebuilds its sync replica set.** A primary that
restarts with its data loses its in-memory sync replica set. If it starts from
an empty set, it can commit without an eligible replica. Scenario: n0 promotes n1
and writes, restarts with its data, counts and promotes n2 (which has the
write), and commits with n2's acknowledgement only. n1 is eligible and
authoritative but lacks the committed write.
Fix: on restart, the primary's sync replica set is the eligible replicas whose
recorded incarnation matches, so it commits nothing until they acknowledge.
Counterexample: `RestartProof.noRebuild_unsafe` (9 steps, `min-sync-replicas = 1`).

**Incarnation check on primaryship.** Design §6 says the takeover
compare-and-set checks the incarnation, so a node that came back empty cannot
be promoted. That is not enough: the node may already be the primary. Scenario:
a write commits on n0 and n1, n0 restarts empty with a new incarnation, and n0
keeps acting as primary on its stale configuration. It counts n2, whose log is
empty, and makes it eligible. n2 is now authoritative without the committed
write.
Fix: a node acts as primary only if its configuration records its current
incarnation (`ThinksPrimary` in the model).
Counterexample: `RestartProof.noIncCheck_unsafe` (9 steps): the model's steps
plus Count and MakeEligible without this check, every fix on.
The model also checks incarnations in two other places: a primary does not count
an eligible replica whose recorded incarnation does not match (`Count`), and
only matching replicas count toward `min-sync-replicas` (`Writable`). The proof
uses both, but no counterexample shows that either is needed on its own.

**Survival condition.** Without it, both copies of a committed write can restart
empty. Scenario: a write commits on n0 and n1, n1 restarts empty, then n0
restarts empty. No node holds the write, and no node is authoritative.
Counterexample: `RestartProof.noSurvivor_loses` (9 steps): the model's steps plus
RestartEmpty without its guard. Because no node is authoritative, property 1
holds only vacuously in that state, so the theorem states the loss directly: the
committed history is non-empty and no node's log holds it. The design calls this
case correlated data loss.

## 4. Method

### 4.1 The inductive invariant

The proof is by induction over executions. We define a predicate `Inv` on
states and show that it holds initially, that every step preserves it, and that
it implies properties 1 and 2. `Inv` has 21 parts, grouped in five layers that
follow the design: configuration, replication, membership, commit and failover.
The ones that carry the argument are:

- **EligibleHoldsCommitted** itself, restricted to authoritative nodes.
- **Acknowledgements are real.** A replica that follows a primary holds that
  primary's log up to the offset the primary has recorded as acknowledged by it.
  The committed offset of a primary never passes the acknowledged offset of any
  member of its sync replica set.
- **Eligible replicas are counted.** A node that thinks it is primary counts
  every replica that is eligible, with a matching incarnation, in the
  configuration it knows. This is the conjunct F5 breaks.
- **Ordering rules of the promotion flow.** A replica the current primary counts
  but has not yet made eligible has acknowledged at least the reconciliation
  point. F1's reconciliation rule is what preserves it.
- **A deposed primary is fenced.** Some members of a deposed primary's sync
  replica set have left it for a newer primary, so their acknowledgements are
  frozen. The smallest of them is the deposed primary's **fence**: by the commit
  rule it can never commit past it. Every eligible replica either holds the
  deposed primary's log up to the fence or is still following it. This is the
  design's "Writes can't split-brain" argument, made precise: the deposed primary
  may still commit, but only entries the new configuration already holds.
- **Incarnations only grow**, and a configuration never records an incarnation
  newer than the node's own. So a node that restarted without data never matches
  its old record.
- **Some node is authoritative.** The survival condition keeps this true.

The acknowledged-write properties (3 and 4) extend `Inv` with three more
conjuncts. Terms never exceed the configuration version. A held reply whose
primaryship is still current points at the write it was issued for.
AckedDurable itself. A takeover gives the new primary a term larger than every
existing tag, so an old reply can never become releasable again.

### 4.2 One proof, three corollaries

The proof has four models of increasing generality, each in its own directory.
`CoreProof` has `min-sync-replicas = 0`. `AckProof` adds held replies.
`ReconProof` allows every `min-sync-replicas` value. `RestartProof` adds
restarts and incarnation IDs. Only the most general, `RestartProof`, has an
invariant proof. Each smaller model is mapped into the next by a function on
states, and every step of the smaller model is shown to be a step of the larger
one (a simulation):

```
CoreProof  --toRecon-->  ReconProof at min-sync-replicas 0  --emb-->  RestartProof with no restarts
AckProof   --toRecon-->  ReconProof with held replies       --emb-->  RestartProof with held replies
```

`emb` gives every node incarnation 1, so every incarnation check holds. A
`CoreProof` step is a `ReconProof` step at `min-sync-replicas = 0` under F1's
reconciliation rule. The safety theorems of the smaller models are then a few
lines each. The smaller models are kept because each states its own
counterexample (F1 in `CoreProof` and `ReconProof`, F2 in `AckProof`).

### 4.3 Lease timing

The lease proofs (`Lease/`) are independent of the replication model. They
model one lease instance in real time. The holder sends at time `s` and the
grantor receives at `r ≥ s`, with unbounded delay. Clocks are arbitrary
functions from real time to local readings, under two drift models: additive
(local elapsed time is within `D` of real elapsed time) and rate-bounded (drift
rate at most `ρ`). The main results:

- **F11.** If the holder subtracts margin `M` and the grantor does not, the
  leases are safe exactly when `M ≥ 2D`, apart from the degenerate case where
  the margin is the whole lease (`asym_safe_iff`). In the rate model the
  condition is `M ≥ 2ρ·Dur/(1+ρ)` (`rate_asym_safe`, `rate_asym_tight`); its
  tightness holds up to a refinement of the real-time tick, since the overlap
  window can be shorter than one integer tick. If both sides subtract the
  margin, which is one reading of "Both sides use monotonic clocks and subtract
  a clock-drift margin", the leases are unsafe for every margin shorter than
  the lease as soon as there is any drift (`literal_unsafe`).
- **F12.** The rule "read lease + time to commit a demotion + clock-drift margin
  < primary lease" (design §5, "Lease durations") is not sufficient. It must
  also cover the acknowledgement round trip and a second drift term. The theorem
  `demotion_in_time` gives a sufficient condition, and
  `doc_condition_insufficient_drift` and `_rtt` give runs where the doc's
  condition holds and the primary lease has already expired when the demotion
  commits. This is an availability issue, not a safety one: the primary then
  stops serving reads.

## 5. Results

| Result | Lean theorem |
|---|---|
| Properties 1 and 2, every `min-sync-replicas`, with restarts | `RestartProof.safety` |
| Properties 3 and 4 (no acknowledged write lost) | `RestartProof.ackSafety`, `ackedHeld` |
| Property 5 (two copies with `min-sync-replicas ≥ 1`) | `RestartProof.commit_two_copies` |
| The same for the smaller models | `CoreProof.safety`, `AckProof.ackSafety`/`ackedHeld`, `ReconProof.safety`/`ackSafety`/`ackedHeld` |
| F1 needed | `CoreProof.noReconcile_unsafe`, `ReconProof.noReconcile_unsafe` |
| F2 needed | `AckProof.noReplyFence_unsafe` |
| F5 needed | `RestartProof.noRebuild_unsafe` |
| Incarnation check on primaryship needed | `RestartProof.noIncCheck_unsafe` |
| Survival condition needed | `RestartProof.noSurvivor_loses` |
| Lease margin (F11) and durations (F12) | `Lease/` |

Every row has a Lean theorem. The TLA+ model checking that found these results
is in Appendix B.

Size: the `RestartProof` model is 429 lines (with the held-reply layer), the
invariant and its lemmas 383, the per-action contracts 460, and the preservation
proofs 1,895, so the proof is about six lines per line of model. Lean checks the
whole of `RestartProof` in about 13 seconds on a laptop-class machine.

[lean/LEAN.md](lean/LEAN.md) has every theorem statement and each model's
details.

## 6. Scope and limitations

### Not modelled

- **Forced failover** (`CLUSTER FAILOVER FORCE`, `TAKEOVER`). The design accepts
  losses when the target is ineligible (F7). F7 and F8 are explored in TLA+ only
  (Appendix B).
- **Reads.** The proofs say nothing about linearizable reads from the primary
  or from sync replicas. Leases appear only in the timing lemmas of `Lease/`,
  which cover one lease instance, not the protocol: in the replication model a
  takeover needs no lease expiry and may pick any eligible replica. Stale reads
  are checked by TLA+ model checking only (Appendix B).
- **Liveness.** Nothing proves that a held reply is ever released, as OK or as
  `UNCOMMITTED`. The model's `UNCOMMITTED` (`abort`) fires only when the
  primaryship that took the write is no longer current. The design's other
  case, a healthy primary whose sync replica set fell below `min-sync-replicas`
  releasing held writes as `UNCOMMITTED`, is not modelled, and neither is the
  `CLUSTERDOWN` rejection path or client retry semantics.
- **Correlated data loss** is excluded by the survival condition. The design's
  detection of it (no eligible replica with a matching incarnation, so the shard
  stays unavailable) is not proven.
- AOF and `appendfsync`; async read replicas beyond being ineligible (in the
  model any follower may replicate uncommitted entries, while the design caps an
  async replica's stream at the committed offset); `target-sync-replicas` and
  the choice of which replica to promote (the model allows any promotion);
  replication lag thresholds; manual failover; slot migration and anything
  beyond a single shard; modules; dirty key tracking.
- **The Cluster V2 bus is assumed correct.** It is modelled as one totally
  ordered configuration with compare-and-set updates, as design §6 requires.
- **Messages are abstracted** (section 1): there is no reordering within a
  connection.

### Assumptions stronger than the design

- **The survival condition** is required at every restart without data
  (section 2).
- **Restarts carry three assumptions the design only implies.** (a) A node
  restarts only when it knows the latest configuration (`KnowsLatest`),
  justified by design §6's "syncs to the latest log before it re-enters the
  cluster"; a restart with a stale configuration is excluded, not proved
  equivalent. (b) A restart without data at once removes the node from the
  *current* primary's sync replica set and acknowledgement table; the design
  reaches that state only through the demotion flow (read-lease wait plus a
  committed demotion), so the model omits the window in which the primary is
  still waiting for the dead replica. (c) A restart with data forces the node to
  re-commit its whole log before serving (`recon = Len(log)`); the design does
  not specify this.
- **The incarnation checks** in `ThinksPrimary`, `Count` and `Writable` are in
  the model as guards. The design states only the takeover compare-and-set.
  Section 3 shows the first is needed; the other two are assumed.

### What the model is checked against

The Lean model is written by hand from the design. Nothing mechanical checks it
against the design doc itself; sections 1 and 6 are the argument, and reviewing
them is the step that needs a systems engineer (Appendix A.6). The model was
also written to mirror a TLA+ model, and for two of the four models (`CoreProof`,
`AckProof`) the Lean steps were checked against that TLA+ model's complete state
graph at small sizes (Appendix B.3; the checker is not part of this tree). That
catches slips in how the Lean was written, not differences between both models
and the design.

"Any number of nodes" means any Lean type of node identifiers, including
infinite ones. `Writable` uses `Set.encard`, which is the usual cardinality on
finite sets and `⊤` on infinite ones. The theorems hold for infinite node types
too, but only the finite case corresponds to a real cluster.

### Future work

Cross-check the `RestartProof` encoding against its TLA+ state graph. Encode
Core's stricter reconciliation rule at `min-sync-replicas ≥ 1` (Appendix B.2).
Determine whether the `Count` and `Writable` incarnation checks are each needed.

## 7. How the proof is organized in code

| Path | Contents | Lines |
|---|---|---|
| `lean/RestartProof/Model.lean`, `AckModel.lean` | The model: state, actions, steps, properties; the held-reply layer | 429 |
| `lean/RestartProof/Contracts.lean` | Each action's effect in design terms, proved without the invariant | 460 |
| `lean/RestartProof/Inv.lean` | The invariant in five layers; the fence; frame lemmas | 383 |
| `lean/RestartProof/Preserve/` | Preservation, one file per subsystem: `Replication`, `Commit`, `Config`, `Failover`, `Restart`; each action proved one layer at a time | 1,895 |
| `lean/RestartProof/Safety.lean`, `AckProof.lean`, `Lists.lean` | The main theorems; the held-reply invariant; list lemmas | 320 |
| `lean/RestartProof/Counterexample.lean` | F5, incarnation-check and survival counterexamples | 167 |
| `lean/ReconProof/`, `lean/CoreProof/`, `lean/AckProof/` | The smaller models, their embeddings into the next model, corollaries, F1 and F2 counterexamples | 1,030 |
| `lean/Lease/` | Lease timing (additive and rate clocks, durations) | 377 |
| `lean/Check.lean` | Prints the axioms of every main theorem and counterexample | |

Lean 4.35 with Mathlib `v4.35.0`, used for list-prefix lemmas and for `Set.encard`
in `Writable`.

**Reading order.** `Model.lean` (the state machine), then `Inv.lean` (the five
layers; each conjunct's docstring cites the design), then `Contracts.lean` (what
each action does), then `Preserve/Failover.lean` (Takeover, the hardest action,
proved layer by layer), then `Safety.lean`. `Preserve.lean` is the one-line-per-
action dispatch that ties preservation to the step relation.

**Conventions.** Actions are functions `State → State` using structure update,
so fields not mentioned are unchanged. Guards live in the `Step` constructors.
`Reachable` is the inductive closure of `Step` from `init`. Names follow the
design where it has one (`ThinksPrimary`, `KnowsLatest`, `IsAuthoritative`,
`Writable`, `catchUpPoint`). A counterexample is a concrete run on `Fin 3` whose
guards `decide` evaluates.

## 8. Reproducing

```
cd lean
../tools/lean.sh lake build                 # builds every proof
../tools/lean.sh lake env lean Check.lean   # prints the axioms of every main theorem
```

`tools/lean.sh` runs the Lean toolchain in a container. Unpack the toolchain
into `tools/elan/`, build the image once
(`docker build -t valkey-lean:24.04 -f tools/Dockerfile.lean tools`), and fetch
Mathlib's prebuilt files once with `../tools/lean.sh lake exe cache get` (about
7 GB). A clean build then takes about 30 seconds.

## References

- Valkey Durability: Synchronous Replication Architecture,
  [`design-docs/sync-replication.md`](../sync-replication.md)
  ([valkey#5286](https://github.com/valkey-io/valkey/pull/5286)).
- Findings F1 to F12 and the TLA+ models:
  [`sync-repl-formal-spec`](https://github.com/jjuleslasarte/valkey/tree/sync-repl-formal-spec)
  branch, `FINDINGS.md` and `spec/`.
- W. Lin, M. Yang, L. Zhang, L. Zhou. PacificA: Replication in Log-Based
  Distributed Storage Systems. Microsoft Research, MSR-TR-2008-25, 2008.

---

## Appendix A: Reading a Lean proof as a systems engineer

This appendix is for reviewers who know distributed systems and have not worked
with a proof assistant. It explains what the proof is, what "proven" means
here, where the trust actually sits, and the words used above.

### A.1 What a machine-checked proof is

A proof assistant is a type checker for arguments. You write a precise
statement, then a proof, and the tool checks every step against a small fixed
set of logical rules. There is no "the reviewer was convinced"; either the proof
type-checks or it does not. Lean 4 is one such tool. Its checker (the *kernel*)
is a few thousand lines, independently re-implemented several times, and every
theorem in this repository passes it.

The useful analogy is a test suite with infinite coverage. A test runs the
system on one input and checks one output. A proof is a single argument that
covers every input at once: here, every number of nodes, every log, every
interleaving of every action, every sequence of configuration changes and
restarts. That is why a proof can say "never" where a test can only say "not in
the cases I tried".

### A.2 What was proven, in one sentence each

The statement proven is of the form: *every state the system can reach
satisfies property P*. "Can reach" means: start from the initial state and take
any finite sequence of steps the model allows. For the main theorem:

> For every type of node identifier, every `min-sync-replicas` value, every
> initial primary, and every reachable state: every authoritative node holds the
> whole committed history, and no node's committed prefix disagrees with it.

Lean's version of that sentence is `RestartProof.safety`, and the words map one
to one onto its symbols (`∀ {Node} [DecidableEq Node] {m} {i} {s}, Reachable m
true true i s → Safety s`). Section 2 translates each of the five theorems the
same way.

A counterexample theorem has the opposite form: *there is* a reachable state,
in the model with one check removed, that violates the property. It proves the
check is needed, the way a failing test proves a bug exists.

### A.3 What "proven" does not mean

A proof is only as good as the model and the statement. Three things to keep
separate:

1. **The model** (section 1 and `Model.lean`) is a simplified description of the
   protocol. The proof says nothing about behaviours the model leaves out
   (section 6, "Not modelled"), and it is vacuous for behaviours the model
   forbids that the real system allows (section 6, "Assumptions stronger than
   the design"). Checking that the model matches the design is a human task.
2. **The statement** must say what you care about. "No committed write is lost"
   is encoded as EligibleHoldsCommitted plus the definition of *authoritative*.
   If that definition is too narrow, the theorem is true but uninteresting. The
   survival counterexample in section 3 is an example: with no authoritative
   node left, property 1 is true and the data is gone. Read the definitions in
   section 2 as carefully as the theorems.
3. **The proof** is the part the machine checks, and the part you need not
   re-check. Its internal structure (section 4.1) matters only for
   understanding *why* the property holds, not for whether it does.

Also outside the proof: liveness (something good eventually happens), timing,
performance, and the correctness of the Valkey implementation. The proof is
about the protocol as designed, not as coded.

### A.4 Where the trust sits

From most to least trusted:

- **The Lean kernel and its three standard axioms** (`propext`,
  `Classical.choice`, `Quot.sound`). These are the same for every Lean proof,
  including all of Mathlib, and are not in question. The absence of `sorry` (a
  placeholder that lets an unproven statement type-check) is confirmed by
  `Check.lean`.
- **The theorem statements.** Short, and the thing to read. If a statement is
  wrong, the proof proves the wrong thing.
- **The definitions the statements use.** `Safety`, `IsAuthoritative`,
  `Reachable`, `Step`. A bug here is the most likely way for a correct proof to
  fail to mean what section 2 says. These are a few hundred lines
  (`Model.lean`).
- **The model's fidelity to the design.** Argued in sections 1 and 6, checked
  by hand, and the main residual risk.

### A.5 Vocabulary

- **State machine / transition system.** The model: a set of states, an initial
  state, and a relation saying which steps are allowed. Each step is an action
  with a guard (precondition) and an effect.
- **Invariant.** A property true of every reachable state. **Inductive**
  invariant: one that is also preserved by every single step, from any state
  where it holds. Finding one is the hard, creative part of the proof. `Inv` (21
  conjuncts) is the inductive strengthening of `Safety` (2 conjuncts). The extra
  19 are the facts you must carry along for the induction to go through. Each
  one is there because some step broke the proof without it.
- **Preservation lemma.** "If `Inv` holds before action X, it holds after." One
  per action, in `Preserve/`. This is where the bulk of the proof lives.
- **Ghost variable.** A variable added to the model for the proof's sake that no
  real node stores and that changes no behaviour. `hist`, the committed history,
  is one. It lets the property be stated as "every authoritative node holds
  `hist`" instead of a quantifier over all pairs of nodes.
- **Simulation / embedding.** A function from the states of a smaller model to
  the states of a larger one, with a proof that every step of the smaller model
  maps to a step of the larger. It transports theorems downward, so the three
  smaller models here are corollaries of one proof (section 4.2).
- **Counterexample.** A concrete run of the model that ends in a state violating
  the property. In Lean it is a list of steps whose guards the tool evaluates.
- **Vacuous.** True because there is nothing to check. "Every authoritative node
  holds the history" is vacuously true when no node is authoritative.
- **Axiom.** A statement accepted without proof. Lean's three standard ones are
  universally used; the thing to check is that a proof uses *only* those. `sorry`
  would appear as a fourth, `sorryAx`.
- **Decidable / `decide`.** A proposition the tool can evaluate to true or false
  by computation. The counterexamples rely on this: the guards and the
  properties are decidable on concrete finite states, so Lean checks them by
  running them.
- **Structure / `extends`.** Lean's record types. `Inv` is a record with 21
  named fields grouped in five sub-records, so a proof of `Inv` is built field
  by field and each field can be read, and cited, on its own.

### A.6 How to review this

1. Read section 2 and check the definitions of *authoritative*, *committed
   history* and the five properties against what you would want the protocol
   to guarantee. This is the step that needs a systems engineer, not a Lean
   expert.
2. Read section 1 and section 6 against the design doc. Anything the model
   leaves out or assumes is listed there; if something you rely on is missing,
   the proof does not cover it.
3. Spot-check `Model.lean` for one or two actions you know well (Takeover and
   RestartEmpty are good choices) against the design.
4. Trust the kernel for the rest. `lake build` with no errors and `Check.lean`
   printing only the three standard axioms is the whole verification; nothing
   else needs re-reading to confirm the theorems hold.

---

## Appendix B: The TLA+ models and what they add

The findings were first found with TLA+ models of the design, checked with the
TLC model checker, on the
[`sync-repl-formal-spec`](https://github.com/jjuleslasarte/valkey/tree/sync-repl-formal-spec)
branch. TLC explores every reachable state of a model up to a size bound (here 3
nodes, logs of length 2, up to 6 configuration versions, 1 restart). It is fast
to iterate with and produces a concrete trace when something fails, but it says
nothing beyond the bound. The Lean model was then written to mirror the TLA+
model of the same name, and Lean proves the properties without a bound.

For everything in sections 2 to 5, the Lean results stand on their own and the
TLA+ runs are redundant: the bounded "safe" runs are subsumed by the Lean
theorems, and every "needed" result now has a Lean counterexample. What TLA+
still contributes is below.

### B.1 Results that rest on TLA+ alone

- **Stale reads.** `NoStaleLinRead` in `spec/Leases.tla` checks, under the
  lease rules, that no linearizable read returns stale data. There is no Lean
  counterpart.
- **Forced failover and the lease fence on acknowledgements** (findings F7 and
  F8) are explored in `spec/Shard.tla` and `spec/Leases.tla`. The Lean model has
  no forced failover.
- **The Count and Writable incarnation checks.** TLC's `CoreRestart_noIncMin1`
  disables all three incarnation checks at once and finds a loss. The Lean
  counterexample isolates the `ThinksPrimary` check. Neither tool shows that the
  other two are each needed.

### B.2 How the Lean model differs from the TLA+ model

The Lean `RestartProof` model has the same variables, actions and guards as the
replication part of `spec/CoreRestart.tla`, except:

- The TLC bounds (`MaxLen`, `MaxVer`, `MaxRestarts`) are dropped.
- TLA+'s `CommitCap`, a `CHOOSE` of the minimum acknowledgement, is written as
  the equivalent universal statement.
- TLA+'s `NoSurvivor` switch cuts the ghost history to empty on a restart that
  leaves no survivor. The Lean model has no such cut; its survival
  counterexample states the loss directly instead.
- The held-reply layer is a strict superset of `CoreRestart.tla`: it also admits
  an append that records no reply and a restart that keeps held replies. A
  safety theorem for the superset covers the original.
- Core's stricter reconciliation rule (`StrictRecon` in `spec/CoreRecon.tla`) is
  not encoded. Every step under it is a step under F1's rule, which the theorem
  covers, so it is covered by argument; at `min-sync-replicas = 0` this is a Lean
  lemma (`CoreProof.step_toRecon`).

### B.3 The state-graph cross-check

To catch slips in writing the Lean model, a separate checker compared it with
TLC's complete state graph for a small configuration (3 nodes, logs of length 1,
up to 5 configuration versions). For `CoreProof` (45,284 states, 146,532
transitions) and `AckProof` (111,840 states, 410,788 transitions):

- TLC's initial state is Lean's initial state;
- every TLC transition is a Lean step with the same action;
- every Lean step from a TLC state that stays within TLC's bounds is a TLC
  transition;
- the invariant `Inv` holds on every TLC state.

All checks passed with no mismatches, at the same theorem statements as this
tree, and a mutation test (one state and one transition label changed) made them
fail. The one difference is intended: the Lean held-reply model also allows a
write that records no reply. The checker evaluated Boolean versions of `Step` and
`Inv`, with soundness lemmas proving that a `true` answer implies the
proposition. `RestartProof` was not cross-checked.

The checker needs TLC and the TLA+ models, so it is not part of this Lean-only
tree. It will come back with the TLA+ models; until then it is in this branch's
history at commit `bb987c679` (`lean/XCheck/`, `xcheck/dot2lean.py`).

### B.4 The TLC runs behind each finding

| Finding | TLC run(s) | Lean counterexample |
|---|---|---|
| F1 | `CoreRecon_noReconcileMin0`/`Min1`, `CoreRestart_noReconcileMin1` | `CoreProof.noReconcile_unsafe`, `ReconProof.noReconcile_unsafe` |
| F2 | `CoreAck_noReplyFence`, `CoreRestart_noReplyFenceMin1` | `AckProof.noReplyFence_unsafe` |
| F5 | `CoreRestart_noRebuildMin1` (10 states), `_noRebuildMin1Loss` (22) | `RestartProof.noRebuild_unsafe` |
| Incarnation checks | `CoreRestart_noIncMin1` (all three off) | `RestartProof.noIncCheck_unsafe` (`ThinksPrimary` check only) |
| Survival | `CoreRestart_noSurvivorMin0`/`Min1` | `RestartProof.noSurvivor_loses` |
| Safe runs | `CoreRestart_safeMin0`/`_safeMin1` (120.6 M and 78.8 M states), `CoreRestart_twoCopiesMin1` | subsumed by `RestartProof.safety`, `ackSafety`, `commit_two_copies` |
| Stale reads | `Leases.tla` (`NoStaleLinRead`) | none |
