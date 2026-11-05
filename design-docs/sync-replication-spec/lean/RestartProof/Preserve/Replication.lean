import RestartProof.Inv
import RestartProof.Contracts

/-!
# Preservation: replication (design §2 write path; full resync)

Steps that move data between a primary and its replicas: AppendWrite and Replicate
(logs only grow, `inv_grow`), Ack (a primary records how far a replica holds its log)
and Resync (a diverged, uncounted, ineligible replica is reset to a prefix of its
primary's log).
-/

namespace RestartProof

variable {Node : Type} [DecidableEq Node]

-- Every layer lemma takes the action's whole guard, so some take a part they
-- do not use.
set_option linter.unusedSectionVars false

/-- Any step that only extends logs preserves the invariant (AppendWrite, Replicate). -/
theorem inv_grow {s : State Node} (h : Inv s) (L : Node → List Nat)
    (hL : ∀ x, s.log x <+: L x) : Inv { s with log := L } where
  ehc := fun n hn => (h.ehc n hn).trans (hL n)
  caLen := fun p => Nat.le_trans (h.caLen p) (hL p).length_le
  caHist := h.caHist
  caEq := by
    intro p
    show (L p).take (s.commit p) = _
    rw [← prefix_take_eq (hL p) (h.caLen p)]; exact h.caEq p
  knownLe := h.knownLe
  knownEq := h.knownEq
  primKnows := h.primKnows
  eligSync := h.eligSync
  noSelfSync := h.noSelfSync
  ackedPrim := h.ackedPrim
  ackedLe := fun p r hp => Nat.le_trans (h.ackedLe p r hp) (hL p).length_le
  ackHolds := by
    intro p n hp hn hf
    show (L p).take (s.acked p n) <+: L n
    rw [← prefix_take_eq (hL p) (h.ackedLe p n hp)]
    exact (h.ackHolds p n hp hn hf).trans (hL n)
  syncNew := h.syncNew
  reconLen := Nat.le_trans h.reconLen (hL _).length_le
  syncCommit := h.syncCommit
  eligFol := h.eligFol
  incLe := h.incLe
  knownIncLe := h.knownIncLe
  authEx := h.authEx
  histLe := h.histLe
  stale := fun p hp hv => (h.stale p hp hv).mono rfl rfl rfl rfl hL
    (fun r => h.ackedLe p r hp) (fun _ x => x) (fun _ _ => rfl)

theorem grow_upd (L : Node → List Nat) (p : Node) (m : List Nat) :
    ∀ x, L x <+: upd L p (L p ++ m) x := by
  intro x; by_cases hx : x = p
  · subst hx; simp [List.prefix_append]
  · rw [upd_ne _ _ hx]

theorem inv_append {s : State Node} (h : Inv s) (p : Node) : Inv (appendWrite s p) :=
  inv_grow h _ (grow_upd s.log p _)

theorem inv_replicate {s : State Node} (h : Inv s) (r : Node) : Inv (replicate s r) :=
  inv_grow h _ (grow_upd s.log r _)

section ack

variable {s : State Node} (h : Inv s) (r : Node) (hne : primaryOf s r ≠ r) (hpre : s.log r <+: s.log (primaryOf s r)) (hlt : s.acked (primaryOf s r) r < (s.log r).length)

include h hne hpre hlt

/-- Ack changes no configuration. -/
theorem config_ack : ConfigInv (ack s r) :=
  (h.toConfigInv).frame rfl rfl rfl

/-- The recorded acknowledgement becomes the replica's log length, and the replica's log is a prefix of its primary's, so the record is real. -/
theorem repl_ack : ReplInv (ack s r) := by
  have hA := ack_acked (s := s) r
  have hfol : (s.known r).prim = primaryOf s r := rfl
  exact {
  noSelfSync := h.noSelfSync
  ackedPrim := by
    intro x hx
    show (ack s r).acked s.cfg.prim x = 0
    rw [hA]; split
    · rename_i hc; obtain ⟨h1, rfl⟩ := hc; exact absurd (h1 ▸ hfol) hx
    · exact h.ackedPrim x hx
  ackedLe := by
    intro q x hq
    show (ack s r).acked q x ≤ _
    rw [hA]; split
    · rename_i hc; obtain ⟨rfl, rfl⟩ := hc; exact hpre.length_le
    · exact h.ackedLe q x hq
  ackHolds := by
    intro q n hq hn hf
    show (s.log q).take ((ack s r).acked q n) <+: s.log n
    rw [hA]; split
    · rename_i hc; obtain ⟨rfl, rfl⟩ := hc
      rw [← prefix_take_eq hpre (Nat.le_refl _), List.take_length]
    · exact h.ackHolds q n hq hn hf
  syncCommit := by
    intro q x hq hs
    show _ ≤ (ack s r).acked q x
    rw [hA]; split
    · rename_i hc; obtain ⟨rfl, rfl⟩ := hc; exact Nat.le_trans (h.syncCommit _ _ hq hs) (Nat.le_of_lt hlt)
    · exact h.syncCommit q x hq hs }

/-- Acknowledgements only grow, so a counted replica stays caught up. -/
theorem membership_ack : MembershipInv (ack s r) := by
  have hA := ack_acked (s := s) r
  exact {
  eligSync := h.eligSync
  syncNew := by
    intro x hs he
    obtain ⟨a, b⟩ := h.syncNew x hs he
    refine ⟨a, ?_⟩
    show _ ≤ (ack s r).acked s.cfg.prim x
    rw [hA]; split
    · rename_i hc; obtain ⟨h1, rfl⟩ := hc
      show s.recon s.cfg.prim ≤ _; have := hlt; rw [← h1] at this; omega
    · exact b
  eligFol := h.eligFol }

/-- Ack changes no commit point, catch-up point or history. -/
theorem commit_ack : CommitInv (ack s r) :=
  (h.toCommitInv).frame rfl rfl rfl rfl rfl rfl

/-- Only the acknowledgement of a replica that still follows its primary moves, so no fence moves. -/
theorem failover_ack : FailoverInv (ack s r) := by
  have hA := ack_acked (s := s) r
  have hfol : (s.known r).prim = primaryOf s r := rfl
  exact {
  stale := by
    -- Ack moves only the acknowledgement of a replica that follows its primary.
    intro p hp hv
    refine (h.stale p hp hv).mono rfl rfl rfl rfl (fun _ => List.prefix_refl _)
      (fun x => h.ackedLe p x hp) (fun _ x => x) ?_
    intro x hx
    show (ack s r).acked p x = s.acked p x
    rw [hA]; split
    · rename_i hc; obtain ⟨rfl, rfl⟩ := hc; exact absurd hfol hx
    · rfl
  authEx := h.authEx }

/-- Ack moves no log. -/
theorem ehc_ack : EligibleHoldsCommitted (ack s r) := by
  exact h.ehc

theorem inv_ack : Inv (ack s r) where
  toConfigInv := config_ack h r hne hpre hlt
  toReplInv := repl_ack h r hne hpre hlt
  toMembershipInv := membership_ack h r hne hpre hlt
  toCommitInv := commit_ack h r hne hpre hlt
  toFailoverInv := failover_ack h r hne hpre hlt
  ehc := ehc_ack h r hne hpre hlt

end ack

section resync

variable {s : State Node} (h : Inv s) (r : Node) (hne : primaryOf s r ≠ r) (hsy : s.sync (primaryOf s r) r = false) (hke : (s.known (primaryOf s r)).elig r = false)

include h hne hsy hke

/-- What the Resync guard says about `r` in the latest configuration. The guard only
says `r` is uncounted and ineligible *in its primary's copy* of the configuration;
`eligFol` lifts that to the latest one, so `r` is neither the current primary nor
eligible, and no node that thinks it is primary is `r`. -/
theorem resync_facts :
    r ≠ s.cfg.prim ∧ s.cfg.elig r = false ∧ (∀ x, ThinksPrimary s x → x ≠ r) := by
  have hrprim : r ≠ s.cfg.prim := by
    intro e; apply hne; show (s.known r).prim = r; rw [e, h.primKnows]
  refine ⟨hrprim, ?_, fun x hx e => by subst e; exact hne hx.1⟩
  cases hc : s.cfg.elig r
  · rfl
  · exfalso
    by_cases hf : (s.known r).prim = s.cfg.prim
    · have : primaryOf s r = s.cfg.prim := hf
      rw [this, h.primKnows, hc] at hke; exact absurd hke (by decide)
    · have := h.eligFol r hc hf
      change (s.known (primaryOf s r)).elig r = true at this
      rw [hke] at this; exact absurd this (by decide)

/-- Resync changes no configuration. -/
theorem config_resync : ConfigInv (resync true s r) :=
  (h.toConfigInv).frame rfl rfl rfl

/-- `r`'s log becomes a prefix of its primary's and its acknowledgement is reset to 0, so recorded acknowledgements stay real. -/
theorem repl_resync : ReplInv (resync true s r) := by
  obtain ⟨hrprim, hrel, htpne⟩ := resync_facts h r hne hsy hke
  have hcl := commit_le_catchUpPoint h (primaryOf s r)
  have hLo : ∀ x, x ≠ r → (resync true s r).log x = s.log x := fun _ => resync_log_other
  have hCo : ∀ x, x ≠ r → (resync true s r).commit x = s.commit x := fun _ => resync_commit_other
  have hA := resync_acked (s := s) (r := r)
  have hAle := resync_acked_le (s := s) (r := r)
  have hAs : ∀ q x, s.sync q x = true → (resync true s r).acked q x = s.acked q x :=
    fun _ _ => resync_acked_sync hsy
  exact {
  noSelfSync := h.noSelfSync
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
  syncCommit := by
    intro q x hq hs
    rw [hCo q (htpne q hq), hAs q x hs]; exact h.syncCommit q x hq hs }

/-- `r` was not counted, so the acknowledgements primaries count are untouched and every
counted replica stays caught up. -/
theorem membership_resync : MembershipInv (resync true s r) := by
  have hcl := commit_le_catchUpPoint h (primaryOf s r)
  have hAs : ∀ q x, s.sync q x = true → (resync true s r).acked q x = s.acked q x :=
    fun _ _ => resync_acked_sync hsy
  exact {
  eligSync := h.eligSync
  syncNew := by
    intro x hs he
    obtain ⟨a, b⟩ := h.syncNew x hs he
    refine ⟨a, ?_⟩
    show s.recon s.cfg.prim ≤ (resync true s r).acked s.cfg.prim x
    rw [hAs _ _ (show s.sync s.cfg.prim x = true from hs)]; exact b
  eligFol := h.eligFol }

/-- `r`'s commit point is capped at its primary's, so its committed prefix is still a prefix of the history. -/
theorem commit_resync : CommitInv (resync true s r) := by
  obtain ⟨hrprim, hrel, htpne⟩ := resync_facts h r hne hsy hke
  have hcl := commit_le_catchUpPoint h (primaryOf s r)
  have hcu := catchUpPoint_le_len h (primaryOf s r)
  have hLr := resync_log_self (s := s) (r := r)
  have hLo : ∀ x, x ≠ r → (resync true s r).log x = s.log x := fun _ => resync_log_other
  have hCr := resync_commit_self (s := s) (r := r)
  have hCo : ∀ x, x ≠ r → (resync true s r).commit x = s.commit x := fun _ => resync_commit_other
  exact {
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
  reconLen := by
    show _ ≤ ((resync true s r).log s.cfg.prim).length
    rw [hLo _ (Ne.symm hrprim)]; exact h.reconLen
  histLe := by
    intro hpa
    show s.hist.length ≤ (resync true s r).commit s.cfg.prim ∨ _
    rw [hCo _ (Ne.symm hrprim)]; exact h.histLe hpa }

/-- `r` is neither a primary nor eligible (`resync_facts`), and no counted acknowledgement
moves, so every fence and everything it covers is unchanged. -/
theorem failover_resync : FailoverInv (resync true s r) := by
  obtain ⟨hrprim, hrel, htpne⟩ := resync_facts h r hne hsy hke
  have hcl := commit_le_catchUpPoint h (primaryOf s r)
  have hLo : ∀ x, x ≠ r → (resync true s r).log x = s.log x := fun _ => resync_log_other
  have hAs : ∀ q x, s.sync q x = true → (resync true s r).acked q x = s.acked q x :=
    fun _ _ => resync_acked_sync hsy
  have hnotr : ∀ x, (s.cfg.elig x = true ∨ x = s.cfg.prim) → x ≠ r := by
    intro x hx e; subst e
    rcases hx with hx | hx
    · rw [hrel] at hx; exact absurd hx (by decide)
    · exact hrprim hx
  exact {
  stale := by
    intro x hx hv
    have hxr := htpne x hx
    exact (h.stale x hx hv).transfer rfl (fun _ => rfl) (hAs x) (hLo x hxr) rfl id
      (fun _ => Nat.le_refl _) (fun _ => hLo _ (Ne.symm hrprim))
      (fun m hm hi => Or.inl ⟨hm, hi, hLo m (hnotr m (Or.inl hm))⟩)
  authEx := h.authEx }

/-- `r` is not authoritative (`resync_facts`), and no authoritative node's log moves. -/
theorem ehc_resync : EligibleHoldsCommitted (resync true s r) := by
  obtain ⟨hrprim, hrel, htpne⟩ := resync_facts h r hne hsy hke
  have hcl := commit_le_catchUpPoint h (primaryOf s r)
  have hLo : ∀ x, x ≠ r → (resync true s r).log x = s.log x := fun _ => resync_log_other
  have hnotr : ∀ x, (s.cfg.elig x = true ∨ x = s.cfg.prim) → x ≠ r := by
    intro x hx e; subst e
    rcases hx with hx | hx
    · rw [hrel] at hx; exact absurd hx (by decide)
    · exact hrprim hx
  intro x hx; rw [hLo x (hnotr x hx.1)]; exact h.ehc x hx

/-- Resync copies the primary's log up to its CatchUpPoint (the inherited log while it
is unreconciled). -/
theorem inv_resync : Inv (resync true s r) where
  toConfigInv := config_resync h r hne hsy hke
  toReplInv := repl_resync h r hne hsy hke
  toMembershipInv := membership_resync h r hne hsy hke
  toCommitInv := commit_resync h r hne hsy hke
  toFailoverInv := failover_resync h r hne hsy hke
  ehc := ehc_resync h r hne hsy hke

end resync

end RestartProof
