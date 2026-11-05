import RestartProof.Restart

/-! Main theorem: every reachable state of CoreRestart's replication part
(ReconcileFirst = TRUE, RebuildSync = TRUE, the survival guard on RestartEmpty)
satisfies Safety, with EligibleHoldsCommitted over authoritative nodes, for EVERY
MinSync `m`, any node type, unbounded logs, versions, incarnations and restarts. -/

namespace RestartProof

variable {Node : Type} [DecidableEq Node]

theorem inv_step {m : Nat} {s s' : State Node} (h : Inv s) (hs : Step m true true s s') : Inv s' := by
  cases hs with
  | append p _ _ _ => exact inv_append h p
  | replicate r _ _ _ => exact inv_replicate h r
  | ack r hne hpre hlt => exact inv_ack h r hne hpre hlt
  | commit p hp _ hlen hcap => exact inv_commit h p hp hlen hcap
  | count p r hp hrp hf _ _ hle => exact inv_count h p r hp hrp hf hle
  | makeElig p r hp hk hsy he => exact inv_makeElig h hp hk hsy he
  | makeInelig p r hp hk he => exact inv_makeInelig h hp hk he
  | learn n hk => exact inv_learn h n hk
  | takeover n hk he hnp hin => exact inv_takeover h n hk he hnp hin
  | resync r hne _ hsy hke => exact inv_resync h r hne hsy hke
  | restartEmpty n hk hsv => exact inv_restartEmpty h n hk hsv
  | restartKeep n hk => exact inv_restartKeep h n hk

theorem inv_reachable {m : Nat} {i : Node} {s : State Node} (h : Reachable m true true i s) : Inv s := by
  induction h with
  | init => exact inv_init i
  | step _ hs ih => exact inv_step ih hs

/-- Safety for every min-sync-replicas value `m`. -/
theorem safety {m : Nat} {i : Node} {s : State Node} (h : Reachable m true true i s) : Safety s :=
  inv_safety (inv_reachable h)

/-- What MinSync >= 1 adds: when the current primary commits, an eligible replica with a
matching incarnation counts toward the commit, so after the commit the history is on
at least two distinct authoritative nodes, the primary and that replica. -/
theorem commit_two_copies {m : Nat} (hm : 1 ≤ m) {i : Node} {s : State Node}
    (h : Reachable m true true i s) {p : Node} (hp : tp s p) (hpp : p = s.cfg.prim)
    (hw : writable m s p) (hlen : s.commit p < (s.log p).length)
    (hcap : ∀ r, s.sync p r = true → s.commit p < s.acked p r) :
    ∃ e, e ≠ p ∧ auth (commitA s p) e ∧ auth (commitA s p) p ∧
      (commitA s p).hist <+: (commitA s p).log e ∧ (commitA s p).hist <+: (commitA s p).log p := by
  have hi := inv_reachable h
  have hs' := (safety (Reachable.step h (Step.commit s p hp hw hlen hcap))).1
  obtain ⟨l, _, hl, hr⟩ := hw
  obtain ⟨r, hrl⟩ : ∃ r, r ∈ l := by
    cases l with
    | nil => simp at hl; omega
    | cons r _ => exact ⟨r, List.mem_cons_self⟩
  obtain ⟨h1, h2, h3⟩ := hr r hrl
  subst hpp
  rw [hi.primKnows] at h2 h3
  have hrp : r ≠ s.cfg.prim := by intro e; rw [e, hi.noSelfSync] at h1; exact absurd h1 (by decide)
  have har : auth (commitA s s.cfg.prim) r := ⟨Or.inl h2, h3⟩
  have hap : auth (commitA s s.cfg.prim) s.cfg.prim := by
    refine ⟨Or.inr rfl, ?_⟩
    have := hp.2; rw [hi.primKnows] at this; exact this
  exact ⟨r, hrp, har, hap, hs' r har, hs' _ hap⟩

end RestartProof
