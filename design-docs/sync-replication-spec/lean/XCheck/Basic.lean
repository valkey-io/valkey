import CoreProof.Embed
import ReconProof.Embed
import RestartProof.Inv

/-! Executable step relation and invariant check for `CoreProof` on `Fin 3`,
with soundness proofs: `stepB s s' = true → Step true false s s'` and
`invB s = true → RestartProof.Inv (lift s)`, where `lift` embeds a Core state into
CoreRestart, whose invariant proof all Core results are derived from. Used by
`XCheck.Main` to cross-check TLC's state graph. -/

namespace XCheck
open CoreProof

abbrev N := Fin 3
abbrev S := State N

/-! ### Decidable equality for states over `Fin 3` -/

instance instDecEqFin3Fun {α : Type} [DecidableEq α] : DecidableEq (Fin 3 → α) := fun f g =>
  if h : f 0 = g 0 ∧ f 1 = g 1 ∧ f 2 = g 2 then
    isTrue (funext fun i => match i with
      | ⟨0, _⟩ => h.1 | ⟨1, _⟩ => h.2.1 | ⟨2, _⟩ => h.2.2)
  else isFalse (fun e => h (by subst e; exact ⟨rfl, rfl, rfl⟩))

instance instDecEqCfg : DecidableEq (Cfg N) := fun a b =>
  if h : a.ver = b.ver ∧ a.prim = b.prim ∧ a.elig = b.elig then
    isTrue (by cases a; cases b; simp_all)
  else isFalse (fun e => h (by subst e; exact ⟨rfl, rfl, rfl⟩))

instance instDecEqState : DecidableEq S := fun a b =>
  if h : a.log = b.log ∧ a.cfg = b.cfg ∧ a.known = b.known ∧ a.sync = b.sync ∧
      a.acked = b.acked ∧ a.commit = b.commit ∧ a.recon = b.recon ∧ a.term = b.term ∧
      a.hist = b.hist then
    isTrue (by cases a; cases b; simp_all)
  else isFalse (fun e => h (by subst e; exact ⟨rfl, rfl, rfl, rfl, rfl, rfl, rfl, rfl, rfl⟩))

/-! ### Decidability of the guards -/

instance (s : S) (p : N) : Decidable (ThinksPrimary s p) := inferInstanceAs (Decidable (_ = _))
instance (s : S) (p : N) : Decidable (KnowsLatest s p) := inferInstanceAs (Decidable (_ = _))
instance (rf : Bool) (s : S) (p : N) : Decidable (Reconciled rf s p) :=
  inferInstanceAs (Decidable (_ → _))

/-! ### One partial successor function per `Step` constructor (rf = true, ed = false) -/

def oAppend (s : S) (p : N) : Option S :=
  if ThinksPrimary s p ∧ Reconciled true s p then some (appendWrite s p) else none
def oReplicate (s : S) (r : N) : Option S :=
  if primaryOf s r ≠ r ∧ s.log r <+: s.log (primaryOf s r) ∧ (s.log r).length < (s.log (primaryOf s r)).length
  then some (replicate s r) else none
def oAck (s : S) (r : N) : Option S :=
  if primaryOf s r ≠ r ∧ s.log r <+: s.log (primaryOf s r) ∧ s.acked (primaryOf s r) r < (s.log r).length
  then some (ack s r) else none
def oCommit (s : S) (p : N) : Option S :=
  if ThinksPrimary s p ∧ s.commit p < (s.log p).length ∧ (∀ r, s.sync p r = true → s.commit p < s.acked p r)
  then some (commitNext s p) else none
def oCount (s : S) (p r : N) : Option S :=
  if ThinksPrimary s p ∧ Reconciled true s p ∧ r ≠ p ∧ primaryOf s r = p ∧ s.sync p r = false ∧
     s.commit p ≤ s.acked p r
  then some (count s p r) else none
def oMakeElig (s : S) (p r : N) : Option S :=
  if ThinksPrimary s p ∧ KnowsLatest s p ∧ s.sync p r = true ∧ s.cfg.elig r = false
  then some (makeElig s p r) else none
def oMakeInelig (s : S) (p r : N) : Option S :=
  if ThinksPrimary s p ∧ KnowsLatest s p ∧ s.cfg.elig r = true then some (makeInelig s p r) else none
def oEarlyUncount (s : S) (p r : N) : Option S :=
  if (false = true) ∧ ThinksPrimary s p ∧ s.sync p r = true then some (earlyUncount s p r) else none
def oLearn (s : S) (n : N) : Option S :=
  if ¬ KnowsLatest s n then some (learn s n) else none
def oTakeover (s : S) (n : N) : Option S :=
  if KnowsLatest s n ∧ s.cfg.elig n = true ∧ n ≠ s.cfg.prim then some (takeover s n) else none
def oResync (s : S) (r : N) : Option S :=
  if primaryOf s r ≠ r ∧ ¬ (s.log r <+: s.log (primaryOf s r)) ∧ s.sync (primaryOf s r) r = false ∧
     (s.known (primaryOf s r)).elig r = false ∧ Reconciled true s (primaryOf s r)
  then some (resync s r) else none

/-- `o` only yields `Step` successors of `s`. -/
def Good (s : S) (o : Option S) : Prop := ∀ t, o = some t → Step true false s t

section sound
variable (s : S)

theorem gAppend (p) : Good s (oAppend s p) := by
  intro t h; unfold oAppend at h; split at h
  · next g => cases h; exact Step.append s p g.1 g.2
  · cases h
theorem gReplicate (r) : Good s (oReplicate s r) := by
  intro t h; unfold oReplicate at h; split at h
  · next g => cases h; exact Step.replicate s r g.1 g.2.1 g.2.2
  · cases h
theorem gAck (r) : Good s (oAck s r) := by
  intro t h; unfold oAck at h; split at h
  · next g => cases h; exact Step.ack s r g.1 g.2.1 g.2.2
  · cases h
theorem gCommit (p) : Good s (oCommit s p) := by
  intro t h; unfold oCommit at h; split at h
  · next g => cases h; exact Step.commit s p g.1 g.2.1 g.2.2
  · cases h
theorem gCount (p r) : Good s (oCount s p r) := by
  intro t h; unfold oCount at h; split at h
  · next g => cases h; exact Step.count s p r g.1 g.2.1 g.2.2.1 g.2.2.2.1 g.2.2.2.2.1 g.2.2.2.2.2
  · cases h
theorem gMakeElig (p r) : Good s (oMakeElig s p r) := by
  intro t h; unfold oMakeElig at h; split at h
  · next g => cases h; exact Step.makeElig s p r g.1 g.2.1 g.2.2.1 g.2.2.2
  · cases h
theorem gMakeInelig (p r) : Good s (oMakeInelig s p r) := by
  intro t h; unfold oMakeInelig at h; split at h
  · next g => cases h; exact Step.makeInelig s p r g.1 g.2.1 g.2.2
  · cases h
theorem gEarlyUncount (p r) : Good s (oEarlyUncount s p r) := by
  intro t h; unfold oEarlyUncount at h; split at h
  · next g => exact absurd g.1 (by decide)
  · cases h
theorem gLearn (n) : Good s (oLearn s n) := by
  intro t h; unfold oLearn at h; split at h
  · next g => cases h; exact Step.learn s n g
  · cases h
theorem gTakeover (n) : Good s (oTakeover s n) := by
  intro t h; unfold oTakeover at h; split at h
  · next g => cases h; exact Step.takeover s n g.1 g.2.1 g.2.2
  · cases h
theorem gResync (r) : Good s (oResync s r) := by
  intro t h; unfold oResync at h; split at h
  · next g => cases h; exact Step.resync s r g.1 g.2.1 g.2.2.1 g.2.2.2.1 g.2.2.2.2
  · cases h

end sound

/-! ### All candidate successors, labelled as TLC labels them -/

def nodeName (n : N) : String := "n" ++ toString (n.val + 1)
def lbl1 (a : String) (n : N) : String := a ++ "(" ++ nodeName n ++ ")"
def lbl2 (a : String) (p r : N) : String := a ++ "(" ++ nodeName p ++ "," ++ nodeName r ++ ")"

def nodes : List N := [0, 1, 2]

def nodeActs (s : S) (n : N) : List (String × Option S) :=
  [(lbl1 "AppendWrite" n, oAppend s n), (lbl1 "Replicate" n, oReplicate s n),
   (lbl1 "Ack" n, oAck s n), (lbl1 "Commit" n, oCommit s n),
   (lbl1 "Learn" n, oLearn s n), (lbl1 "Takeover" n, oTakeover s n),
   (lbl1 "Resync" n, oResync s n)]

def pairActs (s : S) (p r : N) : List (String × Option S) :=
  [(lbl2 "Count" p r, oCount s p r), (lbl2 "MakeEligible" p r, oMakeElig s p r),
   (lbl2 "MakeIneligible" p r, oMakeInelig s p r),
   (lbl2 "EarlyUncount" p r, oEarlyUncount s p r)]

def allActs (s : S) : List (String × Option S) :=
  nodes.flatMap (nodeActs s) ++ nodes.flatMap (fun p => nodes.flatMap (pairActs s p))

/-- Every enabled constructor instance with its label and successor. -/
def succs (s : S) : List (String × S) :=
  (allActs s).filterMap (fun a => a.2.map (a.1, ·))

theorem allActs_good (s : S) : ∀ a ∈ allActs s, Good s a.2 := by
  intro a ha
  simp only [allActs, List.mem_append, List.mem_flatMap] at ha
  rcases ha with ⟨n, _, ha⟩ | ⟨p, _, r, _, ha⟩
  · simp only [nodeActs, List.mem_cons, List.not_mem_nil, or_false] at ha
    rcases ha with rfl | rfl | rfl | rfl | rfl | rfl | rfl
    · exact gAppend s n
    · exact gReplicate s n
    · exact gAck s n
    · exact gCommit s n
    · exact gLearn s n
    · exact gTakeover s n
    · exact gResync s n
  · simp only [pairActs, List.mem_cons, List.not_mem_nil, or_false] at ha
    rcases ha with rfl | rfl | rfl | rfl
    · exact gCount s p r
    · exact gMakeElig s p r
    · exact gMakeInelig s p r
    · exact gEarlyUncount s p r

theorem succs_sound {s t : S} {l : String} (h : (l, t) ∈ succs s) : Step true false s t := by
  simp only [succs, List.mem_filterMap] at h
  obtain ⟨a, ha, he⟩ := h
  cases ho : a.2 with
  | none => rw [ho] at he; cases he
  | some u =>
    rw [ho] at he; simp only [Option.map_some, Option.some.injEq, Prod.mk.injEq] at he
    rw [← he.2]; exact allActs_good s a ha u ho

def stepB (s s' : S) : Bool := (succs s).any (fun x => decide (x.2 = s'))

/-- Labelled version: some constructor instance named `l` leads from `s` to `s'`. -/
def stepLB (s : S) (l : String) (s' : S) : Bool := (succs s).any (fun x => x.1 == l && decide (x.2 = s'))

theorem stepB_sound {s s' : S} (h : stepB s s' = true) : Step true false s s' := by
  simp only [stepB, List.any_eq_true, decide_eq_true_eq] at h
  obtain ⟨⟨l, t⟩, hm, rfl⟩ := h
  exact succs_sound hm

theorem stepLB_sound {s s' : S} {l : String} (h : stepLB s l s' = true) : Step true false s s' := by
  simp only [stepLB, List.any_eq_true, Bool.and_eq_true, decide_eq_true_eq] at h
  obtain ⟨⟨l', t⟩, hm, _, rfl⟩ := h
  exact succs_sound hm

/-! ### The proof's invariant as a decidable proposition

The invariant proof is `RestartProof`'s, so the check (d) evaluates
`RestartProof.Inv` on the image of a Core state under the embedding chain
`CoreProof.toRecon` then `ReconProof.emb` (`lift`). -/

abbrev R := RestartProof.State N

/-- The CoreRestart state a Core state stands for (no restart, every incarnation 1). -/
def lift (s : S) : R := ReconProof.emb (toRecon s)

instance instDecEqRCfg : DecidableEq (RestartProof.Cfg N) := fun a b =>
  if h : a.ver = b.ver ∧ a.prim = b.prim ∧ a.elig = b.elig ∧ a.inc = b.inc then
    isTrue (by cases a; cases b; simp_all)
  else isFalse (fun e => h (by subst e; exact ⟨rfl, rfl, rfl, rfl⟩))

instance (s : R) (p : N) : Decidable (RestartProof.ThinksPrimary s p) := by
  unfold RestartProof.ThinksPrimary; infer_instance
instance (s : R) (n : N) : Decidable (RestartProof.IsAuthoritative s n) := by
  unfold RestartProof.IsAuthoritative; infer_instance
instance (s : R) : Decidable (RestartProof.PrimaryAuthoritative s) := by
  unfold RestartProof.PrimaryAuthoritative; infer_instance
/-- `p` is fenced at `w`'s acknowledgement: the hypotheses of `Fenced.of_left`. A
decidable stand-in for `Fenced`, which is defined with a set minimum. -/
def FencedAt (s : R) (p w : N) : Prop :=
  (s.sync p w = true ∧ (s.known w).prim ≠ p) ∧
  (RestartProof.PrimaryAuthoritative s → s.acked p w ≤ s.recon s.cfg.prim) ∧
  (RestartProof.PrimaryAuthoritative s → (s.log p).take (s.acked p w) <+: s.log s.cfg.prim) ∧
  ∀ m, s.cfg.elig m = true → s.cfg.inc m = s.inc m →
    (s.log p).take (s.acked p w) <+: s.log m ∨ (s.sync p m = true ∧ (s.known m).prim = p)
instance (s : R) (p w : N) : Decidable (FencedAt s p w) := by
  unfold FencedAt; infer_instance
theorem FencedAt.fenced {s : R} {p w : N} (h : FencedAt s p w) : RestartProof.Fenced s p :=
  RestartProof.Fenced.of_left h.1 h.2.1 h.2.2.1 h.2.2.2

/-! `RestartProof.Inv` field by field (same statements, same order), each decidable. -/

section
open RestartProof (IsAuthoritative PrimaryAuthoritative)
variable (s : R)

def I_ehc : Prop := ∀ n, IsAuthoritative s n → s.hist <+: s.log n
def I_caLen : Prop := ∀ p, s.commit p ≤ (s.log p).length
def I_caHist : Prop := ∀ p, s.commit p ≤ s.hist.length
def I_caEq : Prop := ∀ p, (s.log p).take (s.commit p) = s.hist.take (s.commit p)
def I_knownLe : Prop := ∀ n, (s.known n).ver ≤ s.cfg.ver
def I_knownEq : Prop := ∀ n, (s.known n).ver = s.cfg.ver → s.known n = s.cfg
def I_primKnows : Prop := s.known s.cfg.prim = s.cfg
def I_incLe : Prop := ∀ n, s.cfg.inc n ≤ s.inc n
def I_knownIncLe : Prop := ∀ x n, (s.known x).inc n ≤ s.inc n
def I_authEx : Prop := ∃ n, IsAuthoritative s n
def I_eligSync : Prop :=
  ∀ p r, RestartProof.ThinksPrimary s p → (s.known p).elig r = true → (s.known p).inc r = s.inc r →
    s.sync p r = true
def I_noSelfSync : Prop := ∀ p, s.sync p p = false
def I_ackedPrim : Prop := ∀ r, (s.known r).prim ≠ s.cfg.prim → s.acked s.cfg.prim r = 0
def I_ackedLe : Prop := ∀ p r, RestartProof.ThinksPrimary s p → s.acked p r ≤ (s.log p).length
def I_ackHolds : Prop :=
  ∀ p n, RestartProof.ThinksPrimary s p → n ≠ p → (s.known n).prim = p →
    (s.log p).take (s.acked p n) <+: s.log n
def I_syncNew : Prop :=
  ∀ r, s.sync s.cfg.prim r = true → s.cfg.elig r = false →
    (s.known r).prim = s.cfg.prim ∧ s.recon s.cfg.prim ≤ s.acked s.cfg.prim r
def I_reconLen : Prop := s.recon s.cfg.prim ≤ (s.log s.cfg.prim).length
def I_syncCommit : Prop := ∀ q r, RestartProof.ThinksPrimary s q → s.sync q r = true → s.commit q ≤ s.acked q r
def I_eligFol : Prop :=
  ∀ m, s.cfg.elig m = true → (s.known m).prim ≠ s.cfg.prim →
    (s.known (s.known m).prim).elig m = true
def I_histLe : Prop :=
  PrimaryAuthoritative s → s.hist.length ≤ s.commit s.cfg.prim ∨ s.hist.length ≤ s.recon s.cfg.prim
def I_stale : Prop := ∀ p, RestartProof.ThinksPrimary s p → (s.known p).ver < s.cfg.ver → ∃ w, FencedAt s p w

instance : Decidable (I_ehc s) := by unfold I_ehc; infer_instance
instance : Decidable (I_caLen s) := by unfold I_caLen; infer_instance
instance : Decidable (I_caHist s) := by unfold I_caHist; infer_instance
instance : Decidable (I_caEq s) := by unfold I_caEq; infer_instance
instance : Decidable (I_knownLe s) := by unfold I_knownLe; infer_instance
instance : Decidable (I_knownEq s) := by unfold I_knownEq; infer_instance
instance : Decidable (I_primKnows s) := by unfold I_primKnows; infer_instance
instance : Decidable (I_incLe s) := by unfold I_incLe; infer_instance
instance : Decidable (I_knownIncLe s) := by unfold I_knownIncLe; infer_instance
instance : Decidable (I_authEx s) := by unfold I_authEx; infer_instance
instance : Decidable (I_eligSync s) := by unfold I_eligSync; infer_instance
instance : Decidable (I_noSelfSync s) := by unfold I_noSelfSync; infer_instance
instance : Decidable (I_ackedPrim s) := by unfold I_ackedPrim; infer_instance
instance : Decidable (I_ackedLe s) := by unfold I_ackedLe; infer_instance
instance : Decidable (I_ackHolds s) := by unfold I_ackHolds; infer_instance
instance : Decidable (I_syncNew s) := by unfold I_syncNew; infer_instance
instance : Decidable (I_reconLen s) := by unfold I_reconLen; infer_instance
instance : Decidable (I_syncCommit s) := by unfold I_syncCommit; infer_instance
instance : Decidable (I_eligFol s) := by unfold I_eligFol; infer_instance
instance : Decidable (I_histLe s) := by unfold I_histLe; infer_instance
instance : Decidable (I_stale s) := by unfold I_stale; infer_instance

def InvP : Prop :=
  I_ehc s ∧ I_caLen s ∧ I_caHist s ∧ I_caEq s ∧ I_knownLe s ∧ I_knownEq s ∧ I_primKnows s ∧
  I_incLe s ∧ I_knownIncLe s ∧ I_authEx s ∧ I_eligSync s ∧ I_noSelfSync s ∧ I_ackedPrim s ∧
  I_ackedLe s ∧ I_ackHolds s ∧ I_syncNew s ∧ I_reconLen s ∧ I_syncCommit s ∧ I_eligFol s ∧
  I_histLe s ∧ I_stale s

instance : Decidable (InvP s) := by unfold InvP; infer_instance

end

theorem invP_inv {s : R} (h : InvP s) : RestartProof.Inv s := by
  obtain ⟨h1, h2, h3, h4, h5, h6, h7, h8, h9, h10, h11, h12, h13, h14, h15, h16, h17, h18,
    h19, h20, h21⟩ := h
  exact
    { ehc := h1, caLen := h2, caHist := h3, caEq := h4, knownLe := h5, knownEq := h6, primKnows := h7, incLe := h8, knownIncLe := h9, authEx := h10, eligSync := h11, noSelfSync := h12, ackedPrim := h13, ackedLe := h14, ackHolds := h15, syncNew := h16, reconLen := h17, syncCommit := h18, eligFol := h19, histLe := h20, stale := fun p hp hv => (h21 p hp hv).elim fun _ hw => hw.fenced }

/-- `RestartProof.Inv` holds on the CoreRestart image of `s`. -/
def invB (s : S) : Bool := decide (InvP (lift s))

theorem invB_sound {s : S} (h : invB s = true) : RestartProof.Inv (lift s) :=
  invP_inv (of_decide_eq_true h)

instance (s : S) : Decidable (Safety s) := by unfold Safety EligibleHoldsCommitted CommittedAgree; infer_instance

def safetyB (s : S) : Bool := decide (Safety s)

theorem safetyB_sound {s : S} (h : safetyB s = true) : Safety s := of_decide_eq_true h

/-- The TLC bounds of `spec/Core_xcheck.cfg`: MaxLen = 1, MaxVer = 5. -/
def inBounds (s : S) : Bool :=
  nodes.all (fun n => (s.log n).length ≤ 1) && s.hist.length ≤ 1 && s.cfg.ver ≤ 5

end XCheck
