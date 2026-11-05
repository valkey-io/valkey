import RestartProof.Inv
import RestartProof.Contracts

/-!
# Preservation: the commit rule (design §2; promotion step 2)

Commit (a primary advances its committed offset once every sync replica has
acknowledged) and Count (a primary starts counting a caught-up replica).
-/

namespace RestartProof

variable {Node : Type} [DecidableEq Node]

-- Every layer lemma takes the action's whole guard, so some take a part they
-- do not use.
set_option linter.unusedSectionVars false

section commit

variable {s : State Node} (h : Inv s) (p : Node) (hp : ThinksPrimary s p) (hlen : s.commit p < (s.log p).length) (hcap : ∀ r, s.sync p r = true → s.commit p < s.acked p r)

include h hp hlen hcap

/-- Commit changes no configuration. -/
theorem config_commit : ConfigInv (commitNext s p) :=
  (h.toConfigInv).frame rfl rfl rfl

/-- The commit guard requires every sync replica to have acknowledged past the new commit point. -/
theorem repl_commit : ReplInv (commitNext s p) := by
  have hold := commit_holds h hp hlen hcap
  have hC := commitNext_commit (s := s) p
  have hsucc := take_succ_getD hlen
  have hcaEqP := h.caEq p
  have hcl := h.caLen p
  have hch := h.caHist p
  have hH : (commitNext s p).hist.take (s.commit p + 1) = (s.log p).take (s.commit p + 1) ∧
      s.hist <+: (commitNext s p).hist ∧ (commitNext s p).hist.length ≤ s.hist.length + 1 ∧
      ((commitNext s p).hist.length = s.hist.length + 1 → s.commit p = s.hist.length) := by
    by_cases he : s.commit p = s.hist.length
    · have hh := commitNext_hist_new (s := s) he
      rw [hh]
      refine ⟨?_, List.prefix_append _ _, by simp, fun _ => he⟩
      rw [hsucc, hcaEqP, he, List.take_length, List.take_of_length_le (by simp)]
    · have hh := commitNext_hist_old (s := s) he
      rw [hh]
      refine ⟨?_, List.prefix_refl _, by omega, fun e => by omega⟩
      have hlt : s.commit p + 1 ≤ s.hist.length := by omega
      -- both are prefixes of the log of an authoritative node
      obtain ⟨a, ha⟩ := h.authEx
      have h1 := hold a ha
      have h2 := h.ehc a ha
      have hl1 : s.commit p + 1 ≤ ((s.log p).take (s.commit p + 1)).length := by
        rw [List.length_take_of_le hlen]
      have := prefixes_agree h2 h1 (k := s.commit p + 1) hlt hl1
      rw [this, List.take_take]; simp
  obtain ⟨hH1, hH2, hH3, hH4⟩ := hH
  exact {
  noSelfSync := h.noSelfSync
  ackedPrim := h.ackedPrim
  ackedLe := h.ackedLe
  ackHolds := h.ackHolds
  syncCommit := by
    intro q r hq hs
    show (commitNext s p).commit q ≤ _
    rw [hC]; split
    · rename_i hx; subst hx; exact hcap r hs
    · exact h.syncCommit q r hq hs }

/-- Commit changes no sync replica set or eligibility. -/
theorem membership_commit : MembershipInv (commitNext s p) :=
  (h.toMembershipInv).frame rfl rfl rfl rfl rfl rfl

/-- The newly committed entry is the history's entry at that offset, or extends the history by one. -/
theorem commit_commit : CommitInv (commitNext s p) := by
  have hold := commit_holds h hp hlen hcap
  have hC := commitNext_commit (s := s) p
  have hsucc := take_succ_getD hlen
  have hcaEqP := h.caEq p
  have hcl := h.caLen p
  have hch := h.caHist p
  have hH : (commitNext s p).hist.take (s.commit p + 1) = (s.log p).take (s.commit p + 1) ∧
      s.hist <+: (commitNext s p).hist ∧ (commitNext s p).hist.length ≤ s.hist.length + 1 ∧
      ((commitNext s p).hist.length = s.hist.length + 1 → s.commit p = s.hist.length) := by
    by_cases he : s.commit p = s.hist.length
    · have hh := commitNext_hist_new (s := s) he
      rw [hh]
      refine ⟨?_, List.prefix_append _ _, by simp, fun _ => he⟩
      rw [hsucc, hcaEqP, he, List.take_length, List.take_of_length_le (by simp)]
    · have hh := commitNext_hist_old (s := s) he
      rw [hh]
      refine ⟨?_, List.prefix_refl _, by omega, fun e => by omega⟩
      have hlt : s.commit p + 1 ≤ s.hist.length := by omega
      -- both are prefixes of the log of an authoritative node
      obtain ⟨a, ha⟩ := h.authEx
      have h1 := hold a ha
      have h2 := h.ehc a ha
      have hl1 : s.commit p + 1 ≤ ((s.log p).take (s.commit p + 1)).length := by
        rw [List.length_take_of_le hlen]
      have := prefixes_agree h2 h1 (k := s.commit p + 1) hlt hl1
      rw [this, List.take_take]; simp
  obtain ⟨hH1, hH2, hH3, hH4⟩ := hH
  exact {
  caLen := by
    intro x; show (commitNext s p).commit x ≤ _; rw [hC]; split
    · rename_i hx; subst hx; exact hlen
    · exact h.caLen x
  caHist := by
    intro x; show (commitNext s p).commit x ≤ (commitNext s p).hist.length; rw [hC]; split
    · rename_i hx; subst hx
      have := hH1; have hl := congrArg List.length this
      rw [List.length_take_of_le hlen] at hl
      simp [List.length_take] at hl; omega
    · exact Nat.le_trans (h.caHist x) hH2.length_le
  caEq := by
    intro x; show (s.log x).take ((commitNext s p).commit x) = (commitNext s p).hist.take _; rw [hC]
    split
    · rename_i hx; subst hx; exact hH1.symm
    · rw [h.caEq x]; exact prefix_take_eq hH2 (h.caHist x)
  reconLen := h.reconLen
  histLe := by
    intro hpa
    have hpa' : PrimaryAuthoritative s := hpa
    show (commitNext s p).hist.length ≤ (commitNext s p).commit s.cfg.prim ∨
      (commitNext s p).hist.length ≤ s.recon s.cfg.prim
    rw [hC]
    rcases thinksPrimary_cases h hp with ⟨hpe, _⟩ | hv
    · rw [if_pos hpe.symm]
      have hl0 := h.histLe hpa'
      rw [← hpe] at hl0
      rcases Nat.lt_or_ge s.hist.length ((commitNext s p).hist.length) with hl | hl
      · left; have := hH4 (by omega); omega
      · rcases hl0 with a | a
        · left; omega
        · right; rw [← hpe]; omega
    · have hne := stale_ne_prim h hv
      rw [if_neg (fun e => hne e.symm)]
      rcases Nat.lt_or_ge s.hist.length ((commitNext s p).hist.length) with hl | hl
      · right
        have e := hH4 (by omega)
        -- a deposed primary commits below its fence, which is below the reconciliation point
        have hF := h.stale p hp hv
        obtain ⟨w, ⟨hw1, _⟩, hwF⟩ := hF.attained
        have := hF.reconLe hpa'
        have := hcap w hw1; omega
      · rcases h.histLe hpa' with a | a
        · left; omega
        · right; omega }

/-- Fences do not mention commit points. -/
theorem failover_commit : FailoverInv (commitNext s p) := by
  have hold := commit_holds h hp hlen hcap
  have hsucc := take_succ_getD hlen
  have hcaEqP := h.caEq p
  have hcl := h.caLen p
  have hch := h.caHist p
  have hH : (commitNext s p).hist.take (s.commit p + 1) = (s.log p).take (s.commit p + 1) ∧
      s.hist <+: (commitNext s p).hist ∧ (commitNext s p).hist.length ≤ s.hist.length + 1 ∧
      ((commitNext s p).hist.length = s.hist.length + 1 → s.commit p = s.hist.length) := by
    by_cases he : s.commit p = s.hist.length
    · have hh := commitNext_hist_new (s := s) he
      rw [hh]
      refine ⟨?_, List.prefix_append _ _, by simp, fun _ => he⟩
      rw [hsucc, hcaEqP, he, List.take_length, List.take_of_length_le (by simp)]
    · have hh := commitNext_hist_old (s := s) he
      rw [hh]
      refine ⟨?_, List.prefix_refl _, by omega, fun e => by omega⟩
      have hlt : s.commit p + 1 ≤ s.hist.length := by omega
      -- both are prefixes of the log of an authoritative node
      obtain ⟨a, ha⟩ := h.authEx
      have h1 := hold a ha
      have h2 := h.ehc a ha
      have hl1 : s.commit p + 1 ≤ ((s.log p).take (s.commit p + 1)).length := by
        rw [List.length_take_of_le hlen]
      have := prefixes_agree h2 h1 (k := s.commit p + 1) hlt hl1
      rw [this, List.take_take]; simp
  obtain ⟨hH1, hH2, hH3, hH4⟩ := hH
  exact {
  -- Commit moves only commit points and the history, which the fence does not mention.
  stale := fun q hq hv => (h.stale q hq hv).mono rfl rfl rfl rfl (fun _ => List.prefix_refl _)
    (fun x => h.ackedLe q x hq) (fun _ x => x) (fun _ _ => rfl)
  authEx := h.authEx }

/-- Every authoritative node already holds the newly committed entry (`commit_holds`). -/
theorem ehc_commit : EligibleHoldsCommitted (commitNext s p) := by
  have hold := commit_holds h hp hlen hcap
  have hsucc := take_succ_getD hlen
  have hcaEqP := h.caEq p
  have hcl := h.caLen p
  have hch := h.caHist p
  have hH : (commitNext s p).hist.take (s.commit p + 1) = (s.log p).take (s.commit p + 1) ∧
      s.hist <+: (commitNext s p).hist ∧ (commitNext s p).hist.length ≤ s.hist.length + 1 ∧
      ((commitNext s p).hist.length = s.hist.length + 1 → s.commit p = s.hist.length) := by
    by_cases he : s.commit p = s.hist.length
    · have hh := commitNext_hist_new (s := s) he
      rw [hh]
      refine ⟨?_, List.prefix_append _ _, by simp, fun _ => he⟩
      rw [hsucc, hcaEqP, he, List.take_length, List.take_of_length_le (by simp)]
    · have hh := commitNext_hist_old (s := s) he
      rw [hh]
      refine ⟨?_, List.prefix_refl _, by omega, fun e => by omega⟩
      have hlt : s.commit p + 1 ≤ s.hist.length := by omega
      -- both are prefixes of the log of an authoritative node
      obtain ⟨a, ha⟩ := h.authEx
      have h1 := hold a ha
      have h2 := h.ehc a ha
      have hl1 : s.commit p + 1 ≤ ((s.log p).take (s.commit p + 1)).length := by
        rw [List.length_take_of_le hlen]
      have := prefixes_agree h2 h1 (k := s.commit p + 1) hlt hl1
      rw [this, List.take_take]; simp
  obtain ⟨hH1, hH2, hH3, hH4⟩ := hH
  intro n hn
  show (commitNext s p).hist <+: s.log n
  by_cases he : s.commit p = s.hist.length
  · have hh := commitNext_hist_new (s := s) he
    rw [hh]
    have := hold n hn
    rw [hsucc, hcaEqP, he, List.take_length] at this
    rw [he]; exact this
  · have hh := commitNext_hist_old (s := s) he
    rw [hh]; exact h.ehc n hn

theorem inv_commit : Inv (commitNext s p) where
  toConfigInv := config_commit h p hp hlen hcap
  toReplInv := repl_commit h p hp hlen hcap
  toMembershipInv := membership_commit h p hp hlen hcap
  toCommitInv := commit_commit h p hp hlen hcap
  toFailoverInv := failover_commit h p hp hlen hcap
  ehc := ehc_commit h p hp hlen hcap

end commit

section count

variable {s : State Node} (h : Inv s) (p r : Node) (hp : ThinksPrimary s p) (hrp : r ≠ p) (hf : primaryOf s r = p) (hcup : catchUpPoint true s p ≤ s.acked p r)

include h r hp hrp hf hcup

/-- Count changes no configuration. -/
theorem config_count : ConfigInv (count s p r) :=
  (h.toConfigInv).frame rfl rfl rfl

/-- `r` has acknowledged up to the catch-up point, which is at least `p`'s commit point, so `p` still never commits past a sync replica's acknowledgement. -/
theorem repl_count : ReplInv (count s p r) := by
  have hle : s.commit p ≤ s.acked p r := Nat.le_trans (commit_le_catchUpPoint h p) hcup
  have hS := count_sync (s := s) p r
  exact {
  noSelfSync := by intro q; show (count s p r).sync q q = false; rw [hS]; simp [h.noSelfSync q]; intro e; subst e; exact fun e => hrp e.symm
  ackedPrim := h.ackedPrim
  ackedLe := h.ackedLe
  ackHolds := h.ackHolds
  syncCommit := by
    intro q x hq hs
    change (count s p r).sync q x = true at hs
    rw [hS] at hs; simp at hs
    rcases hs with hs | ⟨rfl, rfl⟩
    · exact h.syncCommit q x hq hs
    · exact hle }

/-- A newly counted ineligible replica follows `p` and has acknowledged past the reconciliation point (promotion steps 1-2). -/
theorem membership_count : MembershipInv (count s p r) := by
  have h0 := h
  have hS := count_sync (s := s) p r
  exact {
  eligSync := by intro q x hq he hi; show (count s p r).sync q x = true; rw [hS]; simp [h.eligSync q x hq he hi]
  syncNew := by
    intro x hs he
    change (count s p r).sync s.cfg.prim x = true at hs
    rw [hS] at hs
    simp at hs
    rcases hs with hs | ⟨hq, rfl⟩
    · exact h.syncNew x hs he
    · subst hq
      have := Nat.le_trans (recon_le_catchUpPoint (s := s) h0) hcup
      exact ⟨hf, this⟩
  eligFol := h.eligFol }

/-- Count changes no commit point, catch-up point or history. -/
theorem commit_count : CommitInv (count s p r) :=
  (h.toCommitInv).frame rfl rfl rfl rfl rfl rfl

/-- Adding a sync replica does not move a fence. -/
theorem failover_count : FailoverInv (count s p r) := by
  have hS := count_sync (s := s) p r
  exact {
  stale := by
    -- Count only adds a replica to a sync replica set.
    intro q hq hv
    refine (h.stale q hq hv).mono rfl rfl rfl rfl (fun _ => List.prefix_refl _)
      (fun x => h.ackedLe q x hq) ?_ (fun _ _ => rfl)
    intro x hx; show (count s p r).sync q x = true; rw [hS]; simp [hx]
  authEx := h.authEx }

/-- Count moves no log. -/
theorem ehc_count : EligibleHoldsCommitted (count s p r) := by
  exact h.ehc

theorem inv_count : Inv (count s p r) where
  toConfigInv := config_count h p r hp hrp hf hcup
  toReplInv := repl_count h p r hp hrp hf hcup
  toMembershipInv := membership_count h p r hp hrp hf hcup
  toCommitInv := commit_count h p r hp hrp hf hcup
  toFailoverInv := failover_count h p r hp hrp hf hcup
  ehc := ehc_count h p r hp hrp hf hcup

end count

end RestartProof
