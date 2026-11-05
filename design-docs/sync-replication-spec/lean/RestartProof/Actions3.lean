import RestartProof.Actions2

/-! Preservation for Takeover and Resync. -/

namespace RestartProof

variable {Node : Type} [DecidableEq Node]

theorem inv_takeover {s : State Node} (h : Inv s) (n : Node) (hkl : kl s n)
    (hel : s.cfg.elig n = true) (hnp : n ≠ s.cfg.prim) (hin : s.cfg.inc n = s.inc n) :
    Inv (takeover s n) := by
  have hkn : s.known n = s.cfg := h.knownEq n hkl
  have hfn : (s.known n).prim = s.cfg.prim := by rw [hkn]
  have hntp : ¬ tp s n := by intro c; have := c.1; rw [hfn] at this; exact hnp this.symm
  have hK : ∀ x, (takeover s n).known x = if x = n then toCfg s n else s.known x := fun _ => rfl
  have hKo : ∀ x, x ≠ n → (takeover s n).known x = s.known x := by
    intro x hx; rw [hK, if_neg hx]
  have hSn : (takeover s n).sync n = newSync s n := by simp [takeover]
  have hSo : ∀ x, x ≠ n → (takeover s n).sync x = s.sync x := by
    intro x hx; simp [takeover, upd, hx]
  have hAn : (takeover s n).acked n = fun _ => 0 := by simp [takeover]
  have hAo : ∀ x, x ≠ n → (takeover s n).acked x = s.acked x := by
    intro x hx; simp [takeover, upd, hx]
  have hCn : (takeover s n).commit n = 0 := by simp [takeover]
  have hCo : ∀ x, x ≠ n → (takeover s n).commit x = s.commit x := by
    intro x hx; simp [takeover, upd, hx]
  have hRn : (takeover s n).recon n = (s.log n).length := by simp [takeover]
  have hNE : ∀ x, newElig s n x = true ↔ s.cfg.elig x = true ∧ x ≠ s.cfg.prim ∧ x ≠ n := by
    intro x; simp [newElig, and_assoc]
  have htp : ∀ x, tp (takeover s n) x ↔ x = n ∨ (x ≠ n ∧ tp s x) := by
    intro x; unfold tp; rw [hK]
    by_cases e : x = n
    · subst e; simp only [toCfg, true_or, iff_true]
      exact ⟨rfl, hin⟩
    · simp only [if_neg e]
      constructor
      · intro c; exact Or.inr ⟨e, c⟩
      · rintro (c | ⟨_, c⟩)
        · exact absurd c e
        · exact c
  have hehn : s.hist <+: s.log n := h.ehc n ⟨Or.inl hel, hin⟩
  exact {
  ehc := by
    intro m ⟨hm, hi⟩
    change newElig s n m = true ∨ m = n at hm
    change s.cfg.inc m = s.inc m at hi
    rcases hm with hm | rfl
    · exact h.ehc m ⟨Or.inl ((hNE m).1 hm).1, hi⟩
    · exact hehn
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
  knownLe := by
    intro x; show ((takeover s n).known x).ver ≤ s.cfg.ver + 1; rw [hK]; split
    · exact Nat.le_refl _
    · exact Nat.le_succ_of_le (h.knownLe x)
  knownEq := by
    intro x hx; change ((takeover s n).known x).ver = s.cfg.ver + 1 at hx
    show (takeover s n).known x = toCfg s n
    rw [hK] at hx ⊢; split
    · rfl
    · rename_i e; rw [if_neg e] at hx; have := h.knownLe x; omega
  primKnows := by show (takeover s n).known n = toCfg s n; rw [hK, if_pos rfl]
  incLe := h.incLe
  knownIncLe := by
    intro x y; rw [hK]; split
    · exact h.incLe y
    · exact h.knownIncLe x y
  authEx := ⟨n, Or.inr rfl, hin⟩
  eligSync := by
    intro x y hx hy hi
    rcases (htp x).1 hx with rfl | ⟨e, hx'⟩
    · rw [hSn]; rw [hK, if_pos rfl] at hy hi
      change newElig s x y = true at hy
      change s.cfg.inc y = s.inc y at hi
      simp [newSync, hy, hi]
    · rw [hSo x e]; rw [hKo x e] at hy hi; exact h.eligSync x y hx' hy hi
  noSelfSync := by
    intro x; by_cases e : x = n
    · subst e; rw [hSn]; simp [newSync, newElig]
    · rw [hSo x e]; exact h.noSelfSync x
  ackedPrim := by intro r _; show (takeover s n).acked n r = 0; rw [hAn]
  ackedLe := by
    intro x y hx
    rcases (htp x).1 hx with rfl | ⟨e, hx'⟩
    · rw [hAn]; exact Nat.zero_le _
    · rw [hAo x e]; exact h.ackedLe x y hx'
  ackHolds := by
    intro x y hx hy hf
    rcases (htp x).1 hx with rfl | ⟨e, hx'⟩
    · rw [hAn]; simp
    · by_cases ey : y = n
      · subst ey; rw [hK, if_pos rfl] at hf; exact absurd hf.symm e
      · rw [hKo y ey] at hf; rw [hAo x e]; exact h.ackHolds x y hx' hy hf
  syncNew := by
    intro r hs he
    change (takeover s n).sync n r = true at hs
    rw [hSn] at hs
    change newElig s n r = false at he
    simp [newSync, he] at hs
  syncCommit := by
    intro x y hx hs
    rcases (htp x).1 hx with rfl | ⟨e, hx'⟩
    · rw [hCn]; exact Nat.zero_le _
    · rw [hCo x e, hAo x e]; rw [hSo x e] at hs; exact h.syncCommit x y hx' hs
  eligFol := by
    intro m hm hf
    change newElig s n m = true at hm
    obtain ⟨hm1, hm2, hm3⟩ := (hNE m).1 hm
    rw [hKo m hm3] at hf ⊢
    change (s.known m).prim ≠ n at hf
    rw [hKo _ hf]
    by_cases hq : (s.known m).prim = s.cfg.prim
    · rw [hq, h.primKnows]; exact hm1
    · exact h.eligFol m hm1 hq
  histLe := by intro _; right; show s.hist.length ≤ (takeover s n).recon n; rw [hRn]; exact hehn.length_le
  reconLen := by show (takeover s n).recon n ≤ (s.log n).length; rw [hRn]; exact Nat.le_refl _
  stale := by
    intro x hx hv
    rcases (htp x).1 hx with rfl | ⟨e, hx'⟩
    · rw [hK, if_pos rfl] at hv; exact absurd hv (Nat.lt_irrefl _)
    have hSx := hSo x e
    have hAx := hAo x e
    -- logs are unchanged
    have hL : (takeover s n).log = s.log := rfl
    by_cases hxq : x = s.cfg.prim
    · -- the deposed primary: build a fresh witness
      subst hxq
      have hkq := h.primKnows
      have hpa : pa s := (tp_pa hx' hkq).2
      by_cases hex : ∃ r, s.sync s.cfg.prim r = true ∧ (s.known r).prim ≠ s.cfg.prim ∧ r ≠ n
      · obtain ⟨r, hr1, hr2, hr3⟩ := hex
        have h0 := h.ackedPrim r hr2
        refine ⟨r, ?_, ?_, ?_, ?_, ?_, ?_⟩
        · rw [hSx]; exact hr1
        · rw [hKo r hr3]; exact hr2
        · intro _ _ _; rw [hAx, h0]; exact Nat.zero_le _
        · intro _; rw [hAx, h0]; exact Nat.zero_le _
        · intro _; rw [hAx, h0]; simp
        · intro m _ _; left; rw [hAx, h0]; simp
      · have hsn : s.sync s.cfg.prim n = true :=
          h.eligSync _ n (tp_prim h hpa) (by rw [hkq]; exact hel) (by rw [hkq]; exact hin)
        have hhold := h.ackHolds _ _ (tp_prim h hpa) hnp hfn
        have hlen : s.acked s.cfg.prim n ≤ (s.log s.cfg.prim).length := h.ackedLe _ _ (tp_prim h hpa)
        refine ⟨n, ?_, ?_, ?_, ?_, ?_, ?_⟩
        · rw [hSx]; exact hsn
        · rw [hK, if_pos rfl]; exact hnp
        · intro r hr hf
          rw [hSx] at hr
          by_cases er : r = n
          · subst er; exact Nat.le_refl _
          · rw [hKo r er] at hf; exact absurd ⟨r, hr, hf, er⟩ hex
        · intro _
          change (takeover s n).acked s.cfg.prim n ≤ (takeover s n).recon n
          rw [hAx, hRn]
          have := hhold.length_le; rw [length_take_of_le hlen] at this; exact this
        · intro _
          change ((takeover s n).log s.cfg.prim).take ((takeover s n).acked s.cfg.prim n) <+:
            (takeover s n).log n
          rw [hAx, hL]; exact hhold
        · intro m hm hi
          change newElig s n m = true at hm
          change s.cfg.inc m = s.inc m at hi
          obtain ⟨hm1, hm2, hm3⟩ := (hNE m).1 hm
          have hsm := h.eligSync _ m (tp_prim h hpa) (by rw [hkq]; exact hm1) (by rw [hkq]; exact hi)
          right
          refine ⟨by rw [hSx]; exact hsm, ?_⟩
          rw [hKo m hm3]
          by_cases hf : (s.known m).prim = s.cfg.prim
          · exact hf
          · exact absurd ⟨m, hsm, hf, hm3⟩ hex
    · -- an already stale primary keeps its witness
      have hv' : (s.known x).ver < s.cfg.ver := by
        rw [hKo x e] at hv
        rcases Nat.lt_or_ge (s.known x).ver s.cfg.ver with a | a
        · exact a
        · exfalso; have := h.knownEq x (Nat.le_antisymm (h.knownLe x) a)
          exact hxq (tp_kl h hx' (by unfold kl; rw [this])).2.symm
      obtain ⟨w, hw1, hw2, hw3, hw4, hw5, hw6⟩ := h.stale x hx' hv'
      have hnx : (s.known n).prim ≠ x := by rw [hfn]; exact fun c => hxq c.symm
      -- the new primary held x's fenced prefix as an eligible replica
      have hn6 : (s.log x).take (s.acked x w) <+: s.log n := by
        rcases hw6 n hel hin with d | ⟨_, d2⟩
        · exact d
        · exact absurd d2 hnx
      refine ⟨w, ?_, ?_, ?_, ?_, ?_, ?_⟩
      · rw [hSx]; exact hw1
      · show ((takeover s n).known w).prim ≠ x
        rw [hK]; split
        · show n ≠ x; exact fun c => e c.symm
        · exact hw2
      · intro r hr hf
        rw [hSx] at hr; rw [hAx]
        by_cases er : r = n
        · subst er; exact hw3 r hr hnx
        · rw [hKo r er] at hf; exact hw3 r hr hf
      · intro _
        change (takeover s n).acked x w ≤ (takeover s n).recon n
        rw [hAx, hRn]
        have := hn6.length_le; rw [length_take_of_le (h.ackedLe x w hx')] at this; exact this
      · intro _
        change ((takeover s n).log x).take ((takeover s n).acked x w) <+: (takeover s n).log n
        rw [hAx, hL]; exact hn6
      · intro m hm hi
        change newElig s n m = true at hm
        change s.cfg.inc m = s.inc m at hi
        obtain ⟨hm1, _, hm3⟩ := (hNE m).1 hm
        rw [hAx, hSx, hKo m hm3]
        exact hw6 m hm1 hi }

theorem cup_le_len {s : State Node} (h : Inv s) (p : Node) : cup true s p ≤ (s.log p).length := by
  unfold cup; split
  · exact Nat.min_le_right _ _
  · exact h.caLen p

/-- Resync copies the primary's log up to its CatchUpPoint (the inherited log while it
is unreconciled). -/
theorem inv_resync {s : State Node} (h : Inv s) (r : Node) (hne : pof s r ≠ r)
    (hsy : s.sync (pof s r) r = false) (hke : (s.known (pof s r)).elig r = false) :
    Inv (resync true s r) := by
  have hcl := commit_le_cup h (pof s r)
  have hcu := cup_le_len h (pof s r)
  -- r is not eligible and not the primary
  have hntp : ¬ tp s r := fun c => hne c.1
  have hrprim : r ≠ s.cfg.prim := by
    intro e; apply hne; show (s.known r).prim = r; rw [e, h.primKnows]
  have hrel : s.cfg.elig r = false := by
    cases hc : s.cfg.elig r
    · rfl
    · exfalso
      by_cases hf : (s.known r).prim = s.cfg.prim
      · have : pof s r = s.cfg.prim := hf
        rw [this, h.primKnows, hc] at hke; exact absurd hke (by decide)
      · have := h.eligFol r hc hf
        change (s.known (pof s r)).elig r = true at this
        rw [hke] at this; exact absurd this (by decide)
  have hLr : (resync true s r).log r = (s.log (pof s r)).take (cup true s (pof s r)) := by
    simp [resync]
  have hLo : ∀ x, x ≠ r → (resync true s r).log x = s.log x := by intro x hx; simp [resync, upd, hx]
  have hCr : (resync true s r).commit r = min (s.commit r) (s.commit (pof s r)) := by simp [resync]
  have hCo : ∀ x, x ≠ r → (resync true s r).commit x = s.commit x := by
    intro x hx; simp [resync, upd, hx]
  have hA : ∀ q x, (resync true s r).acked q x = if q = pof s r ∧ x = r then 0 else s.acked q x := by
    intro q x
    by_cases hq : q = pof s r
    · subst hq; by_cases hx : x = r
      · subst hx; simp [resync, upd]
      · simp [resync, upd, hx]
    · simp [resync, upd, hq]
  have hAle : ∀ q x, (resync true s r).acked q x ≤ s.acked q x := by
    intro q x; rw [hA]; split
    · exact Nat.zero_le _
    · exact Nat.le_refl _
  have hAs : ∀ q x, s.sync q x = true → (resync true s r).acked q x = s.acked q x := by
    intro q x hs; rw [hA]; split
    · rename_i c; obtain ⟨rfl, rfl⟩ := c; rw [hsy] at hs; exact absurd hs (by decide)
    · rfl
  have hyr : pof s r ≠ r := hne
  have hnotr : ∀ x, (s.cfg.elig x = true ∨ x = s.cfg.prim) → x ≠ r := by
    intro x hx e; subst e
    rcases hx with hx | hx
    · rw [hrel] at hx; exact absurd hx (by decide)
    · exact hrprim hx
  have htpne : ∀ x, tp s x → x ≠ r := by intro x hx e; subst e; exact hntp hx
  exact {
  ehc := by
    intro x hx; rw [hLo x (hnotr x hx.1)]; exact h.ehc x hx
  caLen := by
    intro x; by_cases e : x = r
    · subst e; rw [hCr, hLr, length_take_of_le hcu]; omega
    · rw [hCo x e, hLo x e]; exact h.caLen x
  caHist := by
    intro x; by_cases e : x = r
    · subst e; rw [hCr]; exact Nat.le_trans (Nat.min_le_left _ _) (h.caHist _)
    · rw [hCo x e]; exact h.caHist x
  caEq := by
    intro x; by_cases e : x = r
    · subst e
      show ((resync true s x).log x).take ((resync true s x).commit x) = s.hist.take ((resync true s x).commit x)
      rw [hCr, hLr, List.take_take]
      have hk0 : min (min (s.commit x) (s.commit (pof s x))) (cup true s (pof s x)) =
          min (s.commit x) (s.commit (pof s x)) := by omega
      have hk : min (min (s.commit x) (s.commit (pof s x))) (s.commit (pof s x)) =
          min (s.commit x) (s.commit (pof s x)) := by omega
      rw [hk0]
      rw [show (s.log (pof s x)).take (min (s.commit x) (s.commit (pof s x))) =
          ((s.log (pof s x)).take (s.commit (pof s x))).take (min (s.commit x) (s.commit (pof s x)))
          by rw [List.take_take]; congr 1; omega]
      rw [h.caEq (pof s x), List.take_take, hk]
    · rw [hCo x e, hLo x e]; exact h.caEq x
  knownLe := h.knownLe, knownEq := h.knownEq, primKnows := h.primKnows
  incLe := h.incLe, authEx := h.authEx, knownIncLe := h.knownIncLe
  eligSync := h.eligSync, noSelfSync := h.noSelfSync
  ackedPrim := by intro x hx; show (resync true s r).acked s.cfg.prim x = 0; have := hAle s.cfg.prim x; rw [h.ackedPrim x hx] at this; omega
  ackedLe := by
    intro p x hp
    show (resync true s r).acked p x ≤ ((resync true s r).log p).length
    rw [hLo p (htpne p hp)]; exact Nat.le_trans (hAle p x) (h.ackedLe p x hp)
  ackHolds := by
    intro p m hp hm hf
    show ((resync true s r).log p).take ((resync true s r).acked p m) <+: (resync true s r).log m
    rw [hLo p (htpne p hp), hA]
    split
    · simp
    · rename_i c
      by_cases em : m = r
      · subst em; exact absurd ⟨hf.symm, rfl⟩ c
      · rw [hLo m em]; exact h.ackHolds p m hp hm hf
  syncNew := by
    intro x hs he
    obtain ⟨a, b⟩ := h.syncNew x hs he
    refine ⟨a, ?_⟩
    show s.recon s.cfg.prim ≤ (resync true s r).acked s.cfg.prim x
    rw [hAs _ _ (show s.sync s.cfg.prim x = true from hs)]; exact b
  syncCommit := by
    intro q x hq hs
    rw [hCo q (htpne q hq), hAs q x hs]; exact h.syncCommit q x hq hs
  eligFol := h.eligFol
  histLe := by
    intro hpa
    show s.hist.length ≤ (resync true s r).commit s.cfg.prim ∨ _
    rw [hCo _ (Ne.symm hrprim)]; exact h.histLe hpa
  reconLen := by
    show _ ≤ ((resync true s r).log s.cfg.prim).length
    rw [hLo _ (Ne.symm hrprim)]; exact h.reconLen
  stale := by
    intro x hx hv
    obtain ⟨w, hw⟩ := h.stale x hx hv
    have hxr := htpne x hx
    exact ⟨w, wit_transfer hw rfl (fun _ => rfl) (hAs x w hw.1) (hAs x) (hLo x hxr) rfl id
      (fun _ => Nat.le_refl _) (fun _ => hLo _ (Ne.symm hrprim))
      (fun m hm hi => Or.inl ⟨hm, hi, hLo m (hnotr m (Or.inl hm))⟩)⟩ }

end RestartProof
