import RestartProof.Actions1

/-! Preservation for Learn, MakeEligible, MakeIneligible, Takeover, Resync. -/

namespace RestartProof

variable {Node : Type} [DecidableEq Node]

theorem upd_eq_ite {α β : Type} [DecidableEq α] (f : α → β) (a : α) (b : β) (x : α) :
    upd f a b x = if x = a then b else f x := rfl

/-! ### Learn -/

theorem inv_learn {s : State Node} (h : Inv s) (n : Node) (hk : ¬ KnowsLatest s n) : Inv (learn s n) := by
  have hnp : n ≠ s.cfg.prim := by
    intro e; apply hk; unfold KnowsLatest; rw [e, h.primKnows]
  have hK := learn_known (s := s) (n := n)
  have hSn : (learn s n).sync n = fun _ => false := learn_sync_self hnp
  have hS : ∀ x, x ≠ n → (learn s n).sync x = s.sync x := fun _ => learn_sync_other
  have htp : ∀ x, ThinksPrimary (learn s n) x → x ≠ n ∧ ThinksPrimary s x :=
    fun _ => learn_thinksPrimary hnp
  exact {
  ehc := h.ehc, caLen := h.caLen, caHist := h.caHist, caEq := h.caEq
  knownLe := by
    intro x; show ((learn s n).known x).ver ≤ s.cfg.ver; rw [hK]; split
    · exact Nat.le_refl _
    · exact h.knownLe x
  knownEq := by
    intro x hx; show (learn s n).known x = s.cfg; rw [hK] at hx ⊢; split
    · rfl
    · rename_i e; rw [if_neg e] at hx; exact h.knownEq x hx
  primKnows := by show (learn s n).known s.cfg.prim = s.cfg; rw [hK, if_neg (Ne.symm hnp)]; exact h.primKnows
  eligSync := by
    intro p r hp he hi
    obtain ⟨hpn, hp'⟩ := htp p hp
    rw [hS p hpn]; rw [hK, if_neg hpn] at he hi; exact h.eligSync p r hp' he hi
  noSelfSync := by
    intro p; by_cases e : p = n
    · subst e; rw [hSn]
    · rw [hS p e]; exact h.noSelfSync p
  ackedPrim := by
    intro r hr; change (if r = n then s.cfg else s.known r).prim ≠ s.cfg.prim at hr
    show s.acked s.cfg.prim r = 0
    split at hr
    · exact absurd rfl hr
    · exact h.ackedPrim r hr
  ackedLe := by intro p r hp; exact h.ackedLe p r (htp p hp).2
  ackHolds := by
    intro p m hp hm hf
    obtain ⟨hpn, hp'⟩ := htp p hp
    change (if m = n then s.cfg else s.known m).prim = p at hf
    show (s.log p).take (s.acked p m) <+: s.log m
    split at hf
    · rename_i e; subst e
      by_cases hf0 : (s.known m).prim = p
      · exact h.ackHolds p m hp' hm hf0
      · rw [← hf] at hf0 ⊢; rw [h.ackedPrim m hf0]; simp
    · exact h.ackHolds p m hp' hm hf
  syncNew := by
    intro r hs he
    have hs0 : (learn s n).sync s.cfg.prim r = true := hs
    rw [hS _ (Ne.symm hnp)] at hs0
    have hs := hs0
    show _ ∧ s.recon s.cfg.prim ≤ s.acked s.cfg.prim r
    obtain ⟨a, b⟩ := h.syncNew r hs he
    refine ⟨?_, b⟩
    show (if r = n then s.cfg else s.known r).prim = s.cfg.prim
    split
    · rfl
    · exact a
  reconLen := h.reconLen
  syncCommit := by
    intro q r hq hs
    obtain ⟨hqn, hq'⟩ := htp q hq
    rw [hS q hqn] at hs; exact h.syncCommit q r hq' hs
  eligFol := by
    intro m he hf
    change (if m = n then s.cfg else s.known m).prim ≠ s.cfg.prim at hf
    show ((learn s n).known ((learn s n).known m).prim).elig m = true
    rw [hK m]
    split at hf
    · exact absurd rfl hf
    · rename_i e
      rw [if_neg e, hK]
      split
      · exact he
      · exact h.eligFol m he hf
  histLe := h.histLe
  incLe := h.incLe
  authEx := h.authEx
  knownIncLe := by
    intro x y; rw [hK]; split
    · exact h.incLe y
    · exact h.knownIncLe x y
  stale := by
    intro p hp hv
    obtain ⟨hpn, hp'⟩ := htp p hp
    have hv' : (s.known p).ver < s.cfg.ver := by
      have := hv; change ((if p = n then s.cfg else s.known p).ver < s.cfg.ver) at this
      rwa [if_neg hpn] at this
    have hprim := stale_ne_prim h hv'
    have hF := h.stale p hp' hv'
    obtain ⟨w, ⟨hw1, hw2⟩, hwF⟩ := hF.attained
    -- Learn moves no log, acknowledgement or configuration; only whom n follows.
    have hfz : ∀ r, ((learn s n).known r).prim ≠ p ↔ (r = n ∨ (s.known r).prim ≠ p) := by
      intro r; rw [hK]; by_cases e : r = n
      · subst e; simp; exact fun x => hprim x.symm
      · simp [e]
    have hSp : (learn s n).sync p = s.sync p := hS p hpn
    by_cases hsw : s.sync p n = true ∧ (s.known n).prim = p ∧ s.acked p n < fence s p
    · -- n followed p with an acknowledgement below the fence. Having learned the new
      -- configuration it has left p, and its acknowledgement is the new fence.
      obtain ⟨hs1, hs2, hs3⟩ := hsw
      have hnp' : n ≠ p := fun e => hpn e.symm
      refine Fenced.of_left (w := n) ⟨by rw [hSp]; exact hs1, (hfz n).2 (Or.inl rfl)⟩ ?_ ?_ ?_
      · intro hpa; show s.acked p n ≤ s.recon s.cfg.prim; have := hF.reconLe hpa; omega
      · intro hpa; exact take_mono_prefix (Nat.le_of_lt hs3) (hF.primHolds hpa)
      · intro m hm hi
        rcases hF.eligHolds m hm hi with d | ⟨d1, d2⟩
        · exact Or.inl (take_mono_prefix (Nat.le_of_lt hs3) d)
        · by_cases e : m = n
          · subst e; exact Or.inl (h.ackHolds p m hp' hnp' hs2)
          · right; rw [hSp]; refine ⟨d1, ?_⟩
            show (if m = n then s.cfg else s.known m).prim = p; rw [if_neg e]; exact d2
    · -- Otherwise the old fence still holds.
      refine Fenced.of_left (w := w) ⟨by rw [hSp]; exact hw1, (hfz w).2 (Or.inr hw2)⟩ ?_ ?_ ?_
      · intro hpa; show s.acked p w ≤ s.recon s.cfg.prim; rw [hwF]; exact hF.reconLe hpa
      · intro hpa
        show (s.log p).take (s.acked p w) <+: s.log s.cfg.prim; rw [hwF]; exact hF.primHolds hpa
      · intro m hm hi
        show (s.log p).take (s.acked p w) <+: s.log m ∨ _
        rw [hwF]
        rcases hF.eligHolds m hm hi with d | ⟨d1, d2⟩
        · exact Or.inl d
        · by_cases e : m = n
          · subst e
            have hnp' : m ≠ p := fun e => hpn e.symm
            have hle : fence s p ≤ s.acked p m := Nat.le_of_not_lt (fun hl => hsw ⟨d1, d2, hl⟩)
            exact Or.inl (take_mono_prefix hle (h.ackHolds p m hp' hnp' d2))
          · right; rw [hSp]; refine ⟨d1, ?_⟩
            show (if m = n then s.cfg else s.known m).prim = p; rw [if_neg e]; exact d2 }

/-! ### MakeEligible / MakeIneligible -/

section cas
variable {s : State Node} (h : Inv s) {p r : Node} (hp : ThinksPrimary s p) (hkl : KnowsLatest s p)
include h hp hkl

theorem inv_makeElig (hs : s.sync p r = true) (he : s.cfg.elig r = false) :
    Inv (makeElig s p r) := by
  obtain ⟨hkp, hpp⟩ := thinksPrimary_knowsLatest h hp hkl
  have hpa : PrimaryAuthoritative s := (thinksPrimary_primaryAuthoritative hp hkp).2
  subst hpp
  have hK := makeElig_known (s := s) (r := r)
  have hKp := makeElig_known_prim (s := s) (r := r) hkp
  have hE := makeElig_elig (s := s) (r := r)
  have hI := makeElig_inc (s := s) (r := r)
  have hrp : r ≠ s.cfg.prim := by
    intro e; rw [e, h.noSelfSync] at hs; exact Bool.false_ne_true hs
  have hKi := makeElig_known_inc (s := s) hkp hrp
  obtain ⟨hrf, hrec⟩ := h.syncNew r hs he
  have hra : (s.log s.cfg.prim).take (s.acked s.cfg.prim r) <+: s.log r :=
    h.ackHolds _ _ (thinksPrimary_prim h hpa) hrp hrf
  have hsc := h.syncCommit _ _ (thinksPrimary_prim h hpa) hs
  have hhl : s.hist.length ≤ s.acked s.cfg.prim r := by
    rcases h.histLe hpa with a | a <;> omega
  have hhist : s.hist <+: s.log r := by
    have hp0 := h.ehc s.cfg.prim ⟨Or.inr rfl, hpa⟩
    have e := prefix_take_eq hp0 (Nat.le_refl s.hist.length)
    rw [List.take_length] at e
    rw [e]; exact take_mono_prefix hhl hra
  have htp := makeElig_thinksPrimary (s := s) hkp hrp
  have hauth : ∀ n, IsAuthoritative (makeElig s s.cfg.prim r) n → n = r ∨ IsAuthoritative s n :=
    fun _ => makeElig_isAuthoritative
  exact {
  ehc := by
    intro n hn
    rcases hauth n hn with rfl | hn
    · exact hhist
    · exact h.ehc n hn
  caLen := h.caLen, caHist := h.caHist, caEq := h.caEq
  knownLe := by
    intro x; show ((makeElig s s.cfg.prim r).known x).ver ≤ s.cfg.ver + 1; rw [hK]; split
    · exact Nat.le_refl _
    · exact Nat.le_succ_of_le (h.knownLe x)
  knownEq := by
    intro x hx; change ((makeElig s s.cfg.prim r).known x).ver = s.cfg.ver + 1 at hx
    show (makeElig s s.cfg.prim r).known x = makeEligCfg s r
    rw [hK] at hx ⊢; split
    · rfl
    · rename_i e; rw [if_neg e] at hx; have := h.knownLe x; omega
  primKnows := by show (makeElig s s.cfg.prim r).known s.cfg.prim = _; rw [hK, if_pos rfl]; rfl
  incLe := by
    intro x; show (makeElig s s.cfg.prim r).cfg.inc x ≤ s.inc x; rw [hI]; split
    · rename_i e; subst e; exact Nat.le_refl _
    · exact h.incLe x
  knownIncLe := by
    intro x y; rw [hK]; split
    · show upd s.cfg.inc r (s.inc r) y ≤ s.inc y
      by_cases ey : y = r
      · subst ey; simp
      · rw [upd_ne _ _ ey]; exact h.incLe y
    · exact h.knownIncLe x y
  authEx := by
    refine ⟨s.cfg.prim, Or.inr rfl, ?_⟩
    show (makeElig s s.cfg.prim r).cfg.inc s.cfg.prim = s.inc s.cfg.prim
    rw [hI, if_neg (Ne.symm hrp)]; exact hpa
  eligSync := by
    intro x y hx hy hi
    show s.sync x y = true
    rw [hK] at hy hi
    split at hy
    · rename_i e; subst e
      change (if y = r then true else s.cfg.elig y) = true at hy
      rw [if_pos rfl] at hi
      change upd s.cfg.inc r (s.inc r) y = s.inc y at hi
      by_cases ey : y = r
      · subst ey; exact hs
      · rw [if_neg ey] at hy; rw [upd_ne _ _ ey] at hi
        exact h.eligSync _ y (thinksPrimary_prim h hpa) (by rw [h.primKnows]; exact hy)
          (by rw [h.primKnows]; exact hi)
    · rename_i e; rw [if_neg e] at hi; exact h.eligSync x y ((htp x).1 hx) hy hi
  noSelfSync := h.noSelfSync
  ackedPrim := by intro x hx; rw [hKp] at hx; exact h.ackedPrim x hx
  ackedLe := by intro x y hx; exact h.ackedLe x y ((htp x).1 hx)
  ackHolds := by intro x y hx hy hf; rw [hKp] at hf; exact h.ackHolds x y ((htp x).1 hx) hy hf
  syncNew := by
    intro x hx hxe
    rw [hE] at hxe
    split at hxe
    · exact absurd hxe (by decide)
    · obtain ⟨a, b⟩ := h.syncNew x hx hxe; rw [hKp]; exact ⟨a, b⟩
  syncCommit := by intro x y hx; exact h.syncCommit x y ((htp x).1 hx)
  eligFol := by
    intro m hm hf
    rw [hKp] at hf
    change (s.known m).prim ≠ s.cfg.prim at hf
    rw [hE] at hm
    split at hm
    · rename_i e; subst e; exact absurd hrf hf
    · rw [hKp, hK, if_neg hf]; exact h.eligFol m hm hf
  histLe := fun _ => h.histLe hpa
  reconLen := h.reconLen
  stale := by
    intro x hx hv
    have hx' := (htp x).1 hx
    have hxp : x ≠ s.cfg.prim := by
      intro e; subst e; rw [hK, if_pos rfl] at hv; exact Nat.lt_irrefl _ hv
    have hv' : (s.known x).ver < s.cfg.ver := by
      rw [hK, if_neg hxp] at hv
      rcases Nat.lt_or_ge (s.known x).ver s.cfg.ver with a | a
      · exact a
      · exfalso; have := h.knownEq x (Nat.le_antisymm (h.knownLe x) a)
        exact hxp (thinksPrimary_knowsLatest h hx' (by unfold KnowsLatest; rw [this])).2.symm
    -- The fence does not move; the newly eligible r holds x's log up to it because r
    -- caught up past the reconciliation point, which is past the fence.
    have hF := h.stale x hx' hv'
    refine hF.transfer rfl hKp (fun _ _ => rfl) rfl rfl (fun _ => hpa)
      (fun _ => Nat.le_refl _) (fun _ => rfl) ?_
    intro m hm hi
    by_cases e : m = r
    · subst e; right
      exact take_transfer (Nat.le_trans (hF.reconLe hpa) hrec)
        (hF.le_len (fun y => h.ackedLe x y hx')) (hF.primHolds hpa) hra
    · left
      change (if m = r then true else s.cfg.elig m) = true at hm
      change (if m = r then s.inc r else s.cfg.inc m) = s.inc m at hi
      rw [if_neg e] at hm hi; exact ⟨hm, hi, rfl⟩ }

theorem inv_makeInelig (he : s.cfg.elig r = true) : Inv (makeInelig s p r) := by
  obtain ⟨hkp, hpp⟩ := thinksPrimary_knowsLatest h hp hkl
  have hpa : PrimaryAuthoritative s := (thinksPrimary_primaryAuthoritative hp hkp).2
  subst hpp
  have hK := makeInelig_known (s := s) (r := r)
  have hKp := makeInelig_known_prim (s := s) (r := r) hkp
  have hKi := makeInelig_known_inc (s := s) (r := r) hkp
  have hE := makeInelig_elig (s := s) (r := r)
  have hS := makeInelig_sync (s := s) (r := r)
  have hSle : ∀ x y, (makeInelig s s.cfg.prim r).sync x y = true → s.sync x y = true :=
    fun _ _ => makeInelig_sync_le
  have htp := makeInelig_thinksPrimary (s := s) (r := r) hkp
  have hEle : ∀ m, (makeInelig s s.cfg.prim r).cfg.elig m = true → m ≠ r ∧ s.cfg.elig m = true :=
    fun _ => makeInelig_elig_true
  have hauth : ∀ n, IsAuthoritative (makeInelig s s.cfg.prim r) n → IsAuthoritative s n :=
    fun _ => makeInelig_isAuthoritative
  exact {
  ehc := fun n hn => h.ehc n (hauth n hn)
  caLen := h.caLen, caHist := h.caHist, caEq := h.caEq
  knownLe := by
    intro x; show ((makeInelig s s.cfg.prim r).known x).ver ≤ s.cfg.ver + 1; rw [hK]; split
    · exact Nat.le_refl _
    · exact Nat.le_succ_of_le (h.knownLe x)
  knownEq := by
    intro x hx; change ((makeInelig s s.cfg.prim r).known x).ver = s.cfg.ver + 1 at hx
    show (makeInelig s s.cfg.prim r).known x = makeIneligCfg s r
    rw [hK] at hx ⊢; split
    · rfl
    · rename_i e; rw [if_neg e] at hx; have := h.knownLe x; omega
  primKnows := by show (makeInelig s s.cfg.prim r).known s.cfg.prim = _; rw [hK, if_pos rfl]; rfl
  incLe := h.incLe
  knownIncLe := by
    intro x y; rw [hK]; split
    · exact h.incLe y
    · exact h.knownIncLe x y
  authEx := ⟨s.cfg.prim, Or.inr rfl, hpa⟩
  eligSync := by
    intro x y hx hy hi
    rw [hS]
    rw [hK] at hy hi
    split at hy
    · rename_i e; subst e
      rw [if_pos rfl] at hi
      change (if y = r then false else s.cfg.elig y) = true at hy
      change s.cfg.inc y = s.inc y at hi
      split at hy
      · exact absurd hy Bool.false_ne_true
      · rename_i ey
        rw [if_neg (fun c => ey c.2)]
        exact h.eligSync _ y (thinksPrimary_prim h hpa) (by rw [h.primKnows]; exact hy)
          (by rw [h.primKnows]; exact hi)
    · rename_i e; rw [if_neg e] at hi; rw [if_neg (fun c => e c.1)]
      exact h.eligSync x y ((htp x).1 hx) hy hi
  noSelfSync := by
    intro x; rw [hS]; split
    · rfl
    · exact h.noSelfSync x
  ackedPrim := by intro x hx; rw [hKp] at hx; exact h.ackedPrim x hx
  ackedLe := by intro x y hx; exact h.ackedLe x y ((htp x).1 hx)
  ackHolds := by intro x y hx hy hf; rw [hKp] at hf; exact h.ackHolds x y ((htp x).1 hx) hy hf
  syncNew := by
    intro x hx hxe
    have hx0 := hSle _ _ hx
    rw [hS] at hx
    rw [hE] at hxe
    split at hxe
    · rename_i e; subst e; rw [if_pos ⟨rfl, rfl⟩] at hx; exact absurd hx Bool.false_ne_true
    · obtain ⟨a, b⟩ := h.syncNew x hx0 hxe; rw [hKp]; exact ⟨a, b⟩
  syncCommit := by intro x y hx hxy; exact h.syncCommit x y ((htp x).1 hx) (hSle x y hxy)
  eligFol := by
    intro m hm hf
    obtain ⟨_, hm'⟩ := hEle m hm
    rw [hKp] at hf
    change (s.known m).prim ≠ s.cfg.prim at hf
    rw [hKp, hK, if_neg hf]; exact h.eligFol m hm' hf
  histLe := h.histLe
  reconLen := h.reconLen
  stale := by
    intro x hx hv
    have hx' := (htp x).1 hx
    have hxp : x ≠ s.cfg.prim := by
      intro e; subst e; rw [hK, if_pos rfl] at hv; exact Nat.lt_irrefl _ hv
    have hv' : (s.known x).ver < s.cfg.ver := by
      rw [hK, if_neg hxp] at hv
      rcases Nat.lt_or_ge (s.known x).ver s.cfg.ver with a | a
      · exact a
      · exfalso; have := h.knownEq x (Nat.le_antisymm (h.knownLe x) a)
        exact hxp (thinksPrimary_knowsLatest h hx' (by unfold KnowsLatest; rw [this])).2.symm
    have hSx : (makeInelig s s.cfg.prim r).sync x = s.sync x := by
      funext y; rw [hS, if_neg (fun c => hxp c.1)]
    exact (h.stale x hx' hv').transfer hSx hKp (fun _ _ => rfl) rfl rfl (fun _ => hpa)
      (fun _ => Nat.le_refl _) (fun _ => rfl)
      (fun m hm hi => Or.inl ⟨(hEle m hm).2, hi, rfl⟩) }

end cas

end RestartProof
