# Acknowledged writes survive failover and restarts: a Lean proof for Valkey synchronous replication

Lean 4 proofs for the synchronous replication design in
[`design-docs/sync-replication.md`](../sync-replication.md), proposed in
[valkey#5286](https://github.com/valkey-io/valkey/pull/5286). The TLA+ models
these proofs encode, the TLC results and the full findings are on the
[`sync-repl-formal-spec`](https://github.com/jjuleslasarte/valkey/tree/sync-repl-formal-spec)
branch.

## Summary

The design promises that acknowledged writes "survive as long as the primary
or at least one eligible sync replica survives" (Consistency and Durability
Properties). We prove this in Lean 4 for a model of the commit protocol, the
Cluster V2 shard configuration, failover and node restarts. The proof holds for
any number of nodes, any log length, any number of configuration changes and
restarts, and every value of `min-sync-replicas`.

The proof needs three changes to the design as written. Each one closes a
scenario in which an acknowledged or committed write is lost, and each such
scenario has a concrete counterexample:

- **F1.** A new primary must reconcile against the log it inherited before it
  promotes replicas or serves writes.
- **F2.** A held reply may be released only while the primaryship that accepted
  the write is still current.
- **F5.** A primary that restarts with its data must rebuild its sync replica
  set from the shard configuration.

Separately, we prove the lease timing rules exactly: which clock-drift margin
makes the two leases safe (F11), and what the "Lease durations" rule must also
account for (F12).

Every theorem is checked by Lean's kernel. There is no `sorry`, and the only
axioms are Lean's standard three (`propext`, `Classical.choice`, `Quot.sound`).

## 1. The protocol as modelled

The model follows the design's terms (sections 2, 4, 5 and 6).

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
  becomes eligible again only through the promotion flow (section 6). Volatile
  state is lost in both cases: the committed offset, the sync replica set, the
  acknowledgements, and the client connections with their held replies.
- **Reply blocking.** A write's reply is held until it commits. It is then
  released as OK, or answered `UNCOMMITTED` (section 5, "Write behavior during
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
is ever lost or replaced. Property 1 says the design's failover argument is
sound: "any eligible replica is safe to promote, because each one holds every
committed write" (section 5, fault detection).

With `min-sync-replicas = 0` the primary may commit with an empty sync replica
set. The history may then be on the primary only, which matches the design's
note that such writes "exist only on the primary". Property 5 is what
`min-sync-replicas ≥ 1` adds.

## 3. Three changes the guarantee needs

Each change has a scenario that loses a write without it. The scenarios come
from TLC runs of the TLA+ models. F1 and F2 are also reproduced in Lean as
explicit runs, with every guard checked by `decide`.

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
an empty set, it can commit without an eligible replica. Scenario (TLC,
`CoreRestart_noRebuildMin1`, 10 states): n1 promotes n2 and writes, restarts with
its data, counts and promotes n3, and commits with n3's acknowledgement only. n2
is eligible but lacks the committed write. A longer run
(`CoreRestart_noRebuildMin1Loss`, 22 states) goes on to replace the committed
write.
Fix: on restart, the primary's sync replica set is the eligible replicas whose
recorded incarnation matches, so it commits nothing until they acknowledge.

The survival condition is also necessary. Without it, an authoritative node that
holds the only copy of an acknowledged write can restart empty
(`CoreRestart_noSurvivorMin1`). The design calls this case correlated data loss.

## 4. Method

### 4.1 From TLA+ to Lean

The design was first modelled in TLA+ and checked with TLC for small
configurations (3 nodes, logs of length 2, a few configuration changes and one
restart). The Lean model is a direct encoding of the replication part of
`spec/CoreRestart.tla`. Its state has the same variables, and its steps have the
same actions and guards. The only difference is that the TLC bounds are
dropped. Lean then proves the properties for all sizes, which model checking
cannot do.

### 4.2 The inductive invariant

The proof is by induction over executions. We define a predicate `Inv` on
states and show that it holds initially, that every step preserves it, and that
it implies properties 1 and 2. `Inv` has 21 parts. The ones that carry the
argument are:

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

### 4.3 One proof, three corollaries

There are four TLA+ models of increasing generality. Core has
`min-sync-replicas = 0`. CoreAck adds held replies. CoreRecon allows every
`min-sync-replicas` value. CoreRestart adds restarts and incarnation IDs. Only
the most general model, CoreRestart, has an invariant proof. Each smaller model
is mapped into the next by a function on states, and every step of the smaller
model is shown to be a step of the larger one (a simulation):

```
Core     --toRecon-->  CoreRecon at min-sync-replicas 0  --emb-->  CoreRestart with no restarts
CoreAck  --toRecon-->  CoreRecon with held replies       --emb-->  CoreRestart with held replies
```

`emb` gives every node incarnation 1, so every incarnation check holds. A Core
step is a CoreRecon step at `min-sync-replicas = 0` under F1's reconciliation
rule. The safety theorems of the smaller models are then a few lines each.

### 4.4 Lease timing

The lease proofs (`Lease/`) are independent of the replication model. They
model one lease instance in real time. The holder sends at time `s` and the
grantor receives at `r ≥ s`, with unbounded delay. Clocks are arbitrary
functions from real time to local readings, under two drift models: additive
(local elapsed time is within `D` of real elapsed time) and rate-bounded (drift
rate at most `ρ`). The main results:

- **F11.** If the holder subtracts margin `M` and the grantor does not, the
  leases are safe exactly when `M ≥ 2D`, apart from the degenerate case where
  the margin is the whole lease (`asym_safe_iff`). In the rate model the
  condition is `M ≥ 2ρ·Dur/(1+ρ)` (`rate_asym_safe`, `rate_asym_tight`). If both
  sides subtract the margin, which is one reading of "Both sides use monotonic
  clocks and subtract a clock-drift margin", the leases are unsafe for every
  margin shorter than the lease as soon as there is any drift
  (`literal_unsafe`).
- **F12.** The rule "read lease + time to commit a demotion + clock-drift margin
  < primary lease" (section 5, "Lease durations") is not sufficient. It must also
  cover the acknowledgement round trip and a second drift term. The theorem
  `demotion_in_time` gives a sufficient condition, and
  `doc_condition_insufficient_drift` and `_rtt` give runs where the doc's
  condition holds and the primary lease has already expired when the demotion
  commits. This is an availability issue, not a safety one: the primary then
  stops serving reads.

### 4.5 Checking the encoding against TLC

A proof is only as good as its model. To check that the Lean steps say the same
thing as the TLA+ actions, we export TLC's complete state graph for a small
configuration and compare it with the Lean step relation (`XCheck/`). For Core
(45,284 states, 146,532 transitions) and CoreAck (111,840 states, 410,788
transitions):

- TLC's initial state is Lean's initial state;
- every TLC transition is a Lean step with the same action;
- every Lean step from a TLC state that stays within TLC's bounds is a TLC
  transition;
- the invariant `Inv` holds on every TLC state.

All checks pass with no mismatches. A mutation test, which changes one state and
one transition label, makes them fail. The only difference is one that is
intended: the Lean ack model also allows a write that records no reply, so it has
more behaviours than CoreAck and the proof covers both.

## 5. Results

| Result | Lean | TLC (bounded) |
|---|---|---|
| Properties 1 and 2, every `min-sync-replicas`, with restarts | `RestartProof.safety` | `CoreRestart_safeMin0`, `_safeMin1` (120.6 M and 78.8 M states) |
| Properties 3 and 4 (no acknowledged write lost) | `RestartProof.ackSafety`, `ackedHeld` | same runs |
| Property 5 (two copies with `min-sync-replicas ≥ 1`) | `RestartProof.commit_two_copies` | `CoreRestart_twoCopiesMin1` |
| The same for the smaller models | `CoreProof.safety`, `AckProof.ackSafety`/`ackedHeld`, `ReconProof.safety`/`ackSafety`/`ackedHeld` | `Core`, `CoreAck`, `CoreRecon` runs |
| F1 needed | `CoreProof.noReconcile_unsafe`, `ReconProof.noReconcile_unsafe` | `CoreRecon_noReconcileMin0`/`Min1`, `CoreRestart_noReconcileMin1` |
| F2 needed | `AckProof.noReplyFence_unsafe` | `CoreAck_noReplyFence`, `CoreRestart_noReplyFenceMin1` |
| F5 needed | (TLC only) | `CoreRestart_noRebuildMin1`, `_noRebuildMin1Loss` |
| Survival condition needed | (TLC only) | `CoreRestart_noSurvivorMin0`/`Min1` |
| Incarnation check needed | (TLC only) | `CoreRestart_noIncMin1` |
| Lease margin (F11) and durations (F12) | `Lease/` | `Leases.tla` runs |
| Lean encoding = TLA+ encoding on the checked configurations | `xcheck`, `xcheck_ack` | Core and CoreAck state graphs |

[lean/LEAN.md](lean/LEAN.md) has every theorem statement, each model's
differences from its TLA+ module, and the TLC configurations.

## 6. Scope and limitations

What the proofs do **not** establish:

- **Forced failover** (`CLUSTER FAILOVER FORCE`, `TAKEOVER`) is not modelled. The
  design accepts losses when the target is ineligible (F7).
- **Reads are not covered.** The replication proofs say nothing about
  linearizable reads from the primary or from sync replicas. Leases appear only
  in the timing lemmas of `Lease/`, which cover one lease instance, not the
  protocol. Stale reads are checked by TLC only (`NoStaleLinRead` in
  `spec/Leases.tla`).
- **Correlated data loss** is excluded by the survival condition. The design's
  detection of it (no eligible replica with a matching incarnation, so the shard
  stays unavailable) is not proven.
- **Not modelled:** AOF and `appendfsync`, async read replicas beyond being
  ineligible, `target-sync-replicas` and the choice of which replica to promote
  (the model allows any promotion), replication lag thresholds, manual failover,
  slot migration, modules, and dirty key tracking.
- **The Cluster V2 bus is assumed correct.** It is modelled as one totally
  ordered configuration with compare-and-set updates, as section 6 requires.
- **Messages are abstracted** (section 1): there is no reordering within a
  connection.
- **The cross-check covers Core and CoreAck only,** for 3 nodes, logs of length
  1 and up to 5 configuration versions. The CoreRestart encoding is not
  cross-checked against its TLA+ state graph. A divergence that only shows up
  outside those bounds, or only with restarts, would not be caught.
- **Min-sync-replicas ≥ 1 with Core's stricter reconciliation rule** is covered
  by argument, not by a Lean lemma: every step under that rule is a step under
  F1's rule, which the theorem covers. At `min-sync-replicas = 0` this is a
  Lean lemma (`CoreProof.step_toRecon`).

## 7. How the proof is organized in code

### Layout

| Directory | Contents | Lines |
|---|---|---|
| `lean/RestartProof/` | The CoreRestart model and the only invariant proof | 2,344 |
| `lean/ReconProof/` | CoreRecon model, embedding into CoreRestart, corollaries, F1 counterexample | 489 |
| `lean/CoreProof/` | Core model, embedding into CoreRecon, corollary, F1 counterexample | 356 |
| `lean/AckProof/` | CoreAck model, corollaries, F2 counterexample | 171 |
| `lean/Lease/` | Lease timing (additive and rate clocks, durations) | 377 |
| `lean/XCheck/` | TLC state-graph cross-checks (`lake exe xcheck`, `lake exe xcheck_ack`) | 929 |
| `lean/Check.lean` | Prints the axioms of every main theorem | |

Lean 4.35 with Mathlib `v4.35.0`, used for list-prefix lemmas and for `Set.encard`
in `Writable`.

### The model as a state machine (`RestartProof/Model.lean`)

- **State.** `State Node` is a structure with one field per TLA+ variable:
  `log`, `inc`, `cfg`, `known`, `sync`, `acked`, `commit`, `recon`, `term`, and
  the ghost `hist`. `Cfg Node` is a shard configuration (`ver`, `prim`, `elig`,
  `inc`). `Node` is any type with decidable equality, which is how the proof
  covers any number of nodes. Sets of nodes are `Node → Bool`. Logs are
  `List Nat`, where each entry is its term.
- **Actions are functions.** Each action is a function `State Node → State Node`
  that returns the post-state using structure update (`{ s with log := … }`).
  Fields it does not mention are unchanged, which is TLA+'s `UNCHANGED`.
  Examples: `appendWrite`, `commitNext`, `takeover`, `restartEmpty`.
- **Guards live in the step relation.** `Step m rf rb : State → State → Prop` is
  an inductive type with one constructor per action. A constructor's arguments
  are the action's guard, and its result is the post-state. `m` is
  `min-sync-replicas`, and the Bool flags select the fixes: `rf` for F1 and `rb`
  for F5.
- **Executions.** `Reachable m rf rb i s` is the inductive closure of `Step` from
  `init i`, where `i` is the initial primary.
- **Predicates.** Names follow the TLA+ spec: `ThinksPrimary`, `KnowsLatest`,
  `primaryOf`, `IsAuthoritative`, `Reconciled`, `Writable`, `catchUpPoint`.
  `EligibleHoldsCommitted`, `CommittedAgree` and `Safety` are the properties.

### The invariant proof

- `RestartProof/Contracts.lean` states each action's effect in design terms (who
  thinks it is primary, who is authoritative, what moves), without the invariant.
  The preservation proofs use these contracts instead of unfolding the actions.
- `RestartProof/Inv.lean` defines `Inv` in layers that follow the design, each a
  structure with one named field per conjunct: `ConfigInv`, `ReplInv`,
  `MembershipInv`, `CommitInv`, `FailoverInv`, and the goal `ehc`. A proof of `Inv`
  is a record. `fence s p` and `Fenced s p` state the deposed-primary argument.
  `inv_init` proves the base case, and `inv_safety` derives `Safety`.
- `Actions1.lean`, `Actions2.lean`, `Actions3.lean` and `Restart.lean` prove one
  preservation lemma per action, such as `inv_commit` and `inv_takeover`.
  `Lists.lean` has the list-prefix lemmas they share.
- `Safety.lean` combines them. `inv_step` dispatches on the `Step` constructor,
  `inv_reachable` is the induction over `Reachable`, and `safety` is the result.

### Held replies (`*/AckModel.lean`, `*/AckProof.lean`)

`AState` wraps a `State` with the list of held replies, each a primary, an
offset and a term, and the list of writes released as OK. `AStep` allows every
replication step (`AStep.core`) plus `write`, `release` (OK), `abort`
(`UNCOMMITTED`), and the two restarts, which drop the restarted node's held
replies. The F2 fence is the `fenced` guard on `release`, switched by
the `nf` flag. The invariant `AInv` extends `Inv` with the reply conjuncts.

### Embeddings (`CoreProof/Embed.lean`, `ReconProof/Embed.lean`)

`toRecon` and `emb` map states. `step_toRecon` and `step_emb` show that steps map
to steps, `reach_toRecon` and `reach_emb` lift that to reachability, and
`safety_of_emb` transports the properties back. Each smaller model keeps its own
`Model.lean`, so each still states its own TLA+ module and its own
counterexample.

### Counterexamples

A counterexample is a concrete list of steps on `Fin 3`. A theorem states that
the run is reachable with a fix turned off and that the final state violates the
property. Lean's `decide` evaluates every guard.

### Cross-checks (`XCheck/`)

`xcheck/dot2lean.py` turns TLC's dot dump into Lean data (`XCheck/data*`,
generated and not committed). The checker decodes each TLC state, renders it
back to TLC's text format to confirm the decoding, and evaluates Boolean
versions of `Step` and `Inv` (`stepB`, `invB`). Soundness lemmas such as
`stepB_sound` and `invB_sound` prove that a `true` answer implies the Prop
definition, and `XCheck/Axioms.lean` prints their axioms.

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
7 GB). A clean build then takes about 30 seconds. The cross-checks (`lake exe xcheck`,
`lake exe xcheck_ack`) need TLC state graphs generated from the spec branch.
[lean/LEAN.md](lean/LEAN.md) gives the commands.

## References

- Valkey Durability: Synchronous Replication Architecture,
  [`design-docs/sync-replication.md`](../sync-replication.md)
  ([valkey#5286](https://github.com/valkey-io/valkey/pull/5286)).
- Findings F1 to F12 and the TLA+ models:
  [`sync-repl-formal-spec`](https://github.com/jjuleslasarte/valkey/tree/sync-repl-formal-spec)
  branch, `FINDINGS.md` and `spec/`.
- W. Lin, M. Yang, L. Zhang, L. Zhou. PacificA: Replication in Log-Based
  Distributed Storage Systems. Microsoft Research, MSR-TR-2008-25, 2008.
