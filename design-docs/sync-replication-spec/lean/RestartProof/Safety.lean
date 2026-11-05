import RestartProof.Preserve

/-! Main theorem: every reachable state of CoreRestart's replication part
(ReconcileFirst = TRUE, RebuildSync = TRUE, the survival guard on RestartEmpty)
satisfies Safety, with EligibleHoldsCommitted over authoritative nodes, for EVERY
MinSync `m`, any node type, unbounded logs, versions, incarnations and restarts. -/

namespace RestartProof

variable {Node : Type} [DecidableEq Node]

/-- Safety for every min-sync-replicas value `m`.

In prose: with the F1 fix (`rf = true`) and the F5 fix (`rb = true`), in every state
reachable from `init i`, every authoritative node holds the whole committed history,
and no node has committed an entry that differs from it. This holds for any number
of nodes, log length, configuration changes and restarts, provided each restart
without data leaves some other authoritative node with its data. -/
theorem safety {m : Nat} {i : Node} {s : State Node} (h : Reachable m true true i s) : Safety s :=
  inv_safety (inv_reachable h)

/-- What MinSync >= 1 adds: when the current primary commits, an eligible replica with a
matching incarnation counts toward the commit, so after the commit the history is on
at least two distinct authoritative nodes, the primary and that replica. -/
theorem commit_two_copies {m : Nat} (hm : 1 ≤ m) {i : Node} {s : State Node}
    (h : Reachable m true true i s) {p : Node} (hp : ThinksPrimary s p) (hpp : p = s.cfg.prim)
    (hw : Writable m s p) (hlen : s.commit p < (s.log p).length)
    (hcap : ∀ r, s.sync p r = true → s.commit p < s.acked p r) :
    ∃ e, e ≠ p ∧ IsAuthoritative (commitNext s p) e ∧ IsAuthoritative (commitNext s p) p ∧
      (commitNext s p).hist <+: (commitNext s p).log e ∧ (commitNext s p).hist <+: (commitNext s p).log p := by
  have hi := inv_reachable h
  have hs' := (safety (Reachable.step h (Step.commit s p hp hw hlen hcap))).1
  obtain ⟨r, h1, h2, h3⟩ : (syncEligible s p).Nonempty :=
    Set.one_le_encard_iff_nonempty.mp (le_trans (by exact_mod_cast hm) hw)
  subst hpp
  rw [hi.primKnows] at h2 h3
  have hrp : r ≠ s.cfg.prim := by intro e; rw [e, hi.noSelfSync] at h1; exact absurd h1 (by decide)
  have har : IsAuthoritative (commitNext s s.cfg.prim) r := ⟨Or.inl h2, h3⟩
  have hap : IsAuthoritative (commitNext s s.cfg.prim) s.cfg.prim := by
    refine ⟨Or.inr rfl, ?_⟩
    have := hp.2; rw [hi.primKnows] at this; exact this
  exact ⟨r, hrp, har, hap, hs' r har, hs' _ hap⟩

end RestartProof
