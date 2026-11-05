# Lean 4 proof of Core safety (iteration 3B)

## Proof structure

There is one inductive invariant proof, for CoreRestart (`RestartProof/Inv.lean`,
`Actions1-3.lean`, `Restart.lean`, `AckProof.lean`). The Core, CoreAck and CoreRecon
results are corollaries, obtained by embedding each smaller model into the next:

```
Core  --toRecon-->  CoreRecon at MinSync 0  --emb-->  CoreRestart with no restart
CoreAck --atoRecon--> CoreRecon + replies   --aemb--> CoreRestart + replies
```

- `CoreProof/Embed.lean`: `toRecon` copies every field. `step_toRecon` shows that every
  Core step with ReconcileFirst = TRUE and EarlyDrop = FALSE is a CoreRecon step at
  MinSync 0 under the A1 rule. Writable at MinSync 0 always holds, and under Reconciled
  CatchUpPoint is the commit, so Core's strict Count and Resync are A1 steps.
- `ReconProof/Embed.lean`: `emb` sets every incarnation, and every incarnation a config
  records, to 1, as in CoreRestart's `Init`. Every incarnation check then holds, so every
  CoreRecon step is a CoreRestart step (`step_emb`), with either RebuildSync value.
  `auth` becomes "eligible or primary", so CoreRestart's Safety and AckedHeld of the
  image are CoreRecon's (`safety_of_emb`).
- `reach_toRecon`, `reach_emb`, `areach_toRecon`, `areach_emb` lift this to
  reachability. `CoreProof.safety`, `AckProof.ackSafety`/`ackedHeld` and
  `ReconProof.safety`/`ackSafety`/`ackedHeld` are each a few lines on top of
  `RestartProof.safety`/`ackSafety`/`ackedHeld`.

The models (`*/Model.lean`, `*/AckModel.lean`) and the counterexamples are unchanged;
each still states its own TLA+ module. Every theorem statement is unchanged; this was
checked by printing all 13 theorems and the definitions they mention at commit 24680af
and after the change, with identical output. The axioms are unchanged too.

Until commit 24680af, CoreProof, ReconProof and RestartProof were three separate proofs,
each a copy of the previous one with changes. The Core-only invariant described in "The
invariant" and "Proof status" below is the one that standalone Core proof used.
RestartProof's invariant generalizes it (see "Invariant changes" in the ReconProof and
RestartProof sections). The old files are in git history.

## Result

`CoreProof.safety` (`CoreProof/Safety.lean`):

```lean
theorem safety {Node : Type} [DecidableEq Node] {i : Node} {s : State Node}
    (h : Reachable true false i s) : Safety s
```

This holds for any node type with decidable equality, so any number of nodes (even
infinitely many). It also holds for any log length and any number of configuration
changes. Every state reachable from `Init` under `Next` with `ReconcileFirst = TRUE` and
`EarlyDrop = FALSE` satisfies `EligibleHoldsCommitted /\ CommittedAgree`.

`#print axioms` (from `Check.lean`):

```
'CoreProof.safety' depends on axioms: [propext, Classical.choice, Quot.sound]
'CoreProof.noReconcile_unsafe' depends on axioms: [propext, Quot.sound]
```

These are Lean's standard axioms. There is no `sorryAx`, and the sorry count is 0.
`Classical.choice` comes from `by_cases` on propositions over an abstract node type.

`CoreProof.noReconcile_unsafe` (`CoreProof/Counterexample.lean`) covers
`ReconcileFirst = FALSE`. An explicit 11-step run on `Fin 3` reaches a state that
violates `EligibleHoldsCommitted`. This is the F1 scenario:

1. n0 commits a write with n1 as its only sync replica.
2. n1 takes over with commit 0.
3. n1 counts and promotes n2, whose log is empty.

`decide` discharges every guard.

## Reproduce

```
cd lean
../tools/lean.sh lake build                 # about 6 s from clean
../tools/lean.sh lake env lean Check.lean   # prints the axioms
```

This uses Lean 4.35.0 core only: no Mathlib, no Batteries, and no packages in
`lake-manifest.json`.

## Encoding (`CoreProof/Model.lean`)

| Core.tla | Lean |
|---|---|
| `log : [Nodes -> Seq(Nat)]` | `log : Node → List Nat` (0-indexed: TLA `log[p][k+1]` is `getD k`) |
| `cfg` record | `Cfg := {ver : Nat, prim : Node, elig : Node → Bool}` |
| `known`, `commit`, `recon`, `term` | `Node → Cfg`, `Node → Nat` |
| `sync : [Nodes -> SUBSET Nodes]` | `Node → Node → Bool` |
| `acked` | `Node → Node → Nat` |
| `hist` | `List Nat` |
| `IsPrefix(s, t)` | `s <+: t` (`List.IsPrefix`) |
| `Prefix(s, k)` | `s.take k` (`take` already clamps to `min(k, Len)`) |
| `[f EXCEPT ![a] = b]` | `upd f a b` |

Each action's post-state is an explicit function (`appendWrite s p`, ...). `Step rf ed s s'`
has one constructor per action, and each constructor carries that action's guards.
The table below lists, action by action, where the encoding differs from the TLA text.

| Action | Correspondence |
|---|---|
| AppendWrite | `Len(log[p]) < MaxLen` dropped (it only bounds TLC) |
| Replicate, Ack, Count, Learn, Resync | literal |
| Commit | `commit[p] < CommitCap(p)` is encoded as `∀ r, sync[p][r] → commit[p] < acked[p][r]`, next to `commit[p] < Len(log[p])`. This is equivalent to the TLA guard for both an empty and a non-empty sync set, and needs no `CHOOSE`/min. The `hist` update is literal. |
| MakeEligible, MakeIneligible, Takeover | `CanCas` (`cfg.ver < MaxVer`) dropped |
| EarlyUncount | present and guarded by `ed = true`. With `ed = false`, as in the theorem, it is impossible. |
| Takeover | `newElig = elig \ {prim, n}` as a Bool function |

The only deviation is the removal of `MaxLen`/`MaxVer`. `Init` is `init i`.

## The invariant (standalone Core proof, until 24680af)

This section and the next describe the standalone Core proof, `CoreProof/Inv.lean` and
`CoreProof/Actions1-3.lean` at commit 24680af. `RestartProof/Inv.lean` keeps these
conjuncts with the changes listed under "Invariant changes" for ReconProof and
RestartProof, and `RestartProof/Actions1-3.lean` has the same lemma names.

The invariant has 17 conjuncts, and the `stale` conjunct uses a witness predicate `Wit`
with 6 parts.

Notation:
- `tp p` means `known[p].prim = p`, that is, p thinks it is primary.
- `prim` means `cfg.prim`.

1. `ehc`: EligibleHoldsCommitted.
2. `caLen`, `caHist`, `caEq`: CommittedAgree, with `commit[p] ≤ Len(hist)` stated explicitly.
3. `knownLe`, `knownEq`, `primKnows`: every config a node knows is a past config.
   - A known version is at most `cfg.ver`.
   - A known version equal to `cfg.ver` means the known config equals `cfg`.
   - The primary knows the current config.
4. `eligSync` (O1): a primary counts every replica that is eligible in the config it
   knows. `noSelfSync`: no primary counts itself.
5. `ackedPrim`: the current primary has ack 0 from every node that does not follow it.
6. `ackedLe`: a primary's acks are at most its log length.
7. `ackHolds`: a follower `n` of a primary `p` holds `take acked[p][n] log[p]`.
8. `syncNew`: a counted replica that is not yet eligible follows the current primary, and
   that primary is reconciled. **`ReconcileFirst` is used here and nowhere else.**
9. `syncCommit`: a primary's commit is at most the ack of each of its sync members.
10. `eligFol`: an eligible replica that follows a stale primary `q` is eligible in
    `known[q]`. This is why Resync never truncates an eligible replica.
11. `histLe`: `Len(hist) ≤ commit[prim]`, or `Len(hist) ≤ recon[prim]`.
12. `stale`: every stale primary `p` (`tp p`, `known[p].ver < cfg.ver`) has a witness `w`
    satisfying `Wit s p w`:
    - `w` is in `sync[p]`.
    - `w` no longer follows `p`, so `acked[p][w]` is frozen.
    - `acked[p][w]` is the least ack among the frozen members.
    - `acked[p][w] ≤ recon[prim]`.
    - `take acked[p][w] log[p]` is a prefix of `log[prim]`.
    - Every eligible replica either holds that prefix, or is a sync member that still
      follows `p`.

The key lemma is `commit_holds`. If any primary, current or stale, may commit offset `c`,
then the current primary and every eligible replica hold `take (c+1) log[p]`.

The O2 argument is in `stale`. A stale primary cannot shrink its sync set, because the
ineligible CAS needs the latest config. So the taker bounds the stale primary's commits
forever.

The O1/F1 argument is in `syncNew`, `histLe`, and the MakeEligible case of `stale`.
Promotion waits until the primary's commit passes its reconciliation point. That point
covers every frozen witness prefix and the whole history.

## Proof status

Every action's preservation lemma is proven, and the proof contains no `sorry`.

| Action | Lemma | Conjuncts that need real work |
|---|---|---|
| AppendWrite, Replicate | `inv_grow` (logs only extend) | `caEq`, `ackHolds`, `stale` (`wit_mono`) |
| Ack | `inv_ack` | `ackedPrim`, `ackedLe`, `ackHolds`, `syncCommit` |
| Commit | `inv_commit` | `ehc`, `caHist`, `caEq`, `syncNew`, `syncCommit`, `histLe` |
| Count | `inv_count` | `eligSync`, `noSelfSync`, `syncNew`, `syncCommit`, `stale` |
| MakeEligible | `inv_makeElig` | `ehc` (needs reconciliation), `known*`, `eligSync`, `syncNew`, `eligFol`, `stale` |
| MakeIneligible | `inv_makeInelig` | `known*`, `eligSync`, `syncNew`, `eligFol`, `stale` |
| Learn | `inv_learn` | `ackHolds`, `syncNew`, `eligFol`, `stale` (the witness may switch to the learner) |
| Takeover | `inv_takeover` | nearly all; `stale` builds a fresh witness for the deposed primary |
| Resync | `inv_resync` | `ca*`, `ackHolds`, `syncCommit`, `stale` |
| EarlyUncount | impossible (`ed = false`) | |

## Observations about the model

These were read off the standalone Core proof.

- **The proof never uses terms.** It never inspects log values. Prefix and ack
  bookkeeping are enough, with no Raft-style log matching. Terms only let TLC tell a
  deposed primary's entries apart.
- **Only Count needs `Reconciled`.** For these two properties, the proof never uses the
  `Reconciled` guard on AppendWrite or on Resync. Those guards may matter for properties
  outside Core, such as client-visible durability. This is a reading of the proof, not a
  recommendation to remove them.
- **The proof never uses these guards either:**
  - Resync's `~IsPrefix` and `r ∉ sync[p]`. Only `r ∉ known[p].elig` matters, through `eligFol`.
  - Count's `r ∉ sync[p]`.
  - Replicate's prefix guard. The proof only uses the fact that logs grow.

  Ack's prefix guard is essential.
- **Deposed primaries do commit.** A deposed primary can keep committing up to its
  taker's frozen ack. This is safe only because of `stale`.
- The proof found no protocol bug and no Core.tla modelling issue. The counterexample for
  `ReconcileFirst = FALSE` reproduces F1 as expected.

## Metrics (for the TLA+ vs Lean comparison)

These metrics are for the standalone Core proof as first written. The dedup is
summarized in "Size after the dedup" at the end of this file.

| Metric | Value |
|---|---|
| Wall time (reading Core.tla to full proof) | about 20 min (16:57 to 17:16 UTC) |
| Invariant conjuncts | 17, plus the 6-part `Wit` |
| Lean lines | 1421 total: model 154, invariant and derived lemmas 137, list lemmas 66, preservation 962, top theorem 34, counterexample 50 |
| `sorry` at the end | 0 |
| Clean build | about 6 s, no dependencies |
| Compile/fix iterations | about 10 |
| Invariant revisions forced by Lean | 0 |

**How the invariant was found.** All compile errors were at the tactic level:

- a lemma used in the wrong direction,
- `omega` atoms hidden behind `{s with ..}` projections,
- deprecated names.

None of them required a change to the invariant. The invariant was derived by hand,
action by action, before any Lean was written. The failed proof goals were never used as
counterexamples to induction. A weaker first guess would have needed that loop, which is
the expensive mode because Lean gives no traces.

**What was hard:**

- **The stale-primary argument.** It needs:
  - an existential witness,
  - a "least frozen ack" condition, because the witness switches when a stale follower
    learns,
  - a disjunctive cover,
  - a fresh witness at takeover.
- **List prefix algebra.** `take` and prefix reasoning needed small lemmas. `grind` and
  `omega` did most of the arithmetic.
- **Function updates.** Each `upd` needs a case split, and there are 10 to 17 conjuncts
  per action.
- **Projections of `{s with ...}`.** They are definitionally equal to the original fields
  but differ syntactically, so `rw` and `omega` need `show` or `change`.

**What Lean gives that TLC and Apalache do not:**

- An unbounded result: any number of nodes, any log length, any number of versions.
  TLC checked 3 nodes, MaxLen 2 and MaxVer 6.
- A checked explanation of why the protocol is safe.
- A guard-usage audit for free (see Observations).

**What TLC gives that Lean does not:**

- Counterexample traces. In Lean, a wrong invariant shows up only as an unprovable goal,
  and you must reconstruct the bad state yourself. The F1 run had to be written by hand
  (TLC had found it earlier).
- Zero proof effort for new configurations and bug switches.
- Reachability and liveness checks. In Lean, non-vacuity is shown only by explicit runs.

## Cross-check against TLC (`XCheck/`)

The Lean encoding was compared mechanically with `spec/Core.tla` on TLC's complete state
graph of `spec/Core_xcheck.cfg`: 3 nodes, `InitPrim = n1`, `MaxLen = 1`, `MaxVer = 5`,
`ReconcileFirst = TRUE`, `EarlyDrop = FALSE`. The graph has 45,284 states and 146,532
labelled transitions.

### Pieces

- `xcheck/dot2lean.py` parses TLC's dot dump (`xcheck/core_xcheck.dot`) into
  `lean/XCheck/data/` (`states.txt`, `labels.txt`, `edges.txt`, `init.txt`). These files
  are generated and git-ignored.
- `XCheck/Data.lean` decodes each line into a `CoreProof.State (Fin 3)` (n1, n2, n3 = 0,
  1, 2). `render` prints a decoded state back in TLC syntax. The checker compares this
  output with TLC's own state text, whitespace removed, for every state. The decoder is
  therefore tested against TLC's output and does not have to trust the Python parser.
- `XCheck/Basic.lean` defines:
  - `succs s`: for each `Step` constructor, node argument and node pair, the guard is
    checked with `decide`. Each enabled instance yields its label and its post-state,
    computed with Model.lean's own post-state functions.
  - `stepB s s'`: true if `s'` is one of `succs s`.
  - `stepLB s l s'`: the same, but the label must also be `l`.
  - `invB s`: `decide` of `InvP (lift s)`, which has the 21 fields of `RestartProof.Inv`.
    `lift` is `ReconProof.emb ∘ CoreProof.toRecon`, so this checks the invariant the
    proof of `CoreProof.safety` actually goes through. Until 24680af it checked the
    standalone Core proof's 17-field `Inv`; that check also passed on all 45,284 states.
  - `safetyB`: `decide` of `Safety`.
  - `inBounds`: every log and `hist` have length ≤ 1, and `cfg.ver ≤ 5`.
  - Decidable equality for states over `Fin 3`, by comparing functions at 0, 1 and 2.
- Soundness is proven, using only the axioms `propext` and `Quot.sound` (see
  `XCheck/Axioms.lean`):
  - `stepB_sound : stepB s s' = true → Step true false s s'`, and the same for `stepLB`.
    Each partial successor `oX` is an `if guard then some (x s p) else none`, where the
    guard is exactly that constructor's premises. Lemmas `gX` prove that every `some t`
    is a `Step`, and `allActs_good` covers every list entry.
  - `invB_sound : invB s = true → RestartProof.Inv (lift s)`.
  - `safetyB_sound : safetyB s = true → Safety s`.
- `XCheck/Main.lean` (`lake exe xcheck`) runs the checks.

### Reproduce

The dot dump and `lean/XCheck/data/` are generated and git-ignored. To regenerate both
(TLC about 5 s, with a metadir under scratch space that is deleted afterwards):

```
cd spec
java -XX:+UseParallelGC -cp ../tools/tla2tools.jar tlc2.TLC -config Core_xcheck.cfg \
     -workers 8 -deadlock -metadir "$MD" -dump dot,actionlabels ../xcheck/core_xcheck \
     Core.tla > ../xcheck/tlc.out 2>&1
find "$MD" -delete
cd ..
python3 xcheck/dot2lean.py               # about 11 s; writes lean/XCheck/data/
cd lean
../tools/lean.sh lake build xcheck XCheck
../tools/lean.sh lake exe xcheck         # about 6 s
../tools/lean.sh lake env lean XCheck/Axioms.lean
```

`lake build XCheck` is needed for `Axioms.lean`, which imports the `XCheck` root module;
the `xcheck` executable alone does not build it.

### Result (2026-11-04, rerun 2026-11-05 after the dedup): no mismatches

| Check | Result |
|---|---|
| Decoding: `render (decode line) = TLC text` | 45,284 / 45,284 |
| (a) every TLC edge `s → s'` has `stepB s s'` | 146,532 / 146,532 |
| (a') with the same action label (`stepLB`) | 146,532 / 146,532 |
| (b) every in-bounds Lean successor of every TLC state is a TLC state, joined by a TLC edge with the same label | 146,532 / 146,532. The in-bounds labelled Lean successors and the distinct labelled TLC edges are the same set. |
| (b) Lean successors outside the bounds | 68,158. All of them violate a dropped bound: AppendWrite with a log longer than MaxLen (42,738), MakeEligible (18,744), MakeIneligible (5,260) and Takeover (1,416) with `ver > MaxVer`. |
| (c) TLC's initial state = `init n1` | yes |
| (d) `invB` (`RestartProof.Inv` of the lifted state) and `safetyB` on every TLC state | 45,284 / 45,284 each |

A mutation test changed one state's `term` and one edge's label in a copy of the data.
The checker reported decode, (a), (a') and (b) failures as expected.

### What this establishes

On this configuration, the two encodings define the same transition system once the
TLC bounds are applied. The two systems have:

- the same initial state,
- the same reachable states,
- the same transitions, with the same action and arguments.

So every behaviour TLC explored is a `Reachable true false 0` run in Lean. `CoreProof.safety`
covers exactly these behaviours and extends them to unbounded logs, versions and node
sets. The check also confirms that `RestartProof.Inv` holds on the image of every
reachable state here, so the invariant is not stronger than reachability at this size.

This catches encoding slips that the proof could not catch, such as a wrong guard, a
wrong `EXCEPT`, or the CommitCap rewrite, as long as the slip shows up within the bounds.

### What it does not establish

- **Behaviour outside the bounds.** Logs longer than 1 and versions above 5 were not
  compared. A Lean/TLA divergence that only shows up with two log entries would go
  unnoticed. One example is the `getD` index in Replicate when a log is non-empty and
  being extended. `MaxLen = 1` exercises appending to an empty log only.
- **Other configurations.** The check does not cover other node counts or
  `InitPrim` values. It does not cover `ReconcileFirst = FALSE` or `EarlyDrop = TRUE`
  either: `stepB` fixes `rf = true` and `ed = false`.
- **Exact fidelity to TLC's semantics.** The decoding and `render` are tested against
  TLC's printed text, not against TLC's internal values. The parser and printer were
  written for this dump format only.
- **TLC's own completeness.** The check relies on TLC's fingerprint-based exploration,
  which has a reported collision probability of about 1e-10. Any Lean successor TLC
  missed would show up as a (b) failure.

## Cross-check of CoreAck against TLC (`XCheck/Ack.lean`, `XCheck/AckMain.lean`)

The same comparison for `spec/CoreAck.tla` and `AckProof.AStep true false false`, on TLC's
complete state graph of `spec/CoreAck_xcheck.cfg`: the Core bounds above (3 nodes,
`InitPrim = n1`, `MaxLen = 1`, `MaxVer = 5`, `ReconcileFirst = TRUE`, `EarlyDrop = FALSE`)
plus `NoReplyFence = FALSE`. The graph has 111,840 states and 410,788 labelled
transitions; TLC checks all of CoreAck's invariants on it.

### Pieces

- `xcheck/dot2lean.py --ack` reads `xcheck/coreack_xcheck.dot` and writes
  `lean/XCheck/data_ack/`. The state encoding is Core's followed by `held` (count, then
  node, offset, tag per reply) and `okd` (count, then offset, value per pair).
  TLC's variable order is not fixed in this dump, so `labels.txt` has the
  `/\ var = value` conjuncts sorted by variable name; this sort is the only transformation
  of TLC's text that the script performs.
- `XCheck/Ack.lean`:
  - `held` and `okd` are lists in Lean and sets in TLA+. `cn` sorts both (lexicographic on
    `(node, offset, tag)` and `(offset, value)`) and removes duplicates. States are
    compared after `cn`.
  - `asuccs a`: every enabled `AStep` instance with a TLC-style label. `AStep.core` steps
    are labelled `C<Action>(...)` as CoreAck.tla's wrappers are, `AStep.write` is
    `Write(p)`, and `AStep.release` / `AStep.abort` are `ReleaseSome` / `AbortSome`
    (TLC does not print the bound `h`; distinct replies give distinct post-states). A core
    `AppendWrite` keeps the label `AppendWrite(p)`: it is an append with no held reply,
    which CoreAck.tla does not have.
  - `asuccs_sound : (l, t) ∈ asuccs a → AStep true false false a t`, and
    `astepLB_sound : astepLB a l t = true → ∃ u, AStep true false false a u ∧ cn u = t`.
  - `ainvB_sound : ainvB a = true → RestartProof.AInv (alift a)` (`Inv`, `TermLe`,
    `HeldInv`, `AckedDurable` of CoreRestart, on `alift a = ReconProof.aemb (atoRecon a)`),
    and `ackedHeldB_sound` for `AckedHeld`. Until 24680af `ainvB` checked the standalone
    `AckProof.AInv`; that check also passed on all 111,840 states.
  - `decodeA`, `encodeA`, and `renderA`, which prints a state in TLC syntax with the
    variables in alphabetical order and records as TLC prints them in this dump.
  - Axioms (`XCheck/Axioms.lean`): `propext`, `Quot.sound`, and for `asuccs_sound` /
    `astepLB_sound` also `Classical.choice`. No `sorry`.
- `XCheck/AckMain.lean` (`lake exe xcheck_ack`) runs the checks.

### Reproduce

```
cd spec
java -XX:+UseParallelGC -cp ../tools/tla2tools.jar tlc2.TLC -config CoreAck_xcheck.cfg \
     -workers 8 -deadlock -metadir "$MD" -dump dot,actionlabels ../xcheck/coreack_xcheck \
     CoreAck.tla > ../xcheck/tlc_ack.out 2>&1      # about 11 s, 198 MB dot file
find "$MD" -delete
cd ..
python3 xcheck/dot2lean.py --ack         # about 33 s; writes lean/XCheck/data_ack/
cd lean
../tools/lean.sh lake build xcheck_ack XCheck
../tools/lean.sh lake exe xcheck_ack     # about 14 s
../tools/lean.sh lake env lean XCheck/Axioms.lean
```

`xcheck/coreack_xcheck.dot`, `xcheck/tlc_ack.out` and `lean/XCheck/data_ack/` are
git-ignored.

### Result (2026-11-04, rerun 2026-11-05 after the dedup): no mismatches; Lean is a strict superset by one action

| Check | Result |
|---|---|
| Decoding: `renderA (decodeA line) = TLC text` | 111,840 / 111,840 |
| Every decoded state is canonical (`cn a = a`): TLC's set order is `cn`'s order | 111,840 / 111,840 |
| (a) every TLC edge `s → s'` has an `AStep` with the same label to a state with `cn = s'` | 410,788 / 410,788 |
| (b) every in-bounds labelled Lean successor, other than a reply-less `AppendWrite`, of every TLC state is a TLC state joined by a TLC edge with the same label | 410,788 / 410,788. The two edge sets are equal. |
| (b) Lean successors outside the bounds | 161,904: Write with a log longer than MaxLen (103,042), CMakeEligible (46,278), CMakeIneligible (9,776) and CTakeover (2,808) with `ver > MaxVer`. |
| Lean successors whose `held` or `okd` list had a duplicate before `cn` | 0 / 0. On these states list and set semantics agree (`erase` removes the only copy). |
| (b') expected superset: `AStep.core` with `AppendWrite`, an append with no held reply | 13,246 in bounds, one per TLC `Write` edge; none of their post-states is a TLC state. 103,042 more are outside the bounds. |
| (c) TLC's initial state = `cn (ainit n1)` | yes |
| (d) `ainvB` (`RestartProof.AInv` of the lifted state), `ackedHeldB`, `safetyB` on every TLC state | 111,840 / 111,840 each |

Direction (b') is the only difference, and it is the one `AckProof/Model.lean` documents:
`AStep.core` admits Core's `AppendWrite` with `held` and `okd` unchanged. Example, from the
initial state, `AppendWrite(n1)` gives `log[n1] = <<1>>` with `held = {}`; in TLC, n1's
only append is `Write(n1)`, which also adds `[n |-> n1, o |-> 1, t |-> 1]` to `held`.
Lean's model therefore has more behaviours than CoreAck.tla, and `ackSafety` covers it.

A mutation test changed one state's held tag, and separately relabelled one
`ReleaseSome` edge as `AbortSome`, in copies of the data. The checker reported decode,
(a), (b) and (d) failures for the first, and (a) and (b) failures for the second.

What this does and does not establish is as for Core above, with one more limit: with
`MaxLen = 1` every reply has offset 1, so `held` holds at most two replies and `okd` at
most one pair, and the sort order of larger sets is not exercised.

# Lease timing proofs (F11)

Files: `Lease/Additive.lean`, `Lease/Rate.lean`, `Lease/Durations.lean`, root
`Lease.lean`, axioms in `LeaseCheck.lean`. The lakefile has a second default target,
`lean_lib Lease`. Lean core only, `omega` plus a few `Int.mul_*` lemmas, about 390 lines, no
`sorry`. Wall time about 10 minutes (18:30 to 18:40 UTC).

```
cd lean
../tools/lean.sh lake build
../tools/lean.sh lake env lean LeaseCheck.lean
```

## Setting

One lease instance. The holder H sends at real time `s`. The grantor G receives at
`r ≥ s`. The delay `r - s` is **unbounded** in every safety theorem. A clock is any
`c : Int → Int` from real time to local reading.

* `Valid cH s W t`: `s ≤ t` and `cH t - cH s < W`, so H believes its lease valid.
* `Over cG r G t`: `r ≤ t` and `cG t - cG r ≥ G`, so G's promise is over.
* The asymmetric rule (the model's) is `W = Dur - M`, `G = Dur`.
* The literal reading ("both sides subtract a margin") is `W = G = Dur - M`.

There are two clock models.

* **Additive** (`AddDrift c a D`, the Leases.tla model). For every `t ≥ a`, local elapsed
  since `a` is within `D` of real elapsed. Every type is `Int`.
* **Rate** (`RateClock a b c`, drift rate `ρ = a/b`, `0 ≤ a < b`). Lean core has no
  ordered-field lemmas for `Rat`, so the model is **scaled to integers**. Clocks read in
  units of `1/b` real tick, and for all `t1 ≤ t2`:
  `(b-a)(t2-t1) ≤ c t2 - c t1 ≤ (b+a)(t2-t1)`.
  A duration `X` corresponds to `b*X` local units. Such clocks are monotone.

## Theorems

| Theorem | Statement |
|---|---|
| `asym_safe` | `M ≥ 2D` ⇒ no real time where H is valid and G is over. Holds for all `s ≤ r`, all additive clocks (monotone or not), and all `Dur, D, M`. |
| `asym_tight` | `0 ≤ D`, `M < 2D`, `M < Dur` ⇒ overlap with zero delay. H's clock is `D` slow and G's clock is `D` fast, both monotone, at real time `max D (Dur - D)`. |
| `asym_safe_iff` | For `D ≥ 0`: every instance is safe ⇔ `2D ≤ M ∨ Dur ≤ M`. The second disjunct says H's lease is never valid. |
| `literal_unsafe` | F11: `D > 0`, `M < Dur` ⇒ overlap, **for every margin M**. |
| `literal_safe_iff` | For `D ≥ 0`: the literal reading is safe ⇔ `D = 0 ∨ Dur ≤ M`. The margin size never matters. |
| `rate_asym_safe` | `2a·Dur ≤ (b+a)·M`, that is `M ≥ 2ρ·Dur/(1+ρ)`, ⇒ no overlap, for all `s ≤ r` and all rate clocks. |
| `rate_asym_tight` | `(b+a)M < 2a·Dur` ⇒ overlap. H runs at `1-ρ` and G at `1+ρ`, with zero delay. |
| `rate_literal_unsafe` / `rate_literal_safe_nodrift` | The literal reading is unsafe for every `a > 0` when `M < Dur`, and safe when `a = 0`. |
| `demotion_in_time` | Doc "Lease durations", with bus commit delay `≤ B` and round trip `≤ Δ`. See below. |
| `doc_condition_insufficient_drift` / `_rtt` | Concrete runs where the doc's condition holds and P's lease has already expired at commit. |

**The rate tightness theorems are tight up to a tick refinement.** With integer real
time, the overlap window `bW/(b-a) - bG/(b+a)` can be shorter than one tick. The
witness therefore scales every duration by `k = (b-a)(b+a)`. This is the same physical
scenario measured with a finer tick.

The rate-form condition depends on `Dur`. The additive `D` is the worst drift over one
lease, and `D ≈ ρ·Dur` recovers `M ≥ 2D` to first order.

`demotion_in_time` (additive model) uses this timeline:

1. P sends offset X at `sp`. This starts P's primary lease from R.
2. R ACKs at `sa`. This starts R's read lease.
3. P receives the ACK at `ra ≤ sp + Δ`.
4. P's wait ends at `w`, the earliest time with local elapsed since `ra` `≥ ReadDur`.
5. The demotion commits at `d ≤ w + B`.

If `ReadDur + B + Δ + M + 2D < PrimDur` and `M ≥ 2D`, then at `d` P's primary lease from
R is still valid and R's read lease is invalid.

The doc's condition, read lease + commit + margin < primary lease, is **not sufficient**
on its own:

* It omits the round trip `Δ`. With no drift, `ReadDur=2, B=1, PrimDur=4, Δ=2` already
  fails.
* In the additive model, P's clock can run `D` slow while P waits and then `D` fast.
  This costs `2D` beyond the margin. With `D=1, M=2, ReadDur=2, B=1, PrimDur=6`, the
  lease expires at commit.

This is an availability property only: P then stops serving reads, which is safe. It
matches the "Model limits" note that the commit delay is not modelled in Leases.tla,
whose `ASSUME ReadDur + Margin < PrimDur` is the `B = Δ = 0` case without the `2D`.

`#print axioms`: every theorem depends only on `[propext, Quot.sound]`, plus
`Classical.choice` for the `_iff` theorems, `rate_asym_tight`, `rate_literal_*`, and
`doc_condition_*` (from `by_cases` and `decide`/`simp` on `Int` propositions). There is
no `sorryAx`.

## Correspondence to Leases.tla

TLC encodes the following:

* `HolderValid(start, dur) == now - start < dur - Margin + Drift`
* `GrantorOver(recv, dur) == now - recv >= dur - Drift`

In the additive model, `Valid` implies `t - s < Dur - M + D`, because local ≥ real − D.
`Over` implies `t - r ≥ Dur - D`, because local ≤ real + D. So each TLA predicate is the
weakest real-time consequence of the Lean one.

The extremes are attained by the clocks used in the Lean witnesses:

* With `slowClock D`, `Valid` ⇔ `t - s < Dur - M + D` once `t - s ≥ D`.
* With `fastClock D`, `Over` ⇔ `t - r ≥ Dur - D` once `t - r ≥ D`.

TLC therefore checks the extreme case of the additive model: holder exactly `D` slow
and grantor exactly `D` fast, at the same time. In TLC terms the safety condition is
`M ≥ 2D`, the same as `asym_safe_iff`.

The extreme case differs in one place. For elapsed times `< D`, `HolderValid` accepts
some states that no monotone clock can produce. A holder that is `D` slow after `e < D`
ticks would need a negative local elapsed. One example: `Dur ≤ M < Dur + D` is valid in
TLC at `now = start`, but is never valid in Lean with a monotone clock. This is
conservative for safety, since TLC allows more validity than any real clock does. It
could only add spurious violations, never hide one.

## Surprises

* The literal reading's failure does not depend on the margin, and its exact safety
  condition is `D = 0`.
* The doc's duration rule needs the round trip and an extra `2D` term, which the doc's
  rule omits.

# Acknowledged writes are never lost (`AckProof/`, CoreAck.tla)

## Theorem

`AckProof.ackSafety` (`AckProof/Proof.lean`):

```lean
theorem ackSafety {Node : Type} [DecidableEq Node] {i : Node} {a : AState Node}
    (h : AReachable true false false i a) : AckedDurable a ∧ Safety a.core
```

and the corollary `AckProof.ackedHeld : AReachable true false false i a → AckedHeld a`.
For any node type, log length and number of configuration changes:
- `AckedDurable`: every acknowledged `(o, v)` has `1 ≤ o ≤ |hist|` and `hist[o] = v`.
  `hist` only grows, so an acknowledged write stays in the committed history.
- `Safety` is Core's `EligibleHoldsCommitted ∧ CommittedAgree`.
- `AckedHeld`: the current primary and every eligible replica hold every acknowledged
  write at its offset.

The three flags are ReconcileFirst = TRUE (F1 fix), EarlyDrop = FALSE, and
NoReplyFence = FALSE (F2 fix: release OK only while `ThinksPrim(p) ∧ term[p] = tag`).

## Assumptions and scope

- **F1 fix and F2 fix:** both needed. Each must-fail switch has a TLC counterexample,
  and NoReplyFence also has a Lean one (below).
- **F8 fix not needed here.** There is no lease and no `cfg.prim = p` guard on Commit or
  Release. A deposed primary that still believes it is primary may commit and release OK
  (TLC: `CoreAck_ReachDeposedCommit`, `CoreAck_ReachDeposedAck`). It is still safe
  because Core's `CommittedAgree` covers deposed primaries: a deposed primary commits only
  up to the frozen ack of a sync replica that left it, and the new primary holds that
  prefix (`Wit` in `RestartProof/Inv.lean`). The F8 loss trace (`Shard_f_noFence`) relies on
  forced failover, which is out of scope here.
- **Out of scope:** forced failover (F7, an operator action whose losses the doc
  accepts), and lossy or correlated events (crash with log loss, restart with empty
  state).
- **min-sync-replicas = 0** (Core's setting, the weakest case). A primary with an empty
  sync set commits alone. That is safe because no action destroys the log of the primary
  or of an eligible replica: `Resync` truncates only an ineligible, uncounted replica.
  min-sync > 0 guards against losing the only copy, which needs a lossy event.
- The Lean model over-approximates CoreAck.tla: `AStep.core` allows any Core step,
  including an append that records no reply. CoreAck.tla's `Write` always records one.
  Bounds (`MaxLen`, `MaxVer`) are dropped.

## Invariant (`AInv`)

`AckProof.ackSafety` is now derived through `ReconProof.ackSafety` from
`RestartProof.ackSafety`, whose invariant `RestartProof.AInv` is CoreRestart's `Inv` plus
the three conjuncts below (with `fenced` reading CoreRestart's ThinksPrim). Until 24680af
AckProof had its own `AInv`: Core's 17-conjunct `Inv`, plus:
- `TermLe`: `term[n] ≤ cfg.ver` for every node.
- `HeldInv`: every held `(p, o, t)` has `t ≤ cfg.ver`. If `ThinksPrim(p) ∧ term[p] = t`,
  then `1 ≤ o ≤ |log[p]|` and `log[p][o] = t`.
- `AckedDurable`.

`step_facts` shows that every Core step extends `hist`, does not lower `cfg.ver`, keeps
`TermLe`, and preserves the fence: a node that is fenced for tag `t ≤ cfg.ver` after the
step was fenced before it, and its log only grew. The interesting cases are:
- `Takeover` gives a new term `cfg.ver + 1`, which exceeds every existing tag.
- `Learn` can never make a node primary, because the current primary already knows the
  latest config.
- `Resync` changes only a node that does not think it is primary.

`Release` then reads `log[p][o] = t` from `HeldInv`. `CommittedAgree` with
`o ≤ commit[p] ≤ |hist|` turns that into `hist[o] = t`.

## Counterexample (`AckProof/Counterexample.lean`)

`AckProof.noReplyFence_unsafe`: with NoReplyFence = TRUE, a 14-step run on `Fin 3`
acknowledges `(1, 1)` while `hist = [5]`. It is the same scenario as TLC's
`CoreAck_noReplyFence` trace, and it reuses the offset after a full resync:
1. n0 writes at offset 1 with term 1, and the reply is held.
2. n1 takes over with an empty log, and n0 is resynced to the empty log.
3. n0 is promoted again and takes over with term 5.
4. n0 writes term 5 at offset 1 and commits it alone.
5. The old reply is released as OK.

## Axioms

```
'AckProof.ackSafety' depends on axioms: [propext, Classical.choice, Quot.sound]
'AckProof.ackedHeld' depends on axioms: [propext, Classical.choice, Quot.sound]
'AckProof.noReplyFence_unsafe' depends on axioms: [propext, Classical.choice, Quot.sound]
```

There is no `sorry`.

## Size

| File | Lines |
|---|---|
| `AckProof/Model.lean` | 70 |
| `AckProof/Proof.lean` | 48 (the embedding into CoreRecon; 190 until 24680af) |
| `AckProof/Counterexample.lean` | 53 |
| `AckProof.lean` | 3 |
| total | 174 |

## TLC (`spec/CoreAck.tla`, 3 nodes, MaxLen 2, MaxVer 6; `gen_coreack_cfgs.sh`)

| cfg | result |
|---|---|
| `CoreAck_safe` | pass, 10,382,580 distinct states, ~40 s. Checks TypeOK, EHC, CA, AckedDurable, AckedHeld, HeldInv, TermLe |
| `CoreAck_noReplyFence` | violated: AckedDurable (15-state trace) |
| `CoreAck_noReconcile` | violated: AckedHeld (14-state trace) |
| `CoreAck_ReachAck` | violated: Reach_Ack (an OK is released) |
| `CoreAck_ReachDeposedAck` | violated: Reach_DeposedAck (a deposed primary can release OK) |
| `CoreAck_ReachDeposedCommit` | violated: Reach_DeposedCommit (a deposed primary can advance commit) |

# Reconciliation against the inherited log, any min-sync-replicas (`ReconProof/`, CoreRecon.tla)

## Theorem

`ReconProof/Safety.lean`, `ReconProof/AckProof.lean`:

```lean
theorem ReconProof.safety {Node : Type} [DecidableEq Node] {m : Nat} {i : Node}
    {s : State Node} (h : Reachable m true i s) : Safety s

theorem ReconProof.ackSafety {Node : Type} [DecidableEq Node] {i : Node}
    {a : AState Node} {m : Nat} (h : AReachable m true false i a) :
    AckedDurable a ∧ Safety a.core

theorem ReconProof.ackedHeld ... (h : AReachable m true false i a) : AckedHeld a
```

`m` is MinSync and is universally quantified, so this covers every
min-sync-replicas value, every node type with decidable equality, unbounded logs
and unbounded configuration changes. `Safety` is `EligibleHoldsCommitted /\
CommittedAgree`. `true` is ReconcileFirst; `false` in `AReachable` is
NoReplyFence (the F2 fence is on).

## Rule proven (A1)

While a new primary `p` is unreconciled (`commit[p] < recon[p]`), promotion
catch-up and full resync are measured against the log it inherited:

```
CatchUpPoint(p) = IF Reconciled(p) THEN commit[p] ELSE Min(recon[p], Len(log[p]))
Count(p, r):  acked[p][r] >= CatchUpPoint(p)            (no Reconciled gate)
Resync(r):    log[r] := Prefix(log[p], CatchUpPoint(p))  (no Reconciled gate)
AppendWrite:  Writable(p) /\ Reconciled(p)              (serve only once reconciled)
Commit:       Writable(p) /\ the commit rule
Writable(p) == Cardinality(sync[p] \cap known[p].elig) >= MinSync
```

`Writable` is Shard.tla's definition (`EligOf(p)` there is the eligible set of the
config `p` knows). In Lean, `writable m s p` says some duplicate-free list of
length at least `m` consists of nodes in `sync[p]` that are eligible in
`known[p]`; this is the cardinality bound without a `Fintype` assumption.

The proof never reads `writable`: it only restricts AppendWrite and Commit, and
the invariant is preserved by every step whether or not the guard holds. So the
all-`m` theorem needed no simulation lemma.

## Relation to Core's stricter rule

Core (and CoreRecon with `StrictRecon = TRUE`) gates Count and Resync on
Reconciled and measures them against `commit[p]`. A strict Count or Resync step
fires only when `p` is reconciled, where `CatchUpPoint(p) = commit[p]`, so every
strict step is an A1 step: strict reachable states are a subset of A1 reachable
states and the theorem covers the strict rule too. At MinSync 0 this is a Lean
lemma: `CoreProof.step_toRecon` proves every Core step is an A1 step, and
`CoreProof.safety` is derived from `ReconProof.safety` through it. For MinSync ≥ 1
it is still argued, not stated in Lean, because CoreRecon's `StrictRecon = TRUE`
is not encoded. The A1 rule is strictly more permissive: it allows an unreconciled
primary to count a replica that has acked its whole inherited log, which is
what removes the deadlock below.

## Invariant changes (`ReconProof/Inv.lean` until 24680af)

ReconProof now has no invariant of its own: `ReconProof.safety` is derived from
`RestartProof.safety` through `ReconProof.emb`. The changes below, made when ReconProof
was a copy of CoreProof, are carried in `RestartProof/Inv.lean` and the lemmas named
here are in `RestartProof/Actions1-3.lean`.

CoreProof's 17 conjuncts and `Wit` are kept, except:

- `syncNew` (counted, not yet eligible replica `r` of the current primary):
  Core required the primary to be reconciled (`recon ≤ commit`). Now it requires
  `recon[prim] ≤ acked[prim][r]`: the replica acked the whole inherited log.
  Core's version implies this one through `syncCommit`.
- New `reconLen`: `recon[prim] ≤ Len(log[prim])`. An unreconciled primary
  cannot append and no step shortens a primary's log, so
  `CatchUpPoint = recon` for the current primary.

Where it matters:

- Count: `recon_le_cup` (`recon[prim] ≤ CatchUpPoint`) gives the new
  `syncNew`; `commit_le_cup` (`commit ≤ CatchUpPoint`) gives `syncCommit`.
- MakeEligible: the new eligible replica holds `hist` because
  `Len(hist) ≤ commit[prim] ≤ acked` or `Len(hist) ≤ recon[prim] ≤ acked`
  (`histLe`), and it holds `take acked log[prim]` (`ackHolds`), of which `hist`
  is a prefix (`ehc`). The stale-witness case uses
  `acked[p][w] ≤ recon[prim] ≤ acked[prim][r]` directly.
- Resync: `commit[p] ≤ CatchUpPoint(p) ≤ Len(log[p])` (`cup_le_len`) is all
  CommittedAgree needs; Core's Resync proof never used the Reconciled gate.
- Ack: the acked value only grows, so `syncNew` is kept.

The stale/`Wit` conjunct needed no change.

## Counterexample (`ReconProof/Counterexample.lean`)

`ReconProof.noReconcile_unsafe`: with `ReconcileFirst = FALSE` (CatchUpPoint is
the own commit, AppendWrite ungated) at MinSync 0, Core's 11-step F1 run on
`Fin 3` reaches a state violating EligibleHoldsCommitted. `decide` discharges the
guards.

## Encoding differences to CoreRecon.tla

As CoreProof: `MaxLen`/`MaxVer` dropped. EarlyUncount is omitted (EarlyDrop =
FALSE). StrictRecon is not encoded (see above). As in AckProof, `AStep.core`
allows an append without a reply, a superset of CoreRecon.tla's `Write`.

## Axioms

```
'ReconProof.safety' depends on axioms: [propext, Classical.choice, Quot.sound]
'ReconProof.ackSafety' depends on axioms: [propext, Classical.choice, Quot.sound]
'ReconProof.ackedHeld' depends on axioms: [propext, Classical.choice, Quot.sound]
'ReconProof.noReconcile_unsafe' depends on axioms: [propext, Quot.sound]
```

There is no `sorry`.

## Size

| File | Lines | Content |
|---|---|---|
| `ReconProof/Model.lean` | 169 | the CoreRecon model |
| `ReconProof/Embed.lean` | 129 | `emb` into CoreRestart, `step_emb`, `reach_emb`, `safety_of_emb` |
| `ReconProof/Safety.lean` | 17 | `safety` |
| `ReconProof/AckModel.lean` | 70 | held replies |
| `ReconProof/AckProof.lean` | 50 | `aemb`, `astep_emb`, `ackSafety`, `ackedHeld` |
| `ReconProof/Counterexample.lean` | 54 | `noReconcile_unsafe` |
| `ReconProof.lean` | 6 | |
| total | 495 | 1742 until 24680af, when ReconProof was a copy of CoreProof + AckProof |

## TLC (`spec/CoreRecon.tla`, 3 nodes, MaxLen 2, MaxVer 6; `gen_corerecon_cfgs.sh`)

| cfg | MinSync | rule | result | distinct states |
|---|---|---|---|---|
| `CoreRecon_safeMin0` | 0 | A1 | pass (TypeOK, EHC, CA, AckedDurable, AckedHeld, SyncNew, ReconLen) | 14,206,308 |
| `CoreRecon_safeMin1` | 1 | A1 | pass (same) | 9,289,348 |
| `CoreRecon_strictMin0` | 0 | strict | pass (TypeOK, EHC, CA, AckedDurable, AckedHeld) | 10,382,580 |
| `CoreRecon_strictMin1` | 1 | strict | pass (same) | 3,800,584 |
| `CoreRecon_noReconcileMin0` | 0 | ReconcileFirst = FALSE | violated: EligibleHoldsCommitted | 23,501 |
| `CoreRecon_noReconcileMin1` | 1 | ReconcileFirst = FALSE | violated: EligibleHoldsCommitted | 9,645 |
| `CoreRecon_noReplyFenceMin1` | 1 | NoReplyFence = TRUE | violated: AckedDurable | 375,915 |
| `CoreRecon_earlyDropMin1` | 1 | EarlyDrop = TRUE | violated: EligibleHoldsCommitted | 7,277 |
| `CoreRecon_ReachAckMin1` | 1 | A1 | violated: Reach_Ack (reachable) | 1,679 |
| `CoreRecon_ReachLoneMin1` | 1 | A1 | violated: Reach_Lone (reachable) | 1,120 |
| `CoreRecon_ReachLoneReconMin1` | 1 | A1 | violated: Reach_LoneReconcile (reachable) | 12,161 |
| `CoreRecon_strictReachLoneMin1` | 1 | strict | violated: Reach_Lone (reachable) | 1,471 |
| `CoreRecon_strictReachLoneReconMin1` | 1 | strict | pass: Reach_LoneReconcile unreachable | 3,800,584 |
| `CoreRecon_strictReachReconMin1` | 1 | strict | violated: Reach_Reconcile (reachable) | 25,137 |

`strictMin0` has exactly CoreAck_safe's state count (10,382,580), as expected:
at MinSync 0 with the strict rule CoreRecon is CoreAck.

Deadlock evidence. The ghost `lone` holds a primary whose primaryship began by a
takeover with a non-empty inherited log and an empty sync set (no eligible
survivor). `Reach_LoneReconcile` says such a primary later reconciles.

- A1, MinSync 1, reachable in 12 states: Count(n1,n2), MakeEligible(n1,n2),
  Learn(n2), Write(n1), Replicate(n2), Takeover(n2) (sync set empty, n2 lone),
  Learn(n1), Ack(n1), Count(n2,n1) (ack 1 ≥ CatchUpPoint = recon 1),
  MakeEligible(n2,n1), Commit(n2).
- Strict, MinSync 1: unreachable over the full state space (3,800,584 states, the
  same as `strictMin1`), while a lone primary itself is reachable
  (`strictReachLoneMin1`). It is stuck: Count needs Reconciled, Commit needs
  Writable, and the sync set can only grow through Count.
- The strict rule can still reconcile when a takeover keeps an eligible survivor
  in the sync set (`strictReachReconMin1`): n1 counts n2 and n3, makes both
  eligible, writes, n2 takes over with sync set {n3}, n3 acks, n2 commits. So the
  deadlock needs a takeover with no eligible survivor.

# Restarts, incarnations and the F5 fix (`RestartProof/`, CoreRestart.tla)

## Theorems

`RestartProof/Safety.lean`, `RestartProof/AckProof.lean`:

```lean
theorem RestartProof.safety {Node : Type} [DecidableEq Node] {m : Nat} {i : Node}
    {s : State Node} (h : Reachable m true true i s) : Safety s

theorem RestartProof.ackSafety {Node : Type} [DecidableEq Node] {i : Node}
    {a : AState Node} {m : Nat} (h : AReachable m true false true i a) :
    AckedDurable a ∧ Safety a.core

theorem RestartProof.ackedHeld ... (h : AReachable m true false true i a) : AckedHeld a

theorem RestartProof.commit_two_copies {m : Nat} (hm : 1 ≤ m) {i : Node} {s : State Node}
    (h : Reachable m true true i s) {p : Node} (hp : tp s p) (hpp : p = s.cfg.prim)
    (hw : writable m s p) (hlen : s.commit p < (s.log p).length)
    (hcap : ∀ r, s.sync p r = true → s.commit p < s.acked p r) :
    ∃ e, e ≠ p ∧ auth (commitA s p) e ∧ auth (commitA s p) p ∧
      (commitA s p).hist <+: (commitA s p).log e ∧ (commitA s p).hist <+: (commitA s p).log p
```

`m` (MinSync) is universally quantified. The Bool arguments are ReconcileFirst =
`true`, NoReplyFence = `false` (F2 fence on) and RebuildSync = `true` (F5 fix on).
Nodes, log lengths, configuration changes, incarnations and the number of restarts
are unbounded. `Safety` is `EHC /\ CA`, where EligibleHoldsCommitted is restated
for authoritative nodes:

```
auth s n  :=  (cfg.elig n ∨ n = cfg.prim) ∧ cfg.inc n = inc n      -- TLA+: n \in Auth
EHC s     :=  ∀ n, auth s n → hist <+: log n
AckedHeld :=  ∀ acknowledged (o, v), ∀ n, auth s n → log n holds v at o
```

The primary is authoritative only while its own incarnation matches: a primary
that came back empty holds nothing and is not a primary any more.

## Survival assumption

RestartEmpty(n) (n loses its log and gets a new incarnation) has the guard

```
∃ m, m ≠ n ∧ auth s m          -- TLA+: \E m \in Auth : m # n
```

At every restart without data, some other node that is the primary or an eligible
replica, with the incarnation the config records for it, keeps its data. This is
the doc's "as long as the primary or at least one eligible replica keeps its
data", stated per restart. RestartKeep (restart with data) has no guard. With the
guard, `Auth` is never empty (`Inv.authEx`), so by EHC some node always holds the
whole committed history; the TLA+ history cut (below) never fires and the Lean
model leaves `hist` unchanged on RestartEmpty.

`NoSurvivor = TRUE` drops the guard. As in Shard.tla, a restart that leaves no
authoritative holder of the history is then an explicit loss that cuts the ghost
history to `<< >>`, and AckedDurable fails (`CoreRestart_noSurvivorMin0/Min1`).

## What MinSync >= 1 adds

EHC alone says every authoritative node holds the history. With MinSync = 0 the
current primary may commit with an empty sync set, so the history may be on the
primary only. `commit_two_copies`: with MinSync >= 1, a commit by the current
primary needs an eligible sync member whose recorded incarnation matches (Writable
counts only those), so right after the commit the history is on at least two
distinct authoritative nodes, the primary and that replica. A later RestartEmpty of
one of them is allowed by the survival guard as long as another authoritative
node remains. TLC checks the same statement as the action property `TwoCopies`.

## Model (`RestartProof/Model.lean`; TLA+ header of `spec/CoreRestart.tla`)

CoreRecon plus:

- `inc n`; the config records `cfg.inc r`. MakeEligible records `inc[r]`;
  Takeover keeps `cfg.inc` and checks `cfg.inc[n] = inc[n]`.
- ThinksPrim checks the incarnation the config `p` knows records for `p`:
  `tp s p := known[p].prim = p ∧ known[p].inc p = inc p`.
- Primaries ack-count only replicas of the matching incarnation: Writable counts
  `sync[p] ∩ {r ∈ known[p].elig : known[p].inc r = inc r}`; Count refuses an
  eligible replica whose incarnation does not match; Takeover counts only the
  matching eligible replicas; when an eligible replica restarts empty the current
  primary stops counting it and forgets its ack. It rejoins by demotion and the
  promotion flow (MakeIneligible, Count, MakeEligible records the new incarnation).
- Both restarts require KnowsLatest(n): a node syncs the Cluster V2 log first
  (doc s6). A node that does not know the latest config takes a Learn step first,
  which reaches the same state.
- RestartEmpty(n): `log = []`, `inc + 1`, commit, recon, own acks and own sync set
  reset; the current primary drops `n` from its sync set and resets its ack. Its
  old incarnation stays in `cfg.inc` until it is demoted, but it can neither act
  as primary nor take over.
- RestartKeep(n): log and incarnation kept; commit 0, own acks 0, `recon = Len(log)`
  (it must commit its log again before serving). If `n` is primary, F5 fix:
  `sync[n] := {r ∈ known[n].elig : known[n].inc r = inc r} \ {n}`, so it commits
  nothing until fresh acks arrive. Other primaries keep their ack from `n`, whose
  log is unchanged.
- Held replies of the restarted node are dropped (`dropHeld`): they are lost with
  the client connections, so the client gets neither OK nor UNCOMMITTED. A restarted
  primary therefore never acknowledges a write it held before the restart.
  `AStep.core` also allows a restart that keeps them (a superset).

## Invariant changes (`RestartProof/Inv.lean`)

ReconProof's invariant, with:

- `ehc`, and the last clause of `Wit`, over authoritative nodes.
- `eligSync` (O1) only for eligible replicas whose recorded incarnation matches.
- `histLe`, and the `Wit` clauses about the current primary's log and recon point,
  only while the current primary is authoritative (`pa s`).
- New: `incLe` (`cfg.inc n ≤ inc n`), `knownIncLe` (the same for every known
  config): a restarted-empty node never matches its old record.
- New: `authEx` (some node is authoritative), kept by the survival guard. Commit
  uses it where ReconProof used the current primary as the common holder of the
  history.

Preservation for the two restarts is `RestartProof/Restart.lean`. RestartKeep
with RebuildSync = TRUE keeps `eligSync`, the conjunct F5 breaks: with an empty
sync set a restarted primary may commit without an eligible replica, which then
lacks the write (`CoreRestart_noRebuildMin1`).

## Axioms

```
'RestartProof.safety' depends on axioms: [propext, Classical.choice, Quot.sound]
'RestartProof.ackSafety' depends on axioms: [propext, Classical.choice, Quot.sound]
'RestartProof.ackedHeld' depends on axioms: [propext, Classical.choice, Quot.sound]
'RestartProof.commit_two_copies' depends on axioms: [propext, Classical.choice, Quot.sound]
```

There is no `sorry`.

## Size

RestartProof was written as a copy of ReconProof; "Changed" counts diff lines (`<` plus
`>`) against the ReconProof file at 24680af with the namespace renamed. It is now the
only invariant proof (see "Proof structure").

| File | Lines | Changed |
|---|---|---|
| `RestartProof/Lists.lean` | 66 | 0 |
| `RestartProof/Model.lean` | 216 | 145 |
| `RestartProof/Inv.lean` | 147 | 90 |
| `RestartProof/Actions1.lean` | 315 | 43 |
| `RestartProof/Actions2.lean` | 450 | 180 |
| `RestartProof/Actions3.lean` | 343 | 102 |
| `RestartProof/Restart.lean` | 326 | new |
| `RestartProof/Safety.lean` | 62 | 48 |
| `RestartProof/AckModel.lean` | 81 | 49 |
| `RestartProof/AckProof.lean` | 215 | 64 |
| `RestartProof.lean` | 2 | |
| total | 2223 | |

## TLC (`spec/CoreRestart.tla`, 3 nodes, MaxLen 2, MaxVer 6, MaxRestarts 1; `gen_corerestart_cfgs.sh`)

| cfg | MinSync | switch | result | distinct states |
|---|---|---|---|---|
| `CoreRestart_safeMin0` | 0 | fixed | pass (TypeOK, EHC, CA, AckedDurable, AckedHeld, SyncNew, ReconLen, EligSync, IncLe) | 120,618,059 |
| `CoreRestart_safeMin1` | 1 | fixed | pass (same) | 78,768,204 |
| `CoreRestart_twoCopiesMin1` | 1 | fixed | pass (action property TwoCopies) | 78,768,204 |
| `CoreRestart_noSurvivorMin0` | 0 | NoSurvivor = TRUE | violated: AckedDurable | 1,706 |
| `CoreRestart_noSurvivorMin1` | 1 | NoSurvivor = TRUE | violated: AckedDurable | 64,983 |
| `CoreRestart_noRebuildMin0` | 0 | RebuildSync = FALSE | violated: EligibleHoldsCommitted | 4,954 |
| `CoreRestart_noRebuildMin1` | 1 | RebuildSync = FALSE | violated: EligibleHoldsCommitted | 16,689 |
| `CoreRestart_noRebuildMin1Ack` | 1 | RebuildSync = FALSE | violated: AckedHeld | 120,853 |
| `CoreRestart_noRebuildMin1Loss` | 1 | RebuildSync = FALSE | violated: AckedDurable | 8,806,708 |
| `CoreRestart_noIncMin1` | 1 | IncCheck = FALSE | violated: EligibleHoldsCommitted | 27,361 |
| `CoreRestart_noReconcileMin1` | 1 | ReconcileFirst = FALSE | violated: EligibleHoldsCommitted | 23,611 |
| `CoreRestart_noReplyFenceMin1` | 1 | NoReplyFence = TRUE | violated: AckedDurable | 1,891,176 |
| `CoreRestart_earlyDropMin1` | 1 | EarlyDrop = TRUE | violated: EligibleHoldsCommitted | 20,986 |
| `CoreRestart_ReachRestartEmptyMin1` | 1 | fixed | violated: Reach_RestartEmpty (reachable) | 1,530 |
| `CoreRestart_ReachTakeoverAfterEmptyMin1` | 1 | fixed | violated: Reach_TakeoverAfterEmpty (reachable) | 39,907 |
| `CoreRestart_ReachReEligibleMin1` | 1 | fixed | violated: Reach_ReEligible (reachable) | 1,809 |
| `CoreRestart_ReachKeepCommitMin1` | 1 | fixed | violated: Reach_KeepCommit (reachable) | 2,162 |
| `CoreRestart_ReachAckSurvivesEmptyPrimMin1` | 1 | fixed | violated: Reach_AckSurvivesEmptyPrim (reachable) | 61,062 |

Traces:

- `noRebuildMin1` (F5, 10 states): n1 counts n2 and makes it eligible, writes,
  restarts with its data and an empty sync set, counts n3 (which acked the
  write), makes n3 eligible and commits with n3's ack only. n2 is eligible and
  authoritative but lacks the committed write.
- `noRebuildMin1Loss` (22 states): the same start, then n2, eligible and lacking
  the committed write, takes over (Learn, Takeover), resyncs n1 to its own log,
  writes at offset 1, counts n1, makes n1 eligible, demotes n3 (the only other
  holder of the committed write) and commits: the committed write at offset 1 is
  replaced. The OK released for the new write fails AckedDurable against the
  ghost history. The lost write itself was not acknowledged (its held reply was
  dropped with the restart); MaxLen 2 leaves no room for an acknowledged write
  after reconciliation. So without the F5 fix a committed write is lost, which
  Shard_f_noRestoreMin1Loss did not reach within its bounds (one client write).
- `noSurvivorMin1` (10 states): a write commits on n1 and n2, n2 is demoted, the
  OK is released, n1 (the only authoritative node) restarts empty.
- `noIncMin1` (11 states): without incarnation checks n1 restarts empty and still
  acts as primary: it counts n3 and makes it eligible with an empty log.
- `ReachAckSurvivesEmptyPrimMin1` (11 states): a write commits on n1 and n2 and is
  acknowledged, n1 restarts empty, n2 takes over with the write.

Coverage (`COVERAGE=1`): see `spec/coverage.tsv`.

# Size after the dedup (2026-11-05)

Lines per tree, including the root module file, at 24680af and after the dedup:

| Tree | Before | After | What is left |
|---|---|---|---|
| `CoreProof` | 1411 | 360 | model, `Embed.lean` (Core → CoreRecon at MinSync 0), `safety`, counterexample |
| `AckProof` | 316 | 174 | model, embedding into CoreRecon, `ackSafety`/`ackedHeld`, counterexample |
| `ReconProof` | 1742 | 495 | model, `Embed.lean` (CoreRecon → CoreRestart), `safety`/`ackSafety`/`ackedHeld`, counterexample |
| `RestartProof` | 2223 | 2223 | unchanged; the only invariant proof |
| `XCheck` | 899 | 932 | check (d) now decides `RestartProof.Inv`/`AInv` of the lifted states |
| total | 6591 | 4184 | |

Checks after the change:

- `lake build`: clean, no `sorry`.
- `Check.lean`: the same axioms for all 13 theorems as at 24680af.
- All 13 theorem statements, and the `Safety`, `Reachable`, `AckedDurable` and
  `AckedHeld` definitions they mention, print identically (`#check @…` / `#print`)
  before and after.
- `XCheck/Axioms.lean`: `propext`, `Quot.sound`, plus `Classical.choice` for
  `asuccs_sound`/`astepLB_sound`, as before.
- `xcheck` and `xcheck_ack` rerun on freshly generated TLC graphs: 0 failures.
