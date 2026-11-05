import XCheck.Data
import AckProof.Proof

/-! Executable successor relation for `AckProof.AStep true false false` on `Fin 3`, with
soundness (`asuccs_sound`, `astepLB_sound`), a decidable `RestartProof.AInv` of the
lifted state (`ainvB_sound`), and
the decode / render pair for TLC's dump of `spec/CoreAck_xcheck.cfg`.
Used by `XCheck.AckMain` (`lake exe xcheck_ack`).

`held` and `okd` are lists in Lean and sets in TLA+. States are compared after `cn`,
which sorts both lists and removes duplicates; `AckMain` reports how many Lean
successors had a duplicate before `cn`. -/

namespace XCheck
open CoreProof AckProof

abbrev A := AState N

instance : Inhabited A := ⟨ainit 0⟩

/-! ### Equality and canonical form -/

instance instDecEqA : DecidableEq A := fun a b =>
  if h : a.core = b.core ∧ a.held = b.held ∧ a.okd = b.okd then
    isTrue (by cases a; cases b; simp_all)
  else isFalse (fun e => h (by subst e; exact ⟨rfl, rfl, rfl⟩))

def keyH (h : Held N) : List Nat := [h.1.val, h.2.1, h.2.2]
def keyO (x : Nat × Nat) : List Nat := [x.1, x.2]

def lexLe : List Nat → List Nat → Bool
  | [], _ => true
  | _ :: _, [] => false
  | x :: xs, y :: ys => x < y || (x == y && lexLe xs ys)

/-- Sorted (by `key`, lexicographic) and without duplicates. TLC prints sets in this
order for the values in this dump (checked by the render test). -/
def canon {α : Type} [DecidableEq α] (key : α → List Nat) (l : List α) : List α :=
  (l.eraseDups).mergeSort (fun a b => lexLe (key a) (key b))

def cn (a : A) : A := ⟨a.core, canon keyH a.held, canon keyO a.okd⟩

def hasDup {α : Type} [DecidableEq α] (l : List α) : Bool := l.eraseDups.length != l.length

/-! ### Successors -/

instance (s : S) (h : Held N) : Decidable (fenced s h) := by unfold fenced; infer_instance

/-- Lean's label for a Core step inside `AStep.core`: TLC's CoreAck wrappers are named
`C<Action>`. `AppendWrite` keeps its name: CoreAck.tla has no reply-less append, so a
Lean successor labelled `AppendWrite` is Lean-only behaviour (reported separately). -/
def coreLabel (l : String) : String :=
  if l.startsWith "AppendWrite" then l else "C" ++ l

def oWrite (a : A) (p : N) : Option A :=
  if ThinksPrimary a.core p ∧ Reconciled true a.core p then some (write a p) else none

def releases (a : A) : List (String × A) :=
  a.held.filterMap fun h =>
    if h.2.1 ≤ a.core.commit h.1 ∧ fenced a.core h then some ("ReleaseSome", release a h) else none

def aborts (a : A) : List (String × A) :=
  a.held.filterMap fun h =>
    if ¬ fenced a.core h then some ("AbortSome", abort a h) else none

def asuccs (a : A) : List (String × A) :=
  (succs a.core).map (fun x => (coreLabel x.1, (⟨x.2, a.held, a.okd⟩ : A))) ++
  nodes.filterMap (fun p => (oWrite a p).map (lbl1 "Write" p, ·)) ++
  releases a ++ aborts a

theorem asuccs_sound {a t : A} {l : String} (h : (l, t) ∈ asuccs a) :
    AStep true false false a t := by
  simp only [asuccs, List.mem_append] at h
  rcases h with ((h | h) | h) | h
  · simp only [List.mem_map] at h
    obtain ⟨⟨l', c⟩, hm, he⟩ := h
    simp only [Prod.mk.injEq] at he
    rw [← he.2]; exact AStep.core a c (succs_sound hm)
  · simp only [List.mem_filterMap] at h
    obtain ⟨p, _, he⟩ := h
    unfold oWrite at he; split at he
    · next g =>
      simp only [Option.map_some, Option.some.injEq, Prod.mk.injEq] at he
      rw [← he.2]; exact AStep.write a p g.1 g.2
    · cases he
  · simp only [releases, List.mem_filterMap] at h
    obtain ⟨x, hm, he⟩ := h
    split at he
    · next g =>
      simp only [Option.some.injEq, Prod.mk.injEq] at he
      rw [← he.2]; exact AStep.release a x hm g.1 (fun _ => g.2)
    · cases he
  · simp only [aborts, List.mem_filterMap] at h
    obtain ⟨x, hm, he⟩ := h
    split at he
    · next g =>
      simp only [Option.some.injEq, Prod.mk.injEq] at he
      rw [← he.2]; exact AStep.abort a x hm g
    · cases he

/-- Some step labelled `l` leads from `a` to a state whose canonical form is `t`. -/
def astepLB (a : A) (l : String) (t : A) : Bool :=
  (asuccs a).any (fun x => x.1 == l && decide (cn x.2 = t))

theorem astepLB_sound {a t : A} {l : String} (h : astepLB a l t = true) :
    ∃ u, AStep true false false a u ∧ cn u = t := by
  simp only [astepLB, List.any_eq_true, Bool.and_eq_true, decide_eq_true_eq] at h
  obtain ⟨⟨l', u⟩, hm, _, he⟩ := h
  exact ⟨u, asuccs_sound hm, he⟩

/-! ### `RestartProof.AInv` and `AckedHeld` as decidable checks -/

/-- The CoreRestart state with held replies that a CoreAck state stands for. -/
def alift (a : A) : RestartProof.AState N := ReconProof.aemb (atoRecon a)

theorem alift_core (a : A) : (alift a).core = lift a.core := rfl

instance (s : R) : Decidable (RestartProof.TermLe s) := by
  unfold RestartProof.TermLe; infer_instance
instance (s : R) (h : Held N) : Decidable (RestartProof.fenced s h) := by
  unfold RestartProof.fenced; infer_instance
instance (s : R) (l : List (Held N)) : Decidable (RestartProof.HeldInv s l) := by
  unfold RestartProof.HeldInv; infer_instance
instance (a : RestartProof.AState N) : Decidable (RestartProof.AckedDurable a) := by
  unfold RestartProof.AckedDurable; infer_instance
instance (a : A) : Decidable (AckedHeld a) := by unfold AckedHeld; infer_instance

/-- `RestartProof.AInv` (the invariant `ackSafety` is derived from) on the lifted state. -/
def ainvB (a : A) : Bool :=
  invB a.core && decide (RestartProof.TermLe (alift a).core) &&
  decide (RestartProof.HeldInv (alift a).core (alift a).held) &&
  decide (RestartProof.AckedDurable (alift a))

theorem ainvB_sound {a : A} (h : ainvB a = true) : RestartProof.AInv (alift a) := by
  simp only [ainvB, Bool.and_eq_true, decide_eq_true_eq] at h
  exact ⟨invB_sound h.1.1.1, h.1.1.2, h.1.2, h.2⟩

def ackedHeldB (a : A) : Bool := decide (AckedHeld a)

theorem ackedHeldB_sound {a : A} (h : ackedHeldB a = true) : AckedHeld a :=
  of_decide_eq_true h

/-! ### Decoding (layout of `xcheck/dot2lean.py --ack`) -/

def takeHeld (l : List Nat) : Option (Held N × List Nat) := do
  let (n, l) ← take1 l
  let (o, l) ← take1 l
  let (t, l) ← take1 l
  let n ← toNode n
  pure ((n, o, t), l)

def takePair (l : List Nat) : Option ((Nat × Nat) × List Nat) := do
  let (o, l) ← take1 l
  let (v, l) ← take1 l
  pure ((o, v), l)

def takeList {α : Type} (f : List Nat → Option (α × List Nat)) :
    Nat → List Nat → Option (List α × List Nat)
  | 0, l => some ([], l)
  | k + 1, l => do
    let (x, l) ← f l
    let (xs, l) ← takeList f k l
    pure (x :: xs, l)

def decodeA (l : List Nat) : Option A := do
  let (log, l) ← take3 takeSeq l
  let (cfg, l) ← takeCfg l
  let (known, l) ← take3 takeCfg l
  let (sync, l) ← take3 takeMask l
  let (acked, l) ← take3 (take3 take1) l
  let (commit, l) ← take3 take1 l
  let (recon, l) ← take3 take1 l
  let (term, l) ← take3 take1 l
  let (hist, l) ← takeSeq l
  let (k, l) ← take1 l
  let (held, l) ← takeList takeHeld k l
  let (k, l) ← take1 l
  let (okd, l) ← takeList takePair k l
  if l.isEmpty then
    pure ⟨{ log, cfg, known, sync, acked, commit, recon, term, hist }, held, okd⟩
  else none

def encodeA (a : A) : List Nat :=
  encode a.core ++ (a.held.length :: a.held.flatMap keyH) ++ (a.okd.length :: a.okd.flatMap keyO)

/-! ### TLC syntax, variables in alphabetical order, records as TLC prints them here -/

def rCfgA (c : Cfg N) : String :=
  "[elig|->" ++ rSet c.elig ++ ",prim|->" ++ nodeName c.prim ++ ",ver|->" ++ toString c.ver ++ "]"
def rHeld (h : Held N) : String :=
  "[n|->" ++ nodeName h.1 ++ ",o|->" ++ toString h.2.1 ++ ",t|->" ++ toString h.2.2 ++ "]"
def rPair (x : Nat × Nat) : String := "<<" ++ toString x.1 ++ "," ++ toString x.2 ++ ">>"
def rList {α : Type} (r : α → String) (l : List α) : String := "{" ++ sep (l.map r) "," ++ "}"

def renderA (a : A) : String :=
  let s := a.core
  "/\\acked=" ++ rFn (rFn toString) s.acked ++ "/\\cfg=" ++ rCfgA s.cfg ++
  "/\\commit=" ++ rFn toString s.commit ++ "/\\held=" ++ rList rHeld a.held ++
  "/\\hist=" ++ rSeq s.hist ++ "/\\known=" ++ rFn rCfgA s.known ++
  "/\\log=" ++ rFn rSeq s.log ++ "/\\okd=" ++ rList rPair a.okd ++
  "/\\recon=" ++ rFn toString s.recon ++ "/\\sync=" ++ rFn rSet s.sync ++
  "/\\term=" ++ rFn toString s.term

def diffFieldsA (a b : A) : List String :=
  diffFields a.core b.core ++ (if a.held = b.held then [] else ["held"]) ++
  (if a.okd = b.okd then [] else ["okd"])

end XCheck
