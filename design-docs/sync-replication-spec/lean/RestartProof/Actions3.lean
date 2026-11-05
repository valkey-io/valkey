import RestartProof.Actions2

/-! Preservation for Takeover and Resync. -/

namespace RestartProof

variable {Node : Type} [DecidableEq Node]

theorem inv_takeover {s : State Node} (h : Inv s) (n : Node) (hkl : KnowsLatest s n)
    (hel : s.cfg.elig n = true) (hnp : n ≠ s.cfg.prim) (hin : s.cfg.inc n = s.inc n) :
    Inv (takeover s n) := by
  have hkn : s.known n = s.cfg := h.knownEq n hkl
  have hfn : (s.known n).prim = s.cfg.prim := by rw [hkn]
  have hntp : ¬ ThinksPrimary s n := by intro c; have := c.1; rw [hfn] at this; exact hnp this.symm
  -- The takeover contract (`Contracts.lean`): n starts a new primaryship, logs do not
  -- move, every other node keeps its state.
  have hK : ∀ x, (takeover s n).known x = if x = n then takeoverCfg s n else s.known x :=
    takeover_known
  have hKo : ∀ x, x ≠ n → (takeover s n).known x = s.known x := fun _ => takeover_known_other
  have hSn : (takeover s n).sync n = takeoverSync s n := takeover_sync_self
  have hSo : ∀ x, x ≠ n → (takeover s n).sync x = s.sync x := fun _ => takeover_sync_other
  have hAn : (takeover s n).acked n = fun _ => 0 := takeover_acked_self
  have hAo : ∀ x, x ≠ n → (takeover s n).acked x = s.acked x := fun _ => takeover_acked_other
  have hCn : (takeover s n).commit n = 0 := takeover_commit_self
  have hCo : ∀ x, x ≠ n → (takeover s n).commit x = s.commit x := fun _ => takeover_commit_other
  have hRn : (takeover s n).recon n = (s.log n).length := takeover_recon_self
  have hNE := takeoverElig_iff (s := s) (n := n)
  have htp := takeover_thinksPrimary hin
  have hehn : s.hist <+: s.log n := h.ehc n ⟨Or.inl hel, hin⟩
  exact {
  ehc := by
    intro m ⟨hm, hi⟩
    change takeoverElig s n m = true ∨ m = n at hm
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
    show (takeover s n).known x = takeoverCfg s n
    rw [hK] at hx ⊢; split
    · rfl
    · rename_i e; rw [if_neg e] at hx; have := h.knownLe x; omega
  primKnows := by show (takeover s n).known n = takeoverCfg s n; rw [hK, if_pos rfl]
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
      change takeoverElig s x y = true at hy
      change s.cfg.inc y = s.inc y at hi
      exact (takeoverSync_iff y).2 ⟨hy, hi⟩
    · rw [hSo x e]; rw [hKo x e] at hy hi; exact h.eligSync x y hx' hy hi
  noSelfSync := by
    intro x; by_cases e : x = n
    · subst e; rw [hSn]; exact takeoverSync_self
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
    change takeoverElig s n r = false at he
    rw [takeoverSync_iff, he] at hs; exact absurd hs.1 (by decide)
  syncCommit := by
    intro x y hx hs
    rcases (htp x).1 hx with rfl | ⟨e, hx'⟩
    · rw [hCn]; exact Nat.zero_le _
    · rw [hCo x e, hAo x e]; rw [hSo x e] at hs; exact h.syncCommit x y hx' hs
  eligFol := by
    intro m hm hf
    change takeoverElig s n m = true at hm
    obtain ⟨hm1, hm2, hm3⟩ := (hNE m).1 hm
    rw [hKo m hm3] at hf ⊢
    change (s.known m).prim ≠ n at hf
    rw [hKo _ hf]
    by_cases hq : (s.known m).prim = s.cfg.prim
    · rw [hq, h.primKnows]; exact hm1
    · exact h.eligFol m hm1 hq
  histLe := by intro _; right; show s.hist.length ≤ (takeover s n).recon n; rw [hRn]; exact hehn.length_le
  reconLen := by show (takeover s n).recon n ≤ (s.log n).length; rw [hRn]
  stale := by
    -- Every node that still thinks it is primary under an old configuration is fenced.
    intro x hx hv
    rcases (htp x).1 hx with rfl | ⟨e, hx'⟩
    · rw [hK, if_pos rfl] at hv; exact absurd hv (Nat.lt_irrefl _)
    -- x keeps its log, sync replica set and acknowledgements.
    have hSx := hSo x e
    have hAx := hAo x e
    have hL : (takeover s n).log = s.log := rfl
    have hleft : ∀ r, s.sync x r = true → ((takeover s n).known r).prim ≠ x →
        r ∈ leftSync (takeover s n) x := fun r hr hf => ⟨by rw [hSx]; exact hr, hf⟩
    by_cases hxq : x = s.cfg.prim
    · -- x is the primary n took over from. n was eligible, so x counts n, and n now
      -- follows itself: n has left x.
      subst hxq
      have hpa : PrimaryAuthoritative s := (thinksPrimary_primaryAuthoritative hx' h.primKnows).2
      have hxp := thinksPrimary_prim h hpa
      by_cases hex : ∃ r, s.sync s.cfg.prim r = true ∧ (s.known r).prim ≠ s.cfg.prim ∧ r ≠ n
      · -- Another sync replica had already left x. The current primary records no
        -- acknowledgement from such a replica, so the fence is 0.
        obtain ⟨r, hr1, hr2, hr3⟩ := hex
        have h0 : (takeover s n).acked s.cfg.prim r = 0 := by rw [hAx]; exact h.ackedPrim r hr2
        refine Fenced.of_left (hleft r hr1 (by rw [hKo r hr3]; exact hr2)) ?_ ?_ ?_ <;> simp [h0]
      · -- n is the only replica that left x: the fence is n's acknowledgement, and n
        -- holds x's log up to it.
        have hsn : s.sync s.cfg.prim n = true :=
          h.eligSync _ n hxp (by rw [h.primKnows]; exact hel) (by rw [h.primKnows]; exact hin)
        have hhold := h.ackHolds _ _ hxp hnp hfn
        have hlen := h.ackedLe s.cfg.prim n hxp
        have hnx : ((takeover s n).known n).prim ≠ s.cfg.prim := by
          rw [hK, if_pos rfl]; exact hnp
        refine Fenced.of_left (hleft n hsn hnx) ?_ ?_ ?_
        · intro _
          change (takeover s n).acked s.cfg.prim n ≤ (takeover s n).recon n
          rw [hAx, hRn]
          have := hhold.length_le; rw [List.length_take_of_le hlen] at this; exact this
        · intro _
          change ((takeover s n).log s.cfg.prim).take ((takeover s n).acked s.cfg.prim n) <+:
            (takeover s n).log n
          rw [hAx, hL]; exact hhold
        · -- Every other eligible replica is counted by x and still follows it.
          intro m hm hi
          change takeoverElig s n m = true at hm
          change s.cfg.inc m = s.inc m at hi
          obtain ⟨hm1, _, hm3⟩ := (hNE m).1 hm
          have hsm := h.eligSync _ m hxp (by rw [h.primKnows]; exact hm1) (by rw [h.primKnows]; exact hi)
          right
          refine ⟨by rw [hSx]; exact hsm, ?_⟩
          rw [hKo m hm3]
          by_cases hf : (s.known m).prim = s.cfg.prim
          · exact hf
          · exact absurd ⟨m, hsm, hf, hm3⟩ hex
    · -- x was already deposed. Its old fence still works: the replica that set it has
      -- still left x, and n, being eligible, already held x's log up to the fence.
      have hv' : (s.known x).ver < s.cfg.ver := by
        rw [hKo x e] at hv
        rcases Nat.lt_or_ge (s.known x).ver s.cfg.ver with a | a
        · exact a
        · exfalso; have := h.knownEq x (Nat.le_antisymm (h.knownLe x) a)
          exact hxq (thinksPrimary_knowsLatest h hx' (by unfold KnowsLatest; rw [this])).2.symm
      have hF := h.stale x hx' hv'
      obtain ⟨w, ⟨hw1, hw2⟩, hwF⟩ := hF.attained
      have hn6 : (s.log x).take (fence s x) <+: s.log n := by
        rcases hF.eligHolds n hel hin with d | ⟨_, d2⟩
        · exact d
        · rw [hfn] at d2; exact absurd d2.symm hxq
      have hw' : w ∈ leftSync (takeover s n) x := hleft w hw1 (by
        rw [hK]; split
        · show n ≠ x; exact fun c => e c.symm
        · exact hw2)
      refine Fenced.of_left hw' ?_ ?_ ?_
      · intro _
        change (takeover s n).acked x w ≤ (takeover s n).recon n
        rw [hAx, hwF, hRn]
        have := hn6.length_le
        rw [List.length_take_of_le (hwF ▸ h.ackedLe x w hx')] at this; exact this
      · intro _
        change ((takeover s n).log x).take ((takeover s n).acked x w) <+: (takeover s n).log n
        rw [hAx, hwF, hL]; exact hn6
      · intro m hm hi
        change takeoverElig s n m = true at hm
        change s.cfg.inc m = s.inc m at hi
        obtain ⟨hm1, _, hm3⟩ := (hNE m).1 hm
        rw [hAx, hwF, hSx, hKo m hm3]
        exact hF.eligHolds m hm1 hi }

theorem catchUpPoint_le_len {s : State Node} (h : Inv s) (p : Node) : catchUpPoint true s p ≤ (s.log p).length := by
  unfold catchUpPoint; split
  · exact Nat.min_le_right _ _
  · exact h.caLen p

/-- Resync copies the primary's log up to its CatchUpPoint (the inherited log while it
is unreconciled). -/
theorem inv_resync {s : State Node} (h : Inv s) (r : Node) (hne : primaryOf s r ≠ r)
    (hsy : s.sync (primaryOf s r) r = false) (hke : (s.known (primaryOf s r)).elig r = false) :
    Inv (resync true s r) := by
  have hcl := commit_le_catchUpPoint h (primaryOf s r)
  have hcu := catchUpPoint_le_len h (primaryOf s r)
  -- r is not eligible and not the primary
  have hntp : ¬ ThinksPrimary s r := fun c => hne c.1
  have hrprim : r ≠ s.cfg.prim := by
    intro e; apply hne; show (s.known r).prim = r; rw [e, h.primKnows]
  have hrel : s.cfg.elig r = false := by
    cases hc : s.cfg.elig r
    · rfl
    · exfalso
      by_cases hf : (s.known r).prim = s.cfg.prim
      · have : primaryOf s r = s.cfg.prim := hf
        rw [this, h.primKnows, hc] at hke; exact absurd hke (by decide)
      · have := h.eligFol r hc hf
        change (s.known (primaryOf s r)).elig r = true at this
        rw [hke] at this; exact absurd this (by decide)
  have hLr := resync_log_self (s := s) (r := r)
  have hLo : ∀ x, x ≠ r → (resync true s r).log x = s.log x := fun _ => resync_log_other
  have hCr := resync_commit_self (s := s) (r := r)
  have hCo : ∀ x, x ≠ r → (resync true s r).commit x = s.commit x := fun _ => resync_commit_other
  have hA := resync_acked (s := s) (r := r)
  have hAle := resync_acked_le (s := s) (r := r)
  have hAs : ∀ q x, s.sync q x = true → (resync true s r).acked q x = s.acked q x :=
    fun _ _ => resync_acked_sync hsy
  have hyr : primaryOf s r ≠ r := hne
  have hnotr : ∀ x, (s.cfg.elig x = true ∨ x = s.cfg.prim) → x ≠ r := by
    intro x hx e; subst e
    rcases hx with hx | hx
    · rw [hrel] at hx; exact absurd hx (by decide)
    · exact hrprim hx
  have htpne : ∀ x, ThinksPrimary s x → x ≠ r := by intro x hx e; subst e; exact hntp hx
  exact {
  ehc := by
    intro x hx; rw [hLo x (hnotr x hx.1)]; exact h.ehc x hx
  caLen := by
    intro x; by_cases e : x = r
    · subst e; rw [hCr, hLr, List.length_take_of_le hcu]; omega
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
      have hk0 : min (min (s.commit x) (s.commit (primaryOf s x))) (catchUpPoint true s (primaryOf s x)) =
          min (s.commit x) (s.commit (primaryOf s x)) := by omega
      have hk : min (min (s.commit x) (s.commit (primaryOf s x))) (s.commit (primaryOf s x)) =
          min (s.commit x) (s.commit (primaryOf s x)) := by omega
      rw [hk0]
      rw [show (s.log (primaryOf s x)).take (min (s.commit x) (s.commit (primaryOf s x))) =
          ((s.log (primaryOf s x)).take (s.commit (primaryOf s x))).take (min (s.commit x) (s.commit (primaryOf s x)))
          by rw [List.take_take]; congr 1; omega]
      rw [h.caEq (primaryOf s x), List.take_take, hk]
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
    have hxr := htpne x hx
    exact (h.stale x hx hv).transfer rfl (fun _ => rfl) (hAs x) (hLo x hxr) rfl id
      (fun _ => Nat.le_refl _) (fun _ => hLo _ (Ne.symm hrprim))
      (fun m hm hi => Or.inl ⟨hm, hi, hLo m (hnotr m (Or.inl hm))⟩) }

end RestartProof
