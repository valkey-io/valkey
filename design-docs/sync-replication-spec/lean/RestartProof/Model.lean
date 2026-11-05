/-! Transition-system encoding of the replication part of `spec/CoreRestart.tla`
(EarlyDrop = FALSE, IncCheck = TRUE, NoSurvivor = FALSE).

Same as `ReconProof.Model` plus:
- `inc n`: the node's incarnation; `cfg.inc r`: the incarnation the config records.
- ThinksPrim checks the recorded incarnation: `tp s p` is
  `known[p].prim = p /\ known[p].inc[p] = inc[p]`.
- `auth s n`: `n \in cfg.elig \cup {cfg.prim} /\ cfg.inc[n] = inc[n]` (Auth).
- Writable counts eligible sync members with a matching incarnation; Count does not
  count an eligible replica whose incarnation does not match; Takeover checks the new
  primary's incarnation and counts only the matching eligible replicas; MakeEligible
  records the replica's incarnation.
- RestartEmpty / RestartKeep (both require KnowsLatest, see the TLA+ header).
  `rb` = RebuildSync. RestartEmpty is guarded by the survival assumption
  `∃ m, m ≠ n ∧ auth s m`. Under it the TLA+ history cut never fires
  (EligibleHoldsCommitted), so here RestartEmpty leaves `hist` unchanged.

Sets of nodes are `Node → Bool`. No `MaxLen` / `MaxVer` / `MaxRestarts` bounds. -/

namespace RestartProof

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
  inc  : Node → Nat

structure State (Node : Type) [DecidableEq Node] where
  log    : Node → List Nat
  inc    : Node → Nat
  cfg    : Cfg Node
  known  : Node → Cfg Node
  sync   : Node → Node → Bool
  acked  : Node → Node → Nat
  commit : Node → Nat
  recon  : Node → Nat
  term   : Node → Nat
  hist   : List Nat

variable {Node : Type} [DecidableEq Node]

def cfg0 (i : Node) : Cfg Node := ⟨1, i, fun _ => false, fun _ => 1⟩

def init (i : Node) : State Node where
  log    := fun _ => []
  inc    := fun _ => 1
  cfg    := cfg0 i
  known  := fun _ => cfg0 i
  sync   := fun _ _ => false
  acked  := fun _ _ => 0
  commit := fun _ => 0
  recon  := fun _ => 0
  term   := fun n => if n = i then 1 else 0
  hist   := []

/-- ThinksPrim(p), with the incarnation check. -/
def tp (s : State Node) (p : Node) : Prop :=
  (s.known p).prim = p ∧ (s.known p).inc p = s.inc p
/-- KnowsLatest(n) -/
def kl (s : State Node) (n : Node) : Prop := (s.known n).ver = s.cfg.ver
/-- PrimOf(n) -/
def pof (s : State Node) (n : Node) : Node := (s.known n).prim
/-- Reconciled(p) -/
def reconciled (rf : Bool) (s : State Node) (p : Node) : Prop := rf = true → s.recon p ≤ s.commit p
/-- `n \in Auth`: the primary or an eligible replica, with a matching incarnation. -/
def auth (s : State Node) (n : Node) : Prop :=
  (s.cfg.elig n = true ∨ n = s.cfg.prim) ∧ s.cfg.inc n = s.inc n
/-- The current primary is authoritative (it has not come back empty). -/
def pa (s : State Node) : Prop := s.cfg.inc s.cfg.prim = s.inc s.cfg.prim

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

/-- MakeEligible's config: records `inc[r]`. -/
def eligCfg (s : State Node) (r : Node) : Cfg Node :=
  { s.cfg with ver := s.cfg.ver + 1, elig := upd s.cfg.elig r true,
               inc := upd s.cfg.inc r (s.inc r) }

def ineligCfg (s : State Node) (r : Node) : Cfg Node :=
  { s.cfg with ver := s.cfg.ver + 1, elig := upd s.cfg.elig r false }

def makeElig (s : State Node) (p r : Node) : State Node :=
  { s with cfg := eligCfg s r, known := upd s.known p (eligCfg s r) }

def makeInelig (s : State Node) (p r : Node) : State Node :=
  { s with cfg := ineligCfg s r, known := upd s.known p (ineligCfg s r),
           sync := upd s.sync p (upd (s.sync p) r false) }

def learn (s : State Node) (n : Node) : State Node :=
  { s with known := upd s.known n s.cfg,
           sync := upd s.sync n (if s.cfg.prim = n then s.sync n else fun _ => false) }

/-- `cfg.elig \ {cfg.prim, n}` -/
def newElig (s : State Node) (n : Node) : Node → Bool :=
  fun x => s.cfg.elig x && decide (x ≠ s.cfg.prim) && decide (x ≠ n)

def toCfg (s : State Node) (n : Node) : Cfg Node := ⟨s.cfg.ver + 1, n, newElig s n, s.cfg.inc⟩

/-- MatchElig of the new config: the matching eligible replicas. -/
def newSync (s : State Node) (n : Node) : Node → Bool :=
  fun x => newElig s n x && decide (s.cfg.inc x = s.inc x)

def takeover (s : State Node) (n : Node) : State Node :=
  { s with cfg := toCfg s n, known := upd s.known n (toCfg s n),
           sync := upd s.sync n (newSync s n),
           acked := upd s.acked n (fun _ => 0),
           commit := upd s.commit n 0,
           recon := upd s.recon n (s.log n).length,
           term := upd s.term n (s.cfg.ver + 1) }

def cup (rf : Bool) (s : State Node) (p : Node) : Nat :=
  if rf = true ∧ s.commit p < s.recon p then min (s.recon p) (s.log p).length else s.commit p

/-- Writable(p): `Cardinality(sync[p] \cap MatchElig(known[p])) >= m`. -/
def writable (m : Nat) (s : State Node) (p : Node) : Prop :=
  ∃ l : List Node, l.Nodup ∧ m ≤ l.length ∧
    ∀ r ∈ l, s.sync p r = true ∧ (s.known p).elig r = true ∧ (s.known p).inc r = s.inc r

def resync (rf : Bool) (s : State Node) (r : Node) : State Node :=
  { s with log := upd s.log r ((s.log (pof s r)).take (cup rf s (pof s r))),
           acked := upd s.acked (pof s r) (upd (s.acked (pof s r)) r 0),
           commit := upd s.commit r (min (s.commit r) (s.commit (pof s r))) }

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
def rebuilt (s : State Node) (n : Node) : Node → Bool :=
  fun r => (s.known n).elig r && decide ((s.known n).inc r = s.inc r) && decide (r ≠ n)

/-- RestartKeep(n): data and incarnation kept, volatile state lost. `rb` = RebuildSync. -/
def restartKeep (rb : Bool) (s : State Node) (n : Node) : State Node :=
  { s with commit := upd s.commit n 0,
           recon := upd s.recon n (s.log n).length,
           acked := upd s.acked n (fun _ => 0),
           sync := upd s.sync n
             (if rb = true ∧ (s.known n).prim = n ∧ (s.known n).inc n = s.inc n
              then rebuilt s n else fun _ => false) }

/-- `Next` of CoreRestart (replication part). `m` = MinSync, `rf` = ReconcileFirst,
`rb` = RebuildSync. -/
inductive Step (m : Nat) (rf rb : Bool) : State Node → State Node → Prop
  | append (s p) : tp s p → writable m s p → reconciled rf s p → Step m rf rb s (appendWrite s p)
  | replicate (s r) : pof s r ≠ r → s.log r <+: s.log (pof s r) →
      (s.log r).length < (s.log (pof s r)).length → Step m rf rb s (replicate s r)
  | ack (s r) : pof s r ≠ r → s.log r <+: s.log (pof s r) →
      s.acked (pof s r) r < (s.log r).length → Step m rf rb s (ack s r)
  | commit (s p) : tp s p → writable m s p → s.commit p < (s.log p).length →
      (∀ r, s.sync p r = true → s.commit p < s.acked p r) → Step m rf rb s (commitA s p)
  | count (s p r) : tp s p → r ≠ p → pof s r = p →
      ((s.known p).elig r = true → (s.known p).inc r = s.inc r) → s.sync p r = false →
      cup rf s p ≤ s.acked p r → Step m rf rb s (count s p r)
  | makeElig (s p r) : tp s p → kl s p → s.sync p r = true → s.cfg.elig r = false →
      Step m rf rb s (makeElig s p r)
  | makeInelig (s p r) : tp s p → kl s p → s.cfg.elig r = true →
      Step m rf rb s (makeInelig s p r)
  | learn (s n) : ¬ kl s n → Step m rf rb s (learn s n)
  | takeover (s n) : kl s n → s.cfg.elig n = true → n ≠ s.cfg.prim →
      s.cfg.inc n = s.inc n → Step m rf rb s (takeover s n)
  | resync (s r) : pof s r ≠ r → ¬ (s.log r <+: s.log (pof s r)) →
      s.sync (pof s r) r = false → (s.known (pof s r)).elig r = false →
      Step m rf rb s (resync rf s r)
  | restartEmpty (s n) : kl s n → (∃ m, m ≠ n ∧ auth s m) → Step m rf rb s (restartEmpty s n)
  | restartKeep (s n) : kl s n → Step m rf rb s (restartKeep rb s n)

inductive Reachable (m : Nat) (rf rb : Bool) (i : Node) : State Node → Prop
  | init : Reachable m rf rb i (init i)
  | step {s s'} : Reachable m rf rb i s → Step m rf rb s s' → Reachable m rf rb i s'

/-! ### Properties -/

/-- EligibleHoldsCommitted, restated for authoritative nodes. -/
def EHC (s : State Node) : Prop := ∀ n, auth s n → s.hist <+: s.log n

/-- CommittedAgree -/
def CA (s : State Node) : Prop :=
  ∀ p, s.commit p ≤ (s.log p).length ∧ (s.log p).take (s.commit p) = s.hist.take (s.commit p)

def Safety (s : State Node) : Prop := EHC s ∧ CA s

end RestartProof
