import RestartProof.Actions3

/-! Preservation for RestartEmpty (under the survival assumption) and RestartKeep
(with the F5 fix, RebuildSync = TRUE). -/

namespace RestartProof

variable {Node : Type} [DecidableEq Node]

theorem inv_restartEmpty {s : State Node} (h : Inv s) (n : Node) (hkl : KnowsLatest s n)
    (hsurv : ∃ m, m ≠ n ∧ IsAuthoritative s m) : Inv (restartEmpty s n) := by
  have hkn : s.known n = s.cfg := h.knownEq n hkl
  have hI := restartEmpty_inc (s := s) (n := n)
  have hIo : ∀ x, x ≠ n → (restartEmpty s n).inc x = s.inc x := fun _ => restartEmpty_inc_other
  have hIn := restartEmpty_inc_self (s := s) (n := n)
  have hLo : ∀ x, x ≠ n → (restartEmpty s n).log x = s.log x := fun _ => restartEmpty_log_other
  have hCo : ∀ x, x ≠ n → (restartEmpty s n).commit x = s.commit x := fun _ => restartEmpty_commit_other
  have hCn := restartEmpty_commit_self (s := s) (n := n)
  have hRo : ∀ x, x ≠ n → (restartEmpty s n).recon x = s.recon x := fun _ => restartEmpty_recon_other
  have hRn := restartEmpty_recon_self (s := s) (n := n)
  have hS := restartEmpty_sync (s := s) (n := n)
  have hA := restartEmpty_acked (s := s) (n := n)
  have hAle := restartEmpty_acked_le (s := s) (n := n)
  have hSle : ∀ p r, (restartEmpty s n).sync p r = true → s.sync p r = true :=
    fun _ _ => restartEmpty_sync_le
  -- the restarted node is no longer authoritative and not a primary
  have hnauth : ¬ (s.cfg.inc n = s.inc n + 1) := by have := h.incLe n; omega
  have htp : ∀ x, ThinksPrimary (restartEmpty s n) x → x ≠ n ∧ ThinksPrimary s x :=
    fun _ => restartEmpty_thinksPrimary hkn (h.incLe n)
  have hauth : ∀ x, IsAuthoritative (restartEmpty s n) x → x ≠ n ∧ IsAuthoritative s x :=
    fun _ => restartEmpty_isAuthoritative (h.incLe n)
  have hpa : PrimaryAuthoritative (restartEmpty s n) → s.cfg.prim ≠ n ∧ PrimaryAuthoritative s :=
    restartEmpty_primaryAuthoritative (h.incLe n)
  exact {
  ehc := by
    intro x hx
    obtain ⟨hxn, hx'⟩ := hauth x hx
    rw [hLo x hxn]; exact h.ehc x hx'
  caLen := by
    intro x; by_cases e : x = n
    · subst e; rw [hCn]; exact Nat.zero_le _
    · rw [hCo x e, hLo x e]; exact h.caLen x
  caHist := by
    intro x; by_cases e : x = n
    · subst e; rw [hCn]; exact Nat.zero_le _
    · rw [hCo x e]; exact h.caHist x
  caEq := by
    intro x; by_cases e : x = n
    · subst e; rw [hCn]; rfl
    · rw [hCo x e, hLo x e]; exact h.caEq x
  knownLe := h.knownLe, knownEq := h.knownEq, primKnows := h.primKnows
  incLe := by
    intro x; show s.cfg.inc x ≤ (restartEmpty s n).inc x; rw [hI]; split
    · rename_i e; subst e; have := h.incLe x; omega
    · exact h.incLe x
  knownIncLe := by
    intro x y; show (s.known x).inc y ≤ (restartEmpty s n).inc y; rw [hI]; split
    · rename_i e; subst e; have := h.knownIncLe x y; omega
    · exact h.knownIncLe x y
  authEx := by
    obtain ⟨m, hmn, hm1, hm2⟩ := hsurv
    refine ⟨m, hm1, ?_⟩
    show s.cfg.inc m = (restartEmpty s n).inc m; rw [hIo m hmn]; exact hm2
  eligSync := by
    intro p r hp he hi
    obtain ⟨hpn, hp'⟩ := htp p hp
    have hrn : r ≠ n := by
      intro e; subst e
      change (s.known p).inc r = (restartEmpty s r).inc r at hi
      rw [hIn] at hi; have := h.knownIncLe p r; omega
    change (s.known p).inc r = (restartEmpty s n).inc r at hi
    rw [hIo r hrn] at hi
    rw [hS, if_neg hpn, if_neg (fun c => hrn c.2)]
    exact h.eligSync p r hp' he hi
  noSelfSync := by
    intro p; rw [hS]; split
    · rfl
    · split
      · rfl
      · exact h.noSelfSync p
  ackedPrim := by
    intro r hr
    show (restartEmpty s n).acked s.cfg.prim r = 0
    have := hAle s.cfg.prim r; rw [h.ackedPrim r hr] at this; omega
  ackedLe := by
    intro p r hp
    obtain ⟨hpn, hp'⟩ := htp p hp
    rw [hLo p hpn]; exact Nat.le_trans (hAle p r) (h.ackedLe p r hp')
  ackHolds := by
    intro p m hp hm hf
    obtain ⟨hpn, hp'⟩ := htp p hp
    rw [hLo p hpn]
    by_cases emn : m = n
    · subst emn
      have hpp : p = s.cfg.prim := by rw [← hf]; show (s.known m).prim = s.cfg.prim; rw [hkn]
      rw [hA, if_neg hpn, if_pos ⟨hpp, rfl⟩]; simp
    · rw [hLo m emn, hA, if_neg hpn, if_neg (fun c => emn c.2)]
      exact h.ackHolds p m hp' hm hf
  syncNew := by
    intro r hs he
    change (restartEmpty s n).sync s.cfg.prim r = true at hs
    by_cases ep : s.cfg.prim = n
    · rw [hS, if_pos ep] at hs; exact absurd hs (by decide)
    · have hrn : r ≠ n := by
        intro e; rw [hS, if_neg ep, if_pos ⟨rfl, e⟩] at hs; exact absurd hs (by decide)
      obtain ⟨a, b⟩ := h.syncNew r (hSle _ _ hs) he
      refine ⟨a, ?_⟩
      show (restartEmpty s n).recon s.cfg.prim ≤ (restartEmpty s n).acked s.cfg.prim r
      rw [hRo _ ep, hA, if_neg ep, if_neg (fun c => hrn c.2)]; exact b
  reconLen := by
    show (restartEmpty s n).recon s.cfg.prim ≤ ((restartEmpty s n).log s.cfg.prim).length
    by_cases ep : s.cfg.prim = n
    · rw [ep, hRn]; exact Nat.zero_le _
    · rw [hRo _ ep, hLo _ ep]; exact h.reconLen
  syncCommit := by
    intro q r hq hs
    obtain ⟨hqn, hq'⟩ := htp q hq
    rw [hCo q hqn]
    have hs' := hS q r; rw [hs, if_neg hqn] at hs'
    rw [hA, if_neg hqn]
    split
    · rename_i c; rw [if_pos c] at hs'; exact absurd hs' (by decide)
    · rename_i c; rw [if_neg c] at hs'; exact h.syncCommit q r hq' hs'.symm
  eligFol := h.eligFol
  histLe := by
    intro hp
    obtain ⟨hpn, hp'⟩ := hpa hp
    show s.hist.length ≤ (restartEmpty s n).commit s.cfg.prim ∨
      s.hist.length ≤ (restartEmpty s n).recon s.cfg.prim
    rw [hCo _ hpn, hRo _ hpn]; exact h.histLe hp'
  stale := by
    intro p hp hv
    obtain ⟨hpn, hp'⟩ := htp p hp
    have hpp := stale_ne_prim h hv
    have hSp : (restartEmpty s n).sync p = s.sync p := by
      funext r; rw [hS, if_neg hpn, if_neg (fun c => hpp c.1)]
    have hAp : ∀ r, (restartEmpty s n).acked p r = s.acked p r := by
      intro r; rw [hA, if_neg hpn, if_neg (fun c => hpp c.1)]
    refine (h.stale p hp' hv).transfer hSp (fun _ => rfl) (fun r _ => hAp r) (hLo p hpn) rfl
      (fun hq => (hpa hq).2) (fun hq => by rw [hRo _ (hpa hq).1])
      (fun hq => hLo _ (hpa hq).1) ?_
    intro m hm hi
    have hmn : m ≠ n := (hauth m ⟨Or.inl hm, hi⟩).1
    left
    refine ⟨hm, ?_, hLo m hmn⟩
    change s.cfg.inc m = (restartEmpty s n).inc m at hi
    rw [hIo m hmn] at hi; exact hi }

theorem inv_restartKeep {s : State Node} (h : Inv s) (n : Node) (hkl : KnowsLatest s n) :
    Inv (restartKeep true s n) := by
  have hkn : s.known n = s.cfg := h.knownEq n hkl
  have hCo : ∀ x, x ≠ n → (restartKeep true s n).commit x = s.commit x := fun _ => restartKeep_commit_other
  have hCn := restartKeep_commit_self (s := s) (n := n)
  have hRo : ∀ x, x ≠ n → (restartKeep true s n).recon x = s.recon x := fun _ => restartKeep_recon_other
  have hRn := restartKeep_recon_self (s := s) (n := n)
  have hAo : ∀ x, x ≠ n → (restartKeep true s n).acked x = s.acked x := fun _ => restartKeep_acked_other
  have hAn := restartKeep_acked_self (s := s) (n := n)
  have hSo : ∀ x, x ≠ n → (restartKeep true s n).sync x = s.sync x := fun _ => restartKeep_sync_other
  have hSn := restartKeep_sync_self (s := s) (n := n)
  -- a primary is never eligible in its own config
  have hnse : ThinksPrimary s n → (s.known n).elig n = true → (s.known n).inc n = s.inc n → False := by
    intro hp he hi; have := h.eligSync n n hp he hi; rw [h.noSelfSync] at this
    exact absurd this (by decide)
  have htp := restartKeep_thinksPrimary (s := s) (n := n)
  exact {
  ehc := h.ehc
  caLen := by
    intro x; by_cases e : x = n
    · subst e; rw [hCn]; exact Nat.zero_le _
    · rw [hCo x e]; exact h.caLen x
  caHist := by
    intro x; by_cases e : x = n
    · subst e; rw [hCn]; exact Nat.zero_le _
    · rw [hCo x e]; exact h.caHist x
  caEq := by
    intro x; by_cases e : x = n
    · subst e; rw [hCn]; rfl
    · rw [hCo x e]; exact h.caEq x
  knownLe := h.knownLe, knownEq := h.knownEq, primKnows := h.primKnows
  incLe := h.incLe, knownIncLe := h.knownIncLe, authEx := h.authEx
  eligSync := by
    intro p r hp he hi
    by_cases e : p = n
    · subst e
      have hc : (s.known p).prim = p ∧ (s.known p).inc p = s.inc p := hp
      rw [hSn, if_pos hc]
      have hrp : r ≠ p := by
        intro c; subst c; exact hnse hp he hi
      simp only [rebuiltSync, Bool.and_eq_true, decide_eq_true_eq]
      exact ⟨⟨he, hi⟩, hrp⟩
    · rw [hSo p e]; exact h.eligSync p r hp he hi
  noSelfSync := by
    intro p; by_cases e : p = n
    · subst e; rw [hSn]; split
      · simp [rebuiltSync]
      · rfl
    · rw [hSo p e]; exact h.noSelfSync p
  ackedPrim := by
    intro r hr
    show (restartKeep true s n).acked s.cfg.prim r = 0
    by_cases e : s.cfg.prim = n
    · rw [e, hAn]
    · rw [hAo _ e]; exact h.ackedPrim r hr
  ackedLe := by
    intro p r hp
    by_cases e : p = n
    · subst e; rw [hAn]; exact Nat.zero_le _
    · rw [hAo p e]; exact h.ackedLe p r hp
  ackHolds := by
    intro p m hp hm hf
    by_cases e : p = n
    · subst e; rw [hAn]; simp
    · rw [hAo p e]; exact h.ackHolds p m hp hm hf
  syncNew := by
    intro r hs he
    by_cases e : s.cfg.prim = n
    · exfalso
      have hs' : (restartKeep true s n).sync n r = true := by
        have := hs; change (restartKeep true s n).sync s.cfg.prim r = true at this
        rw [e] at this; exact this
      change s.cfg.elig r = false at he
      rw [hSn] at hs'
      split at hs'
      · simp only [rebuiltSync, hkn, Bool.and_eq_true] at hs'
        have := hs'.1.1; rw [he] at this; exact absurd this (by decide)
      · exact Bool.false_ne_true hs'
    · have hs' : s.sync s.cfg.prim r = true := by rw [← hSo _ e]; exact hs
      obtain ⟨a, b⟩ := h.syncNew r hs' he
      refine ⟨a, ?_⟩
      show (restartKeep true s n).recon s.cfg.prim ≤ (restartKeep true s n).acked s.cfg.prim r
      rw [hRo _ e, hAo _ e]; exact b
  reconLen := by
    show (restartKeep true s n).recon s.cfg.prim ≤ (s.log s.cfg.prim).length
    by_cases e : s.cfg.prim = n
    · rw [e, hRn]
    · rw [hRo _ e]; exact h.reconLen
  syncCommit := by
    intro q r hq hs
    by_cases e : q = n
    · subst e; rw [hCn]; exact Nat.zero_le _
    · rw [hCo q e, hAo q e]; rw [hSo q e] at hs; exact h.syncCommit q r hq hs
  eligFol := h.eligFol
  histLe := by
    intro hp
    show s.hist.length ≤ (restartKeep true s n).commit s.cfg.prim ∨
      s.hist.length ≤ (restartKeep true s n).recon s.cfg.prim
    by_cases e : s.cfg.prim = n
    · right; rw [e, hRn]; rw [← e]; exact (h.ehc _ ⟨Or.inr rfl, hp⟩).length_le
    · rw [hCo _ e, hRo _ e]; exact h.histLe hp
  stale := by
    intro p hp hv
    have hpn : p ≠ n := by
      intro c
      have hv' : (s.known p).ver < s.cfg.ver := hv
      rw [c] at hv'; unfold KnowsLatest at hkl; omega
    refine (h.stale p hp hv).transfer (hSo p hpn) (fun _ => rfl)
      (fun r _ => by rw [hAo p hpn]) rfl rfl id ?_ (fun _ => rfl)
      (fun m hm hi => Or.inl ⟨hm, hi, rfl⟩)
    intro _
    show s.recon s.cfg.prim ≤ (restartKeep true s n).recon s.cfg.prim
    by_cases e : s.cfg.prim = n
    · rw [e, hRn]; rw [← e]; exact h.reconLen
    · rw [hRo _ e] }

end RestartProof
