import RestartProof.Lists

/-! Transition-system encoding of the replication part of `spec/CoreRecon.tla`
(StrictRecon = FALSE, EarlyDrop = FALSE).

Same state and post-states as `CoreProof` except:
- `m` = MinSync: `Writable m s p` is `Cardinality(sync[p] \cap known[p].elig) >= m`,
  stated without `Fintype` as "some duplicate-free list of `m` such nodes exists".
  It guards AppendWrite and Commit.
- Count and Resync use `catchUpPoint` (CatchUpPoint) instead of the Reconciled gate:
  `catchUpPoint = commit[p]` once reconciled, else `min(recon[p], Len(log[p]))`.
- AppendWrite still needs Reconciled (serve only once reconciled).
- There is no EarlyUncount (EarlyDrop = FALSE).

Sets of nodes are `Node → Bool`; functions are updated with `upd`.
There are no `MaxLen` / `MaxVer` bounds (they only bound model checking):
`Len(log[p]) < MaxLen` and `CanCas` are dropped. -/

namespace ReconProof

def upd {α β : Type} [DecidableEq α] (f : α → β) (a : α) (b : β) : α → β :=
  fun x => if x = a then b else f x

@[simp] theorem upd_same {α β : Type} [DecidableEq α] (f : α → β) (a : α) (b : β) :
    upd f a b a = b := by simp [upd]

theorem upd_ne {α β : Type} [DecidableEq α] (f : α → β) {a x : α} (b : β) (h : x ≠ a) :
    upd f a b x = f x := by simp [upd, h]

structure Cfg (Node : Type) where
  ver  : Nat
  prim : Node
  elig : Node → Bool

structure State (Node : Type) [DecidableEq Node] where
  log    : Node → List Nat
  cfg    : Cfg Node
  known  : Node → Cfg Node
  sync   : Node → Node → Bool
  acked  : Node → Node → Nat
  commit : Node → Nat
  recon  : Node → Nat
  term   : Node → Nat
  hist   : List Nat

variable {Node : Type} [DecidableEq Node]

def initCfg (i : Node) : Cfg Node := ⟨1, i, fun _ => false⟩

def init (i : Node) : State Node where
  log    := fun _ => []
  cfg    := initCfg i
  known  := fun _ => initCfg i
  sync   := fun _ _ => false
  acked  := fun _ _ => 0
  commit := fun _ => 0
  recon  := fun _ => 0
  term   := fun n => if n = i then 1 else 0
  hist   := []

/-- ThinksPrim(p) -/
def ThinksPrimary (s : State Node) (p : Node) : Prop := (s.known p).prim = p
/-- KnowsLatest(n) -/
def KnowsLatest (s : State Node) (n : Node) : Prop := (s.known n).ver = s.cfg.ver
/-- PrimOf(n) -/
def primaryOf (s : State Node) (n : Node) : Node := (s.known n).prim
/-- Reconciled(p) -/
def Reconciled (rf : Bool) (s : State Node) (p : Node) : Prop := rf = true → s.recon p ≤ s.commit p

/-! ### Post-states of the actions (the UNCHANGED parts are implicit). -/

def appendWrite (s : State Node) (p : Node) : State Node :=
  { s with log := upd s.log p (s.log p ++ [s.term p]) }

def replicate (s : State Node) (r : Node) : State Node :=
  { s with log := upd s.log r (s.log r ++ [(s.log (primaryOf s r)).getD (s.log r).length 0]) }

def ack (s : State Node) (r : Node) : State Node :=
  { s with acked := upd s.acked (primaryOf s r) (upd (s.acked (primaryOf s r)) r (s.log r).length) }

def commitNext (s : State Node) (p : Node) : State Node :=
  { s with commit := upd s.commit p (s.commit p + 1),
           hist := if s.commit p = s.hist.length
                   then s.hist ++ [(s.log p).getD (s.commit p) 0] else s.hist }

def count (s : State Node) (p r : Node) : State Node :=
  { s with sync := upd s.sync p (upd (s.sync p) r true) }

def makeEligCfg (s : State Node) (r : Node) (b : Bool) : Cfg Node :=
  { s.cfg with ver := s.cfg.ver + 1, elig := upd s.cfg.elig r b }

def makeElig (s : State Node) (p r : Node) : State Node :=
  { s with cfg := makeEligCfg s r true, known := upd s.known p (makeEligCfg s r true) }

def makeInelig (s : State Node) (p r : Node) : State Node :=
  { s with cfg := makeEligCfg s r false, known := upd s.known p (makeEligCfg s r false),
           sync := upd s.sync p (upd (s.sync p) r false) }

def learn (s : State Node) (n : Node) : State Node :=
  { s with known := upd s.known n s.cfg,
           sync := upd s.sync n (if s.cfg.prim = n then s.sync n else fun _ => false) }

/-- `cfg.elig \ {cfg.prim, n}` -/
def takeoverElig (s : State Node) (n : Node) : Node → Bool :=
  fun x => s.cfg.elig x && decide (x ≠ s.cfg.prim) && decide (x ≠ n)

def takeoverCfg (s : State Node) (n : Node) : Cfg Node := ⟨s.cfg.ver + 1, n, takeoverElig s n⟩

def takeover (s : State Node) (n : Node) : State Node :=
  { s with cfg := takeoverCfg s n, known := upd s.known n (takeoverCfg s n),
           sync := upd s.sync n (takeoverElig s n),
           acked := upd s.acked n (fun _ => 0),
           commit := upd s.commit n 0,
           recon := upd s.recon n (s.log n).length,
           term := upd s.term n (s.cfg.ver + 1) }

/-- CatchUpPoint(p): `commit[p]` once reconciled (or with `rf = false`, no
reconciliation at all), else the inherited log `min(recon[p], Len(log[p]))`. -/
def catchUpPoint (rf : Bool) (s : State Node) (p : Node) : Nat :=
  if rf = true ∧ s.commit p < s.recon p then min (s.recon p) (s.log p).length else s.commit p

/-- `sync[p] \cap known[p].elig`: the sync members of `p` that are eligible in `p`'s
configuration. -/
def syncEligible (s : State Node) (p : Node) : Set Node :=
  {r | s.sync p r = true ∧ (s.known p).elig r = true}

/-- Writable(p): `Cardinality(sync[p] \cap known[p].elig) >= m` (`Set.encard`, the
cardinality in `ℕ∞`; no finiteness assumption on `Node`). -/
def Writable (m : Nat) (s : State Node) (p : Node) : Prop :=
  (m : ℕ∞) ≤ (syncEligible s p).encard

/-- `Writable` is the earlier list form: some duplicate-free list of at least `m`
nodes, all counted. -/
theorem writable_iff_list (m : Nat) (s : State Node) (p : Node) :
    Writable m s p ↔ ∃ l : List Node, l.Nodup ∧ m ≤ l.length ∧
      ∀ r ∈ l, s.sync p r = true ∧ (s.known p).elig r = true :=
  RestartProof.le_encard_iff_list m _

def resync (rf : Bool) (s : State Node) (r : Node) : State Node :=
  { s with log := upd s.log r ((s.log (primaryOf s r)).take (catchUpPoint rf s (primaryOf s r))),
           acked := upd s.acked (primaryOf s r) (upd (s.acked (primaryOf s r)) r 0),
           commit := upd s.commit r (min (s.commit r) (s.commit (primaryOf s r))) }

/-- `Next` of CoreRecon (replication part), one constructor per action.
`m` = MinSync, `rf` = ReconcileFirst. -/
inductive Step (m : Nat) (rf : Bool) : State Node → State Node → Prop
  | append (s p) : ThinksPrimary s p → Writable m s p → Reconciled rf s p → Step m rf s (appendWrite s p)
  | replicate (s r) : primaryOf s r ≠ r → s.log r <+: s.log (primaryOf s r) →
      (s.log r).length < (s.log (primaryOf s r)).length → Step m rf s (replicate s r)
  | ack (s r) : primaryOf s r ≠ r → s.log r <+: s.log (primaryOf s r) →
      s.acked (primaryOf s r) r < (s.log r).length → Step m rf s (ack s r)
  | commit (s p) : ThinksPrimary s p → Writable m s p → s.commit p < (s.log p).length →
      (∀ r, s.sync p r = true → s.commit p < s.acked p r) → Step m rf s (commitNext s p)
  | count (s p r) : ThinksPrimary s p → r ≠ p → primaryOf s r = p → s.sync p r = false →
      catchUpPoint rf s p ≤ s.acked p r → Step m rf s (count s p r)
  | makeElig (s p r) : ThinksPrimary s p → KnowsLatest s p → s.sync p r = true → s.cfg.elig r = false →
      Step m rf s (makeElig s p r)
  | makeInelig (s p r) : ThinksPrimary s p → KnowsLatest s p → s.cfg.elig r = true →
      Step m rf s (makeInelig s p r)
  | learn (s n) : ¬ KnowsLatest s n → Step m rf s (learn s n)
  | takeover (s n) : KnowsLatest s n → s.cfg.elig n = true → n ≠ s.cfg.prim →
      Step m rf s (takeover s n)
  | resync (s r) : primaryOf s r ≠ r → ¬ (s.log r <+: s.log (primaryOf s r)) →
      s.sync (primaryOf s r) r = false → (s.known (primaryOf s r)).elig r = false →
      Step m rf s (resync rf s r)

inductive Reachable (m : Nat) (rf : Bool) (i : Node) : State Node → Prop
  | init : Reachable m rf i (init i)
  | step {s s'} : Reachable m rf i s → Step m rf s s' → Reachable m rf i s'

/-! ### Properties -/

/-- EligibleHoldsCommitted -/
def EligibleHoldsCommitted (s : State Node) : Prop :=
  ∀ n, (s.cfg.elig n = true ∨ n = s.cfg.prim) → s.hist <+: s.log n

/-- CommittedAgree; `Prefix(s, k)` of Core.tla is `List.take k s`. -/
def CommittedAgree (s : State Node) : Prop :=
  ∀ p, s.commit p ≤ (s.log p).length ∧ (s.log p).take (s.commit p) = s.hist.take (s.commit p)

def Safety (s : State Node) : Prop := EligibleHoldsCommitted s ∧ CommittedAgree s

end ReconProof
