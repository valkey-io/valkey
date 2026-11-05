/-! Transition-system encoding of the replication part of `spec/CoreRecon.tla`
(StrictRecon = FALSE, EarlyDrop = FALSE).

Same state and post-states as `CoreProof` except:
- `m` = MinSync: `writable m s p` is `Cardinality(sync[p] \cap known[p].elig) >= m`,
  stated without `Fintype` as "some duplicate-free list of `m` such nodes exists".
  It guards AppendWrite and Commit.
- Count and Resync use `cup` (CatchUpPoint) instead of the Reconciled gate:
  `cup = commit[p]` once reconciled, else `min(recon[p], Len(log[p]))`.
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

def cfg0 (i : Node) : Cfg Node := ⟨1, i, fun _ => false⟩

def init (i : Node) : State Node where
  log    := fun _ => []
  cfg    := cfg0 i
  known  := fun _ => cfg0 i
  sync   := fun _ _ => false
  acked  := fun _ _ => 0
  commit := fun _ => 0
  recon  := fun _ => 0
  term   := fun n => if n = i then 1 else 0
  hist   := []

/-- ThinksPrim(p) -/
def tp (s : State Node) (p : Node) : Prop := (s.known p).prim = p
/-- KnowsLatest(n) -/
def kl (s : State Node) (n : Node) : Prop := (s.known n).ver = s.cfg.ver
/-- PrimOf(n) -/
def pof (s : State Node) (n : Node) : Node := (s.known n).prim
/-- Reconciled(p) -/
def reconciled (rf : Bool) (s : State Node) (p : Node) : Prop := rf = true → s.recon p ≤ s.commit p

/-! ### Post-states of the actions (the UNCHANGED parts are implicit). -/

def appendWrite (s : State Node) (p : Node) : State Node :=
  { s with log := upd s.log p (s.log p ++ [s.term p]) }

def replicate (s : State Node) (r : Node) : State Node :=
  { s with log := upd s.log r (s.log r ++ [(s.log (pof s r)).getD (s.log r).length 0]) }

def ack (s : State Node) (r : Node) : State Node :=
  { s with acked := upd s.acked (pof s r) (upd (s.acked (pof s r)) r (s.log r).length) }

def commitA (s : State Node) (p : Node) : State Node :=
  { s with commit := upd s.commit p (s.commit p + 1),
           hist := if s.commit p = s.hist.length
                   then s.hist ++ [(s.log p).getD (s.commit p) 0] else s.hist }

def count (s : State Node) (p r : Node) : State Node :=
  { s with sync := upd s.sync p (upd (s.sync p) r true) }

def eligCfg (s : State Node) (r : Node) (b : Bool) : Cfg Node :=
  { s.cfg with ver := s.cfg.ver + 1, elig := upd s.cfg.elig r b }

def makeElig (s : State Node) (p r : Node) : State Node :=
  { s with cfg := eligCfg s r true, known := upd s.known p (eligCfg s r true) }

def makeInelig (s : State Node) (p r : Node) : State Node :=
  { s with cfg := eligCfg s r false, known := upd s.known p (eligCfg s r false),
           sync := upd s.sync p (upd (s.sync p) r false) }

def learn (s : State Node) (n : Node) : State Node :=
  { s with known := upd s.known n s.cfg,
           sync := upd s.sync n (if s.cfg.prim = n then s.sync n else fun _ => false) }

/-- `cfg.elig \ {cfg.prim, n}` -/
def newElig (s : State Node) (n : Node) : Node → Bool :=
  fun x => s.cfg.elig x && decide (x ≠ s.cfg.prim) && decide (x ≠ n)

def toCfg (s : State Node) (n : Node) : Cfg Node := ⟨s.cfg.ver + 1, n, newElig s n⟩

def takeover (s : State Node) (n : Node) : State Node :=
  { s with cfg := toCfg s n, known := upd s.known n (toCfg s n),
           sync := upd s.sync n (newElig s n),
           acked := upd s.acked n (fun _ => 0),
           commit := upd s.commit n 0,
           recon := upd s.recon n (s.log n).length,
           term := upd s.term n (s.cfg.ver + 1) }

/-- CatchUpPoint(p): `commit[p]` once reconciled (or with `rf = false`, no
reconciliation at all), else the inherited log `min(recon[p], Len(log[p]))`. -/
def cup (rf : Bool) (s : State Node) (p : Node) : Nat :=
  if rf = true ∧ s.commit p < s.recon p then min (s.recon p) (s.log p).length else s.commit p

/-- Writable(p): `Cardinality(sync[p] \cap known[p].elig) >= m`. -/
def writable (m : Nat) (s : State Node) (p : Node) : Prop :=
  ∃ l : List Node, l.Nodup ∧ m ≤ l.length ∧
    ∀ r ∈ l, s.sync p r = true ∧ (s.known p).elig r = true

def resync (rf : Bool) (s : State Node) (r : Node) : State Node :=
  { s with log := upd s.log r ((s.log (pof s r)).take (cup rf s (pof s r))),
           acked := upd s.acked (pof s r) (upd (s.acked (pof s r)) r 0),
           commit := upd s.commit r (min (s.commit r) (s.commit (pof s r))) }

/-- `Next` of CoreRecon (replication part), one constructor per action.
`m` = MinSync, `rf` = ReconcileFirst. -/
inductive Step (m : Nat) (rf : Bool) : State Node → State Node → Prop
  | append (s p) : tp s p → writable m s p → reconciled rf s p → Step m rf s (appendWrite s p)
  | replicate (s r) : pof s r ≠ r → s.log r <+: s.log (pof s r) →
      (s.log r).length < (s.log (pof s r)).length → Step m rf s (replicate s r)
  | ack (s r) : pof s r ≠ r → s.log r <+: s.log (pof s r) →
      s.acked (pof s r) r < (s.log r).length → Step m rf s (ack s r)
  | commit (s p) : tp s p → writable m s p → s.commit p < (s.log p).length →
      (∀ r, s.sync p r = true → s.commit p < s.acked p r) → Step m rf s (commitA s p)
  | count (s p r) : tp s p → r ≠ p → pof s r = p → s.sync p r = false →
      cup rf s p ≤ s.acked p r → Step m rf s (count s p r)
  | makeElig (s p r) : tp s p → kl s p → s.sync p r = true → s.cfg.elig r = false →
      Step m rf s (makeElig s p r)
  | makeInelig (s p r) : tp s p → kl s p → s.cfg.elig r = true →
      Step m rf s (makeInelig s p r)
  | learn (s n) : ¬ kl s n → Step m rf s (learn s n)
  | takeover (s n) : kl s n → s.cfg.elig n = true → n ≠ s.cfg.prim →
      Step m rf s (takeover s n)
  | resync (s r) : pof s r ≠ r → ¬ (s.log r <+: s.log (pof s r)) →
      s.sync (pof s r) r = false → (s.known (pof s r)).elig r = false →
      Step m rf s (resync rf s r)

inductive Reachable (m : Nat) (rf : Bool) (i : Node) : State Node → Prop
  | init : Reachable m rf i (init i)
  | step {s s'} : Reachable m rf i s → Step m rf s s' → Reachable m rf i s'

/-! ### Properties -/

/-- EligibleHoldsCommitted -/
def EHC (s : State Node) : Prop :=
  ∀ n, (s.cfg.elig n = true ∨ n = s.cfg.prim) → s.hist <+: s.log n

/-- CommittedAgree; `Prefix(s, k)` of Core.tla is `List.take k s`. -/
def CA (s : State Node) : Prop :=
  ∀ p, s.commit p ≤ (s.log p).length ∧ (s.log p).take (s.commit p) = s.hist.take (s.commit p)

def Safety (s : State Node) : Prop := EHC s ∧ CA s

end ReconProof
