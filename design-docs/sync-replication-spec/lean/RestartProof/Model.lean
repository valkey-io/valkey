import RestartProof.Lists

/-!
# CoreRestart: the replication model

Transition-system encoding of the replication part of `spec/CoreRestart.tla`
(EarlyDrop = FALSE, IncCheck = TRUE, NoSurvivor = FALSE).

## In prose

A shard has one primary and any number of replicas. The primary appends client
writes to its log, replicas copy the log and acknowledge how far they have copied,
and the primary commits an entry once every replica in its sync set has
acknowledged it. Cluster V2 holds a versioned configuration (the primary, the set
of replicas eligible to take over, and the incarnation recorded for each node);
every node keeps its own possibly stale copy. A node can restart and keep its data,
or restart empty with a new incarnation.

The theorem proved about this model (`RestartProof.safety` in `Safety.lean`):

> In every reachable state, for every min-sync-replicas value, every authoritative
> node (the primary or an eligible replica, with the incarnation the configuration
> records for it) holds the whole committed history, and no node has committed an
> entry that differs from that history.

It assumes the F1 fix (a new primary commits its inherited log before writing,
`rf = true`), the F5 fix (a restarted primary rebuilds its sync set, `rb = true`),
and the survival assumption (a node restarts empty only while some other
authoritative node keeps its data).

## Relation to the other models

Same as `ReconProof.Model` plus:
- `inc n`: the node's incarnation; `cfg.inc r`: the incarnation the config records.
- ThinksPrim checks the recorded incarnation: `ThinksPrimary s p` is
  `known[p].prim = p /\ known[p].inc[p] = inc[p]`.
- `IsAuthoritative s n`: `n \in cfg.elig \catchUpPoint {cfg.prim} /\ cfg.inc[n] = inc[n]` (Auth).
- Writable counts eligible sync members with a matching incarnation; Count does not
  count an eligible replica whose incarnation does not match; Takeover checks the new
  primary's incarnation and counts only the matching eligible replicas; MakeEligible
  records the replica's incarnation.
- RestartEmpty / RestartKeep (both require KnowsLatest, see the TLA+ header).
  `rb` = RebuildSync. RestartEmpty is guarded by the survival assumption
  `∃ m, m ≠ n ∧ IsAuthoritative s m`. Under it the TLA+ history cut never fires
  (EligibleHoldsCommitted), so here RestartEmpty leaves `hist` unchanged.

## Encoding conventions

- Sets of nodes are `Node → Bool`.
- A log entry is the term it was written in, so a write is identified by its
  offset and term.
- Each action is a function `State → State` returning the post-state; fields it
  does not mention keep their value (TLA+ `UNCHANGED`). Its guard lives in the
  matching `Step` constructor.
- No `MaxLen` / `MaxVer` / `MaxRestarts` bounds.
-/

namespace RestartProof

/-- Functional update: `f` with the value at `a` replaced by `b`
(TLA+ `[f EXCEPT ![a] = b]`). -/
def upd {α β : Type} [DecidableEq α] (f : α → β) (a : α) (b : β) : α → β :=
  fun x => if x = a then b else f x

@[simp] theorem upd_same {α β : Type} [DecidableEq α] (f : α → β) (a : α) (b : β) :
    upd f a b a = b := by simp [upd]

theorem upd_ne {α β : Type} [DecidableEq α] (f : α → β) {a x : α} (b : β) (h : x ≠ a) :
    upd f a b x = f x := by simp [upd, h]

/-- A Cluster V2 configuration. -/
structure Cfg (Node : Type) where
  /-- Version; every configuration change increments it. -/
  ver  : Nat
  /-- The primary. -/
  prim : Node
  /-- The replicas eligible to take over. -/
  elig : Node → Bool
  /-- The incarnation the configuration records for each node. -/
  inc  : Node → Nat

/-- The state of the whole shard, plus the ghost history. -/
structure State (Node : Type) [DecidableEq Node] where
  /-- Each node's replication log; an entry is the term it was written in. -/
  log    : Node → List Nat
  /-- Each node's actual incarnation; restarting empty increments it. -/
  inc    : Node → Nat
  /-- The latest configuration in Cluster V2. -/
  cfg    : Cfg Node
  /-- The configuration each node has seen, possibly stale. -/
  known  : Node → Cfg Node
  /-- `sync p r`: `r` is in `p`'s sync set, so `p` waits for `r`'s ack to commit. -/
  sync   : Node → Node → Bool
  /-- `acked p r`: how far `p` believes `r` has acknowledged. -/
  acked  : Node → Node → Nat
  /-- Each node's commit point. -/
  commit : Node → Nat
  /-- Catch-up point: a new primary must commit up to here before taking writes. -/
  recon  : Node → Nat
  /-- The term each node writes new entries with. -/
  term   : Node → Nat
  /-- Ghost variable: the committed history. No node stores it; it is extended the
  first time any node commits a given offset. -/
  hist   : List Nat

variable {Node : Type} [DecidableEq Node]

/-- The initial configuration: `i` is primary at version 1, no replica is eligible,
every incarnation is 1. -/
def initCfg (i : Node) : Cfg Node := ⟨1, i, fun _ => false, fun _ => 1⟩

/-- The initial state: empty logs and history, every node knows `initCfg i`, and `i`
writes with term 1. -/
def init (i : Node) : State Node where
  log    := fun _ => []
  inc    := fun _ => 1
  cfg    := initCfg i
  known  := fun _ => initCfg i
  sync   := fun _ _ => false
  acked  := fun _ _ => 0
  commit := fun _ => 0
  recon  := fun _ => 0
  term   := fun n => if n = i then 1 else 0
  hist   := []

/-- ThinksPrim(p): `p`'s own configuration names it primary and records its current
incarnation. A primary that restarted empty fails the second half. -/
def ThinksPrimary (s : State Node) (p : Node) : Prop :=
  (s.known p).prim = p ∧ (s.known p).inc p = s.inc p
/-- KnowsLatest(n): `n` has seen the latest configuration. -/
def KnowsLatest (s : State Node) (n : Node) : Prop := (s.known n).ver = s.cfg.ver
/-- PrimOf(n): the node `n` believes is primary, the one it replicates from. -/
def primaryOf (s : State Node) (n : Node) : Node := (s.known n).prim
/-- Reconciled(p): with ReconcileFirst on, `p` has committed up to its catch-up point. -/
def Reconciled (rf : Bool) (s : State Node) (p : Node) : Prop := rf = true → s.recon p ≤ s.commit p
/-- `n \in Auth`: in the latest configuration, `n` is the primary or an eligible
replica, and the incarnation recorded for it is its actual one. These are the nodes
that must hold the committed history. -/
def IsAuthoritative (s : State Node) (n : Node) : Prop :=
  (s.cfg.elig n = true ∨ n = s.cfg.prim) ∧ s.cfg.inc n = s.inc n
/-- The current primary is authoritative (it has not come back empty). -/
def PrimaryAuthoritative (s : State Node) : Prop := s.cfg.inc s.cfg.prim = s.inc s.cfg.prim

/-! ### Post-states of the actions (the UNCHANGED parts are implicit). -/

/-- AppendWrite(p): `p` appends one client write to its own log, tagged with its
current term. Nothing else changes. -/
def appendWrite (s : State Node) (p : Node) : State Node :=
  { s with log := upd s.log p (s.log p ++ [s.term p]) }

/-- Replicate(r): `r` copies the next entry of its primary's log onto its own. -/
def replicate (s : State Node) (r : Node) : State Node :=
  { s with log := upd s.log r (s.log r ++ [(s.log (primaryOf s r)).getD (s.log r).length 0]) }

/-- Ack(r): `r`'s primary records that `r` holds its whole log. -/
def ack (s : State Node) (r : Node) : State Node :=
  { s with acked := upd s.acked (primaryOf s r) (upd (s.acked (primaryOf s r)) r (s.log r).length) }

/-- Commit(p): `p` commits its next entry. If no node has committed this offset yet,
the entry is appended to the ghost history. -/
def commitNext (s : State Node) (p : Node) : State Node :=
  { s with commit := upd s.commit p (s.commit p + 1),
           hist := if s.commit p = s.hist.length
                   then s.hist ++ [(s.log p).getD (s.commit p) 0] else s.hist }

/-- Count(p, r): `p` adds `r` to its sync set. -/
def count (s : State Node) (p r : Node) : State Node :=
  { s with sync := upd s.sync p (upd (s.sync p) r true) }

/-- MakeEligible's configuration: next version, `r` eligible, `r`'s current
incarnation recorded. -/
def makeEligCfg (s : State Node) (r : Node) : Cfg Node :=
  { s.cfg with ver := s.cfg.ver + 1, elig := upd s.cfg.elig r true,
               inc := upd s.cfg.inc r (s.inc r) }

/-- MakeIneligible's configuration: next version, `r` no longer eligible. -/
def makeIneligCfg (s : State Node) (r : Node) : Cfg Node :=
  { s.cfg with ver := s.cfg.ver + 1, elig := upd s.cfg.elig r false }

/-- MakeEligible(p, r): primary `p` publishes a configuration in which `r` is eligible. -/
def makeElig (s : State Node) (p r : Node) : State Node :=
  { s with cfg := makeEligCfg s r, known := upd s.known p (makeEligCfg s r) }

/-- MakeIneligible(p, r): primary `p` publishes a configuration in which `r` is not
eligible, and drops `r` from its sync set. -/
def makeInelig (s : State Node) (p r : Node) : State Node :=
  { s with cfg := makeIneligCfg s r, known := upd s.known p (makeIneligCfg s r),
           sync := upd s.sync p (upd (s.sync p) r false) }

/-- Learn(n): `n` adopts the latest configuration; unless it is the primary there,
it empties its sync set. -/
def learn (s : State Node) (n : Node) : State Node :=
  { s with known := upd s.known n s.cfg,
           sync := upd s.sync n (if s.cfg.prim = n then s.sync n else fun _ => false) }

/-- `cfg.elig \ {cfg.prim, n}`: the eligible set after `n` takes over from the
current primary. -/
def takeoverElig (s : State Node) (n : Node) : Node → Bool :=
  fun x => s.cfg.elig x && decide (x ≠ s.cfg.prim) && decide (x ≠ n)

/-- Takeover's configuration: next version, `n` primary, `takeoverElig` eligible, the
recorded incarnations unchanged. -/
def takeoverCfg (s : State Node) (n : Node) : Cfg Node := ⟨s.cfg.ver + 1, n, takeoverElig s n, s.cfg.inc⟩

/-- MatchElig of the new config: the matching eligible replicas. -/
def takeoverSync (s : State Node) (n : Node) : Node → Bool :=
  fun x => takeoverElig s n x && decide (s.cfg.inc x = s.inc x)

/-- Takeover(n): eligible replica `n` becomes primary in a new configuration. Its
sync set is the matching eligible replicas, its acks and commit point are reset, its
catch-up point is the end of its log (so it commits what it inherited before it
writes), and it writes with a new term. -/
def takeover (s : State Node) (n : Node) : State Node :=
  { s with cfg := takeoverCfg s n, known := upd s.known n (takeoverCfg s n),
           sync := upd s.sync n (takeoverSync s n),
           acked := upd s.acked n (fun _ => 0),
           commit := upd s.commit n 0,
           recon := upd s.recon n (s.log n).length,
           term := upd s.term n (s.cfg.ver + 1) }

/-- The point a replica must have acknowledged before `p` counts it: `p`'s catch-up
point while it is still reconciling, otherwise its commit point. -/
def catchUpPoint (rf : Bool) (s : State Node) (p : Node) : Nat :=
  if rf = true ∧ s.commit p < s.recon p then min (s.recon p) (s.log p).length else s.commit p

/-- `sync[p] \cap MatchElig(known[p])`: the sync members of `p` that are eligible in
`p`'s configuration with the incarnation it records for them. These are the replicas
that count toward `min-sync-replicas`. -/
def syncEligible (s : State Node) (p : Node) : Set Node :=
  {r | s.sync p r = true ∧ (s.known p).elig r = true ∧ (s.known p).inc r = s.inc r}

/-- Writable(p): `Cardinality(sync[p] \cap MatchElig(known[p])) >= m`, that is, at
least `m` distinct sync members of `p` are eligible in `p`'s configuration with the
incarnation it records for them. `Set.encard` is the cardinality in `ℕ∞`, so this
needs no finiteness assumption on `Node`. -/
def Writable (m : Nat) (s : State Node) (p : Node) : Prop :=
  (m : ℕ∞) ≤ (syncEligible s p).encard

/-- `Writable` is the earlier list form: some duplicate-free list of at least `m`
nodes, all counted. -/
theorem writable_iff_list (m : Nat) (s : State Node) (p : Node) :
    Writable m s p ↔ ∃ l : List Node, l.Nodup ∧ m ≤ l.length ∧
      ∀ r ∈ l, s.sync p r = true ∧ (s.known p).elig r = true ∧ (s.known p).inc r = s.inc r :=
  le_encard_iff_list m _

/-- Resync(r): a replica whose log has diverged from its primary's replaces it with
the primary's log up to the primary's catch-up point, and starts acking from 0. -/
def resync (rf : Bool) (s : State Node) (r : Node) : State Node :=
  { s with log := upd s.log r ((s.log (primaryOf s r)).take (catchUpPoint rf s (primaryOf s r))),
           acked := upd s.acked (primaryOf s r) (upd (s.acked (primaryOf s r)) r 0),
           commit := upd s.commit r (min (s.commit r) (s.commit (primaryOf s r))) }

/-- RestartEmpty(n): empty log, new incarnation, no volatile state; the current
primary forgets `n`'s ack and stops counting it. -/
def restartEmpty (s : State Node) (n : Node) : State Node :=
  { s with log := upd s.log n [],
           inc := upd s.inc n (s.inc n + 1),
           commit := upd s.commit n 0,
           recon := upd s.recon n 0,
           sync := fun p => if p = n then (fun _ => false)
                            else if p = s.cfg.prim then upd (s.sync p) n false else s.sync p,
           acked := fun p => if p = n then (fun _ => 0)
                             else if p = s.cfg.prim then upd (s.acked p) n 0 else s.acked p }

/-- The rebuilt sync set of a restarted primary (F5 fix):
`MatchElig(known[n]) \ {n}`. -/
def rebuiltSync (s : State Node) (n : Node) : Node → Bool :=
  fun r => (s.known n).elig r && decide ((s.known n).inc r = s.inc r) && decide (r ≠ n)

/-- RestartKeep(n): data and incarnation kept, volatile state lost. `n` must commit
its whole log again before it serves. If `n` is primary and `rb` (RebuildSync) is
on, its sync set is rebuilt; otherwise it is emptied. -/
def restartKeep (rb : Bool) (s : State Node) (n : Node) : State Node :=
  { s with commit := upd s.commit n 0,
           recon := upd s.recon n (s.log n).length,
           acked := upd s.acked n (fun _ => 0),
           sync := upd s.sync n
             (if rb = true ∧ (s.known n).prim = n ∧ (s.known n).inc n = s.inc n
              then rebuiltSync s n else fun _ => false) }

/-- `Next` of CoreRestart (replication part). `m` = MinSync, `rf` = ReconcileFirst,
`rb` = RebuildSync. Each constructor is one action: its arguments after the state
are the guard, and its result is the post-state. -/
inductive Step (m : Nat) (rf rb : Bool) : State Node → State Node → Prop
  /-- A node that thinks it is primary, has enough sync replicas and has reconciled
  may append a write. -/
  | append (s p) : ThinksPrimary s p → Writable m s p → Reconciled rf s p → Step m rf rb s (appendWrite s p)
  /-- A replica whose log is a strict prefix of its primary's may copy the next entry. -/
  | replicate (s r) : primaryOf s r ≠ r → s.log r <+: s.log (primaryOf s r) →
      (s.log r).length < (s.log (primaryOf s r)).length → Step m rf rb s (replicate s r)
  /-- A replica whose log is a prefix of its primary's may acknowledge new entries. -/
  | ack (s r) : primaryOf s r ≠ r → s.log r <+: s.log (primaryOf s r) →
      s.acked (primaryOf s r) r < (s.log r).length → Step m rf rb s (ack s r)
  /-- A writable primary may commit its next entry once every sync member has
  acknowledged it. -/
  | commit (s p) : ThinksPrimary s p → Writable m s p → s.commit p < (s.log p).length →
      (∀ r, s.sync p r = true → s.commit p < s.acked p r) → Step m rf rb s (commitNext s p)
  /-- A primary may count one of its replicas once that replica has acknowledged up
  to the catch-up point, unless it is eligible with a stale recorded incarnation. -/
  | count (s p r) : ThinksPrimary s p → r ≠ p → primaryOf s r = p →
      ((s.known p).elig r = true → (s.known p).inc r = s.inc r) → s.sync p r = false →
      catchUpPoint rf s p ≤ s.acked p r → Step m rf rb s (count s p r)
  /-- A primary that knows the latest configuration may make a sync member eligible. -/
  | makeElig (s p r) : ThinksPrimary s p → KnowsLatest s p → s.sync p r = true → s.cfg.elig r = false →
      Step m rf rb s (makeElig s p r)
  /-- A primary that knows the latest configuration may make a replica ineligible. -/
  | makeInelig (s p r) : ThinksPrimary s p → KnowsLatest s p → s.cfg.elig r = true →
      Step m rf rb s (makeInelig s p r)
  /-- A node with a stale configuration may learn the latest one. -/
  | learn (s n) : ¬ KnowsLatest s n → Step m rf rb s (learn s n)
  /-- An eligible replica that knows the latest configuration, with a matching
  recorded incarnation, may take over from the current primary. -/
  | takeover (s n) : KnowsLatest s n → s.cfg.elig n = true → n ≠ s.cfg.prim →
      s.cfg.inc n = s.inc n → Step m rf rb s (takeover s n)
  /-- A diverged replica may resync once its primary has neither counted it nor made
  it eligible. -/
  | resync (s r) : primaryOf s r ≠ r → ¬ (s.log r <+: s.log (primaryOf s r)) →
      s.sync (primaryOf s r) r = false → (s.known (primaryOf s r)).elig r = false →
      Step m rf rb s (resync rf s r)
  /-- A node that knows the latest configuration may restart empty while some other
  authoritative node keeps its data (the survival assumption). -/
  | restartEmpty (s n) : KnowsLatest s n → (∃ m, m ≠ n ∧ IsAuthoritative s m) → Step m rf rb s (restartEmpty s n)
  /-- A node that knows the latest configuration may restart and keep its data. -/
  | restartKeep (s n) : KnowsLatest s n → Step m rf rb s (restartKeep rb s n)

/-- The states reachable from `init i` by any finite sequence of steps. -/
inductive Reachable (m : Nat) (rf rb : Bool) (i : Node) : State Node → Prop
  | init : Reachable m rf rb i (init i)
  | step {s s'} : Reachable m rf rb i s → Step m rf rb s s' → Reachable m rf rb i s'

/-! ### Properties -/

/-- EligibleHoldsCommitted, restated for authoritative nodes: every authoritative
node holds the whole committed history, so losing any other node loses nothing
committed. -/
def EligibleHoldsCommitted (s : State Node) : Prop := ∀ n, IsAuthoritative s n → s.hist <+: s.log n

/-- CommittedAgree: every node's commit point is within its log, and what it has
committed is exactly the start of the committed history. In particular no two nodes
have committed different entries at the same offset. -/
def CommittedAgree (s : State Node) : Prop :=
  ∀ p, s.commit p ≤ (s.log p).length ∧ (s.log p).take (s.commit p) = s.hist.take (s.commit p)

/-- The property `RestartProof.safety` proves for every reachable state:
EligibleHoldsCommitted and CommittedAgree. -/
def Safety (s : State Node) : Prop := EligibleHoldsCommitted s ∧ CommittedAgree s

end RestartProof
