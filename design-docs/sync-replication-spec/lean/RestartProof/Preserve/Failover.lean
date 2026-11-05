import RestartProof.Inv
import RestartProof.Contracts

/-!
# Preservation: failover (design §5)

Takeover: an eligible replica becomes primary in a new configuration. The old primary
is not stopped; it is fenced (`Fenced`).
-/

namespace RestartProof

variable {Node : Type} [DecidableEq Node]

section takeover

/-! Every lemma below takes the takeover's guard as hypotheses: `n` knows the latest
configuration, is eligible in it, is not its primary, and its recorded incarnation
matches. They all take the same arguments, so some lemmas take a guard part they do
not use. -/
set_option linter.unusedSectionVars false

variable {s : State Node} (h : Inv s) (n : Node) (hkl : KnowsLatest s n)
    (hel : s.cfg.elig n = true) (hnp : n ≠ s.cfg.prim) (hin : s.cfg.inc n = s.inc n)
include h hkl hel hnp hin

/-- Configuration: the takeover's configuration becomes the latest one, `n`'s copy is it,
and recorded incarnations do not change. -/
theorem config_takeover : ConfigInv (takeover s n) := by
  have hK : ∀ x, (takeover s n).known x = if x = n then takeoverCfg s n else s.known x :=
    takeover_known
  exact {
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
    · exact h.knownIncLe x y }

/-- Replication: `n` starts its primaryship with no acknowledgements recorded; every other
node's records are unchanged, and no log moves. -/
theorem repl_takeover : ReplInv (takeover s n) := by
  have hK : ∀ x, (takeover s n).known x = if x = n then takeoverCfg s n else s.known x :=
    takeover_known
  have hKo : ∀ x, x ≠ n → (takeover s n).known x = s.known x := fun _ => takeover_known_other
  have hSn : (takeover s n).sync n = takeoverSync s n := takeover_sync_self
  have hSo : ∀ x, x ≠ n → (takeover s n).sync x = s.sync x := fun _ => takeover_sync_other
  have hAn : (takeover s n).acked n = fun _ => 0 := takeover_acked_self
  have hAo : ∀ x, x ≠ n → (takeover s n).acked x = s.acked x := fun _ => takeover_acked_other
  have hCn : (takeover s n).commit n = 0 := takeover_commit_self
  have hCo : ∀ x, x ≠ n → (takeover s n).commit x = s.commit x := fun _ => takeover_commit_other
  have htp := takeover_thinksPrimary hin
  exact {
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
  syncCommit := by
    intro x y hx hs
    rcases (htp x).1 hx with rfl | ⟨e, hx'⟩
    · rw [hCn]; exact Nat.zero_le _
    · rw [hCo x e, hAo x e]; rw [hSo x e] at hs; exact h.syncCommit x y hx' hs }

/-- Membership: `n` counts exactly the eligible replicas of the new configuration whose
recorded incarnation matches, and counts no ineligible replica. -/
theorem membership_takeover : MembershipInv (takeover s n) := by
  have hK : ∀ x, (takeover s n).known x = if x = n then takeoverCfg s n else s.known x :=
    takeover_known
  have hKo : ∀ x, x ≠ n → (takeover s n).known x = s.known x := fun _ => takeover_known_other
  have hSn : (takeover s n).sync n = takeoverSync s n := takeover_sync_self
  have hSo : ∀ x, x ≠ n → (takeover s n).sync x = s.sync x := fun _ => takeover_sync_other
  have hNE := takeoverElig_iff (s := s) (n := n)
  have htp := takeover_thinksPrimary hin
  exact {
  eligSync := by
    intro x y hx hy hi
    rcases (htp x).1 hx with rfl | ⟨e, hx'⟩
    · rw [hSn]; rw [hK, if_pos rfl] at hy hi
      change takeoverElig s x y = true at hy
      change s.cfg.inc y = s.inc y at hi
      exact (takeoverSync_iff y).2 ⟨hy, hi⟩
    · rw [hSo x e]; rw [hKo x e] at hy hi; exact h.eligSync x y hx' hy hi
  syncNew := by
    intro r hs he
    change (takeover s n).sync n r = true at hs
    rw [hSn] at hs
    change takeoverElig s n r = false at he
    rw [takeoverSync_iff, he] at hs; exact absurd hs.1 (by decide)
  eligFol := by
    intro m hm hf
    change takeoverElig s n m = true at hm
    obtain ⟨hm1, hm2, hm3⟩ := (hNE m).1 hm
    rw [hKo m hm3] at hf ⊢
    change (s.known m).prim ≠ n at hf
    rw [hKo _ hf]
    by_cases hq : (s.known m).prim = s.cfg.prim
    · rw [hq, h.primKnows]; exact hm1
    · exact h.eligFol m hm1 hq }

/-- Commit: `n`'s commit point is 0 and its catch-up point is the end of its log. As an
eligible replica it held the whole committed history, so the history is below its
catch-up point (F1: it reconciles before it serves). -/
theorem commit_takeover : CommitInv (takeover s n) := by
  have hCn : (takeover s n).commit n = 0 := takeover_commit_self
  have hCo : ∀ x, x ≠ n → (takeover s n).commit x = s.commit x := fun _ => takeover_commit_other
  have hRn : (takeover s n).recon n = (s.log n).length := takeover_recon_self
  have hehn : s.hist <+: s.log n := h.ehc n ⟨Or.inl hel, hin⟩
  exact {
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
  reconLen := by show (takeover s n).recon n ≤ (s.log n).length; rw [hRn]
  histLe := by intro _; right; show s.hist.length ≤ (takeover s n).recon n; rw [hRn]; exact hehn.length_le }

/-- Failover: every deposed primary is fenced, including the one `n` replaced, and `n`
itself is authoritative. -/
theorem failover_takeover : FailoverInv (takeover s n) := by
  have hkn : s.known n = s.cfg := h.knownEq n hkl
  have hfn : (s.known n).prim = s.cfg.prim := by rw [hkn]
  have hK : ∀ x, (takeover s n).known x = if x = n then takeoverCfg s n else s.known x :=
    takeover_known
  have hKo : ∀ x, x ≠ n → (takeover s n).known x = s.known x := fun _ => takeover_known_other
  have hSo : ∀ x, x ≠ n → (takeover s n).sync x = s.sync x := fun _ => takeover_sync_other
  have hAo : ∀ x, x ≠ n → (takeover s n).acked x = s.acked x := fun _ => takeover_acked_other
  have hRn : (takeover s n).recon n = (s.log n).length := takeover_recon_self
  have hNE := takeoverElig_iff (s := s) (n := n)
  have htp := takeover_thinksPrimary hin
  exact {
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
        exact hF.eligHolds m hm1 hi
  authEx := ⟨n, Or.inr rfl, hin⟩ }

/-- Goal: `n` held the committed history as an eligible replica, and the new eligible
set is part of the old one, so every authoritative node still holds it. -/
theorem ehc_takeover :
    ∀ m, IsAuthoritative (takeover s n) m → (takeover s n).hist <+: (takeover s n).log m := by
  have hNE := takeoverElig_iff (s := s) (n := n)
  have hehn : s.hist <+: s.log n := h.ehc n ⟨Or.inl hel, hin⟩
  intro m ⟨hm, hi⟩
  change takeoverElig s n m = true ∨ m = n at hm
  change s.cfg.inc m = s.inc m at hi
  rcases hm with hm | rfl
  · exact h.ehc m ⟨Or.inl ((hNE m).1 hm).1, hi⟩
  · exact hehn
/-- Takeover preserves the invariant, layer by layer. -/
theorem inv_takeover : Inv (takeover s n) where
  toConfigInv := config_takeover h n hkl hel hnp hin
  toReplInv := repl_takeover h n hkl hel hnp hin
  toMembershipInv := membership_takeover h n hkl hel hnp hin
  toCommitInv := commit_takeover h n hkl hel hnp hin
  toFailoverInv := failover_takeover h n hkl hel hnp hin
  ehc := ehc_takeover h n hkl hel hnp hin

end takeover

end RestartProof
