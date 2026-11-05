import RestartProof.Lists
import RestartProof.Model

/-! The inductive invariant of CoreRestart. It is `ReconProof.Inv` with:
- `ehc` and the last clause of `Wit` over authoritative nodes (`auth`);
- `eligSync` (O1) only for eligible replicas whose recorded incarnation matches;
- `histLe` and the current-primary clauses of `Wit` only while the current primary is
  authoritative (`pa`): a primary that came back empty holds nothing, and nothing new
  is committed until an authoritative eligible replica takes over;
- a new conjunct `incLe`: the config never records an incarnation ahead of the node. -/

namespace RestartProof

variable {Node : Type} [DecidableEq Node]

def Wit (s : State Node) (p w : Node) : Prop :=
  s.sync p w = true ∧ (s.known w).prim ≠ p ∧
  (∀ r, s.sync p r = true → (s.known r).prim ≠ p → s.acked p w ≤ s.acked p r) ∧
  (pa s → s.acked p w ≤ s.recon s.cfg.prim) ∧
  (pa s → (s.log p).take (s.acked p w) <+: s.log s.cfg.prim) ∧
  ∀ m, s.cfg.elig m = true → s.cfg.inc m = s.inc m →
    ((s.log p).take (s.acked p w) <+: s.log m ∨ (s.sync p m = true ∧ (s.known m).prim = p))

structure Inv (s : State Node) : Prop where
  ehc        : ∀ n, auth s n → s.hist <+: s.log n
  caLen      : ∀ p, s.commit p ≤ (s.log p).length
  caHist     : ∀ p, s.commit p ≤ s.hist.length
  caEq       : ∀ p, (s.log p).take (s.commit p) = s.hist.take (s.commit p)
  knownLe    : ∀ n, (s.known n).ver ≤ s.cfg.ver
  knownEq    : ∀ n, (s.known n).ver = s.cfg.ver → s.known n = s.cfg
  primKnows  : s.known s.cfg.prim = s.cfg
  incLe      : ∀ n, s.cfg.inc n ≤ s.inc n
  knownIncLe : ∀ x n, (s.known x).inc n ≤ s.inc n
  /-- Survival: some node is authoritative (kept by the RestartEmpty guard). -/
  authEx     : ∃ n, auth s n
  eligSync   : ∀ p r, tp s p → (s.known p).elig r = true → (s.known p).inc r = s.inc r →
                 s.sync p r = true
  noSelfSync : ∀ p, s.sync p p = false
  ackedPrim  : ∀ r, (s.known r).prim ≠ s.cfg.prim → s.acked s.cfg.prim r = 0
  ackedLe    : ∀ p r, tp s p → s.acked p r ≤ (s.log p).length
  ackHolds   : ∀ p n, tp s p → n ≠ p → (s.known n).prim = p →
                 (s.log p).take (s.acked p n) <+: s.log n
  syncNew    : ∀ r, s.sync s.cfg.prim r = true → s.cfg.elig r = false →
                 (s.known r).prim = s.cfg.prim ∧ s.recon s.cfg.prim ≤ s.acked s.cfg.prim r
  reconLen   : s.recon s.cfg.prim ≤ (s.log s.cfg.prim).length
  syncCommit : ∀ q r, tp s q → s.sync q r = true → s.commit q ≤ s.acked q r
  eligFol    : ∀ m, s.cfg.elig m = true → (s.known m).prim ≠ s.cfg.prim →
                 (s.known (s.known m).prim).elig m = true
  histLe     : pa s → s.hist.length ≤ s.commit s.cfg.prim ∨ s.hist.length ≤ s.recon s.cfg.prim
  stale      : ∀ p, tp s p → (s.known p).ver < s.cfg.ver → ∃ w, Wit s p w

theorem inv_safety {s : State Node} (h : Inv s) : Safety s :=
  ⟨h.ehc, fun p => ⟨h.caLen p, h.caEq p⟩⟩

theorem inv_init (i : Node) : Inv (init i) where
  ehc := by intro n _; simp [init]
  caLen := by intro p; simp [init]
  caHist := by intro p; simp [init]
  caEq := by intro p; simp [init]
  knownLe := by intro n; simp [init]
  knownEq := by intro n _; rfl
  primKnows := rfl
  incLe := by intro n; simp [init, cfg0]
  authEx := ⟨i, Or.inr rfl, rfl⟩
  knownIncLe := by intro x n; simp [init, cfg0]
  eligSync := by intro p r _ h; simp [init, cfg0] at h
  noSelfSync := by intro p; rfl
  ackedPrim := by intro r _; rfl
  ackedLe := by intro p r _; simp [init]
  ackHolds := by intro p n _ _ _; simp [init]
  syncNew := by intro r h; simp [init] at h
  reconLen := by simp [init]
  syncCommit := by intro q r _ _; simp [init]
  eligFol := by intro m h; simp [init, cfg0] at h
  histLe := by intro _; simp [init]
  stale := by intro p _ h; simp [init, cfg0] at h

/-! ### Derived facts -/

section
variable {s : State Node} (h : Inv s)
include h

theorem tp_prim (hpa : pa s) : tp s s.cfg.prim := by
  unfold tp; rw [h.primKnows]; exact ⟨rfl, hpa⟩

theorem tp_kl {p : Node} (hp : tp s p) (hk : kl s p) : s.known p = s.cfg ∧ s.cfg.prim = p := by
  have e := h.knownEq p hk
  refine ⟨e, ?_⟩
  have := hp.1; rw [e] at this; exact this

theorem tp_cases {p : Node} (hp : tp s p) :
    (p = s.cfg.prim ∧ s.known p = s.cfg) ∨ (s.known p).ver < s.cfg.ver := by
  rcases Nat.lt_or_ge (s.known p).ver s.cfg.ver with hl | hg
  · exact Or.inr hl
  · have hk : kl s p := Nat.le_antisymm (h.knownLe p) hg
    have := tp_kl h hp hk
    exact Or.inl ⟨this.2.symm, this.1⟩

omit h in
/-- A node that thinks it is primary and knows the latest config is the current,
authoritative primary. -/
theorem tp_pa {p : Node} (hp : tp s p) (hk : s.known p = s.cfg) : p = s.cfg.prim ∧ pa s := by
  have e1 := hp.1; have e2 := hp.2
  rw [hk] at e1 e2
  subst e1; exact ⟨rfl, e2⟩

theorem stale_ne_prim {p : Node} (hv : (s.known p).ver < s.cfg.ver) : p ≠ s.cfg.prim := by
  intro e; subst e; rw [h.primKnows] at hv; omega

theorem kl_iff_prim {p : Node} (hp : tp s p) (hk : kl s p) : p = s.cfg.prim :=
  (tp_kl h hp hk).2.symm

theorem prim_holds_sync (hpa : pa s) {m : Node} (hs : s.sync s.cfg.prim m = true) :
    (s.log s.cfg.prim).take (s.acked s.cfg.prim m) <+: s.log m := by
  by_cases hf : (s.known m).prim = s.cfg.prim
  · have hne : m ≠ s.cfg.prim := by
      intro e; rw [e, h.noSelfSync] at hs; exact Bool.false_ne_true hs
    exact h.ackHolds _ _ (tp_prim h hpa) hne hf
  · rw [h.ackedPrim m hf]; simp

/-- Core commit fact: if `p` may commit offset `c` by the commit rule, every
authoritative node holds the first `c+1` entries of `p`. -/
theorem commit_holds {p : Node} (hp : tp s p) (hlen : s.commit p < (s.log p).length)
    (hcap : ∀ r, s.sync p r = true → s.commit p < s.acked p r) :
    ∀ m, auth s m → (s.log p).take (s.commit p + 1) <+: s.log m := by
  intro m ⟨hm, hi⟩
  rcases tp_cases h hp with ⟨rfl, hk⟩ | hv
  · have hpa : pa s := (tp_pa hp hk).2
    rcases hm with he | rfl
    · have hs : s.sync s.cfg.prim m = true :=
        h.eligSync _ _ hp (by rw [hk]; exact he) (by rw [hk]; exact hi)
      exact take_mono_prefix (hcap m hs) (prim_holds_sync h hpa hs)
    · exact List.take_prefix _ _
  · obtain ⟨w, hw1, _, _, _, hwp, hwe⟩ := h.stale p hp hv
    have hcw := hcap w hw1
    rcases hm with he | rfl
    · rcases hwe m he hi with hd | ⟨hs, hf⟩
      · exact take_mono_prefix hcw hd
      · by_cases hmp : m = p
        · subst hmp; exact List.take_prefix _ _
        · exact take_mono_prefix (hcap m hs) (h.ackHolds _ _ hp hmp hf)
    · exact take_mono_prefix hcw (hwp hi)

end

end RestartProof
