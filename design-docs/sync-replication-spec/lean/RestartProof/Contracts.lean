import RestartProof.Model

/-!
# Action contracts

What each action of `Model.lean` changes and what it leaves alone (AppendWrite and
Replicate only extend one log, and are covered by `inv_grow` instead), stated in the
design's terms: who thinks it is primary, who is authoritative, whose log, sync
replica set, acknowledgements and commit point move. The invariant proofs
(`Actions1-3.lean`, `Restart.lean`) use these lemmas instead of unfolding the
actions, so each action's effect is written down, and can be checked against the
design doc, in one place.

None of these lemmas needs the invariant; they follow from the action's definition
alone. Hypotheses that come from the action's guard are named after it.
-/

namespace RestartProof

variable {Node : Type} [DecidableEq Node] {s : State Node}

/-! ## Takeover(n)

An eligible replica `n` becomes primary in a new configuration. Logs do not move. `n`
starts a new primaryship: its sync replica set is the matching eligible replicas,
its acknowledgements and commit point are reset, and its catch-up point is the end of
its log (F1). Every other node keeps its state, including the old primary, which is
not told. -/

section takeover
variable {n : Node}

theorem takeover_log : (takeover s n).log = s.log := rfl

theorem takeover_known (x : Node) :
    (takeover s n).known x = if x = n then takeoverCfg s n else s.known x := rfl

theorem takeover_known_other {x : Node} (hx : x ≠ n) : (takeover s n).known x = s.known x := by
  rw [takeover_known, if_neg hx]

theorem takeover_sync_self : (takeover s n).sync n = takeoverSync s n := by simp [takeover]

theorem takeover_sync_other {x : Node} (hx : x ≠ n) : (takeover s n).sync x = s.sync x := by
  simp [takeover, upd, hx]

theorem takeover_acked_self : (takeover s n).acked n = fun _ => 0 := by simp [takeover]

theorem takeover_acked_other {x : Node} (hx : x ≠ n) : (takeover s n).acked x = s.acked x := by
  simp [takeover, upd, hx]

theorem takeover_commit_self : (takeover s n).commit n = 0 := by simp [takeover]

theorem takeover_commit_other {x : Node} (hx : x ≠ n) : (takeover s n).commit x = s.commit x := by
  simp [takeover, upd, hx]

/-- The new primary must commit its whole inherited log before it serves (F1). -/
theorem takeover_recon_self : (takeover s n).recon n = (s.log n).length := by simp [takeover]

/-- The new primary counts exactly the eligible replicas of the new configuration
whose recorded incarnation matches. -/
theorem takeoverSync_iff (x : Node) :
    takeoverSync s n x = true ↔ takeoverElig s n x = true ∧ s.cfg.inc x = s.inc x := by
  simp [takeoverSync]

/-- The new primary does not count itself. -/
theorem takeoverSync_self : takeoverSync s n n = false := by
  simp [takeoverSync, takeoverElig]

/-- The new eligible set: the old one without the old primary and without `n`. -/
theorem takeoverElig_iff (x : Node) :
    takeoverElig s n x = true ↔ s.cfg.elig x = true ∧ x ≠ s.cfg.prim ∧ x ≠ n := by
  simp [takeoverElig, and_assoc]

/-- After a takeover the new primary thinks it is primary, and every other node
thinks so exactly when it did before. In particular a deposed primary still does. -/
theorem takeover_thinksPrimary (hin : s.cfg.inc n = s.inc n) (x : Node) :
    ThinksPrimary (takeover s n) x ↔ x = n ∨ (x ≠ n ∧ ThinksPrimary s x) := by
  unfold ThinksPrimary; rw [takeover_known]
  by_cases e : x = n
  · subst e; simp only [takeoverCfg, true_or, iff_true]
    exact ⟨rfl, hin⟩
  · simp only [if_neg e]
    constructor
    · intro c; exact Or.inr ⟨e, c⟩
    · rintro (c | ⟨_, c⟩)
      · exact absurd c e
      · exact c

end takeover

/-! ## Learn(n)

`n` adopts the latest configuration. If it is not the primary there, it drops its
sync replica set. Nothing else changes. -/

section learn
variable {n : Node}

theorem learn_known (x : Node) : (learn s n).known x = if x = n then s.cfg else s.known x := rfl

theorem learn_sync_self (hnp : n ≠ s.cfg.prim) : (learn s n).sync n = fun _ => false := by
  simp [learn, upd, Ne.symm hnp]

theorem learn_sync_other {x : Node} (hx : x ≠ n) : (learn s n).sync x = s.sync x := by
  simp [learn, upd, hx]

/-- Learning a configuration never makes a node primary: a node that is not the
primary of the latest configuration learns that it is not. -/
theorem learn_thinksPrimary (hnp : n ≠ s.cfg.prim) {x : Node} (hx : ThinksPrimary (learn s n) x) :
    x ≠ n ∧ ThinksPrimary s x := by
  obtain ⟨hx1, hx2⟩ := hx
  rw [learn_known] at hx1 hx2
  by_cases e : x = n
  · rw [if_pos e] at hx1; exact absurd (hx1.trans e).symm hnp
  · rw [if_neg e] at hx1 hx2; exact ⟨e, hx1, hx2⟩

end learn

/-! ## MakeEligible(prim, r) and MakeIneligible(prim, r)

The current primary publishes a configuration with `r` added to or removed from the
eligible set (step 4 of promotion, step 2 of demotion). Only its own copy of the
configuration changes; MakeIneligible also stops counting `r`. Stated for the
current primary, which is the only node these steps can run on (`hkp`: it knows the
latest configuration). -/

section cas
variable {r : Node}

theorem makeElig_known (x : Node) : (makeElig s s.cfg.prim r).known x =
    if x = s.cfg.prim then makeEligCfg s r else s.known x := rfl

theorem makeElig_known_prim (hkp : s.known s.cfg.prim = s.cfg) (x : Node) :
    ((makeElig s s.cfg.prim r).known x).prim = (s.known x).prim := by
  rw [makeElig_known]; split
  · rename_i e; subst e; rw [hkp]; rfl
  · rfl

theorem makeElig_elig (x : Node) :
    (makeElig s s.cfg.prim r).cfg.elig x = if x = r then true else s.cfg.elig x := rfl

/-- MakeEligible records `r`'s current incarnation. -/
theorem makeElig_inc (x : Node) :
    (makeElig s s.cfg.prim r).cfg.inc x = if x = r then s.inc r else s.cfg.inc x := rfl

theorem makeElig_known_inc (hkp : s.known s.cfg.prim = s.cfg) (hrp : r ≠ s.cfg.prim) (x : Node) :
    ((makeElig s s.cfg.prim r).known x).inc x = (s.known x).inc x := by
  rw [makeElig_known]; split
  · rename_i e; subst e; rw [hkp]
    show upd s.cfg.inc r (s.inc r) s.cfg.prim = s.cfg.inc s.cfg.prim
    exact upd_ne _ _ (Ne.symm hrp)
  · rfl

/-- Promotion does not change who thinks it is primary. -/
theorem makeElig_thinksPrimary (hkp : s.known s.cfg.prim = s.cfg) (hrp : r ≠ s.cfg.prim) (x : Node) :
    ThinksPrimary (makeElig s s.cfg.prim r) x ↔ ThinksPrimary s x := by
  unfold ThinksPrimary; rw [makeElig_known_prim hkp, makeElig_known_inc hkp hrp]; rfl

/-- The only node promotion can make authoritative is `r`. -/
theorem makeElig_isAuthoritative {x : Node} (hx : IsAuthoritative (makeElig s s.cfg.prim r) x) :
    x = r ∨ IsAuthoritative s x := by
  obtain ⟨hn, hi⟩ := hx
  by_cases e : x = r
  · exact Or.inl e
  · right
    change (if x = r then true else s.cfg.elig x) = true ∨ x = s.cfg.prim at hn
    change (if x = r then s.inc r else s.cfg.inc x) = s.inc x at hi
    rw [if_neg e] at hn hi; exact ⟨hn, hi⟩

theorem makeInelig_known (x : Node) : (makeInelig s s.cfg.prim r).known x =
    if x = s.cfg.prim then makeIneligCfg s r else s.known x := rfl

theorem makeInelig_known_prim (hkp : s.known s.cfg.prim = s.cfg) (x : Node) :
    ((makeInelig s s.cfg.prim r).known x).prim = (s.known x).prim := by
  rw [makeInelig_known]; split
  · rename_i e; subst e; rw [hkp]; rfl
  · rfl

theorem makeInelig_known_inc (hkp : s.known s.cfg.prim = s.cfg) (x : Node) :
    ((makeInelig s s.cfg.prim r).known x).inc = (s.known x).inc := by
  rw [makeInelig_known]; split
  · rename_i e; subst e; rw [hkp]; rfl
  · rfl

theorem makeInelig_elig (x : Node) :
    (makeInelig s s.cfg.prim r).cfg.elig x = if x = r then false else s.cfg.elig x := rfl

/-- Demotion stops the primary counting `r`, and changes no other sync replica set. -/
theorem makeInelig_sync (x y : Node) : (makeInelig s s.cfg.prim r).sync x y =
    if x = s.cfg.prim ∧ y = r then false else s.sync x y := by
  by_cases ex : x = s.cfg.prim
  · subst ex; by_cases ey : y = r
    · subst ey; simp [makeInelig, upd]
    · simp [makeInelig, upd, ey]
  · simp [makeInelig, upd, ex]

theorem makeInelig_sync_le {x y : Node} (hxy : (makeInelig s s.cfg.prim r).sync x y = true) :
    s.sync x y = true := by
  rw [makeInelig_sync] at hxy; split at hxy
  · exact absurd hxy Bool.false_ne_true
  · exact hxy

/-- Demotion does not change who thinks it is primary. -/
theorem makeInelig_thinksPrimary (hkp : s.known s.cfg.prim = s.cfg) (x : Node) :
    ThinksPrimary (makeInelig s s.cfg.prim r) x ↔ ThinksPrimary s x := by
  unfold ThinksPrimary; rw [makeInelig_known_prim hkp, makeInelig_known_inc hkp]; rfl

theorem makeInelig_elig_true {m : Node} (hm : (makeInelig s s.cfg.prim r).cfg.elig m = true) :
    m ≠ r ∧ s.cfg.elig m = true := by
  rw [makeInelig_elig] at hm; split at hm
  · exact absurd hm Bool.false_ne_true
  · rename_i e; exact ⟨e, hm⟩

/-- Demotion makes no node authoritative. -/
theorem makeInelig_isAuthoritative {x : Node} (hx : IsAuthoritative (makeInelig s s.cfg.prim r) x) :
    IsAuthoritative s x := by
  obtain ⟨hn, hi⟩ := hx
  rcases hn with hn | hn
  · exact ⟨Or.inl (makeInelig_elig_true hn).2, hi⟩
  · exact ⟨Or.inr hn, hi⟩

end cas

/-! ## Replication: Ack(r), Count(p, r), Commit(p)

Each changes one entry: `p`'s view of `r`'s acknowledged offset, `p`'s sync replica
set, or `p`'s commit point (and the committed history, the first time an offset
commits). -/

section repl

/-- Ack(r): only `r`'s primary's record of `r`'s acknowledgement moves, to the end of
`r`'s log. -/
theorem ack_acked (r q x : Node) : (ack s r).acked q x =
    if q = primaryOf s r ∧ x = r then (s.log r).length else s.acked q x := by
  by_cases hq : q = primaryOf s r
  · subst hq; by_cases hx : x = r
    · subst hx; simp [ack, upd]
    · simp [ack, upd, hx]
  · simp [ack, upd, hq]

/-- Count(p, r): `p` adds `r` to its sync replica set (promotion step 2). -/
theorem count_sync (p r q x : Node) : (count s p r).sync q x = (s.sync q x || (q = p ∧ x = r)) := by
  by_cases hq : q = p
  · subst hq; by_cases hx : x = r
    · subst hx; simp [count, upd]
    · simp [count, upd, hx]
  · simp [count, upd, hq]

/-- Commit(p): `p`'s committed offset advances by one. -/
theorem commitNext_commit (p x : Node) :
    (commitNext s p).commit x = if x = p then s.commit p + 1 else s.commit x := by
  by_cases hx : x = p
  · subst hx; simp [commitNext, upd]
  · simp [commitNext, upd, hx]

/-- Commit(p) at an offset no node has committed yet: the entry joins the committed
history. -/
theorem commitNext_hist_new {p : Node} (he : s.commit p = s.hist.length) :
    (commitNext s p).hist = s.hist ++ [(s.log p).getD (s.commit p) 0] := by
  simp [commitNext, he]

/-- Commit(p) at an offset already committed elsewhere: the history does not change. -/
theorem commitNext_hist_old {p : Node} (he : ¬ s.commit p = s.hist.length) :
    (commitNext s p).hist = s.hist := by
  simp [commitNext, he]

end repl

/-! ## Resync(r)

A diverged, uncounted, ineligible replica `r` replaces its log by a prefix of its
primary's log, up to the primary's catch-up point, its acknowledgement is reset, and its commit
point is capped at its primary's. Nothing else changes. -/

section resync
variable {r : Node}

theorem resync_log_self :
    (resync true s r).log r = (s.log (primaryOf s r)).take (catchUpPoint true s (primaryOf s r)) := by
  simp [resync]

theorem resync_log_other {x : Node} (hx : x ≠ r) : (resync true s r).log x = s.log x := by
  simp [resync, upd, hx]

theorem resync_commit_self :
    (resync true s r).commit r = min (s.commit r) (s.commit (primaryOf s r)) := by simp [resync]

theorem resync_commit_other {x : Node} (hx : x ≠ r) : (resync true s r).commit x = s.commit x := by
  simp [resync, upd, hx]

theorem resync_acked (q x : Node) :
    (resync true s r).acked q x = if q = primaryOf s r ∧ x = r then 0 else s.acked q x := by
  by_cases hq : q = primaryOf s r
  · subst hq; by_cases hx : x = r
    · subst hx; simp [resync, upd]
    · simp [resync, upd, hx]
  · simp [resync, upd, hq]

theorem resync_acked_le (q x : Node) : (resync true s r).acked q x ≤ s.acked q x := by
  rw [resync_acked]; split
  · exact Nat.zero_le _
  · exact Nat.le_refl _

/-- Acknowledgements the primary counts are untouched, because `r` is not counted. -/
theorem resync_acked_sync (hsy : s.sync (primaryOf s r) r = false) {q x : Node}
    (hs : s.sync q x = true) : (resync true s r).acked q x = s.acked q x := by
  rw [resync_acked]; split
  · rename_i c; obtain ⟨rfl, rfl⟩ := c; rw [hsy] at hs; exact absurd hs (by decide)
  · rfl

end resync

/-! ## RestartEmpty(n)

`n` loses its log and its volatile state, and gets a new incarnation, so the
configuration's record of it no longer matches: it stops being authoritative and
stops thinking it is primary. The current primary stops counting `n` and forgets its
acknowledgement. Every other node is unchanged. -/

section restartEmpty
variable {n : Node}

theorem restartEmpty_inc (x : Node) :
    (restartEmpty s n).inc x = if x = n then s.inc n + 1 else s.inc x := by
  by_cases e : x = n
  · subst e; simp [restartEmpty]
  · simp [restartEmpty, upd, e]

theorem restartEmpty_inc_other {x : Node} (e : x ≠ n) : (restartEmpty s n).inc x = s.inc x := by
  rw [restartEmpty_inc, if_neg e]

theorem restartEmpty_inc_self : (restartEmpty s n).inc n = s.inc n + 1 := by
  rw [restartEmpty_inc, if_pos rfl]

theorem restartEmpty_log_other {x : Node} (e : x ≠ n) : (restartEmpty s n).log x = s.log x := by
  simp [restartEmpty, upd, e]

theorem restartEmpty_commit_other {x : Node} (e : x ≠ n) :
    (restartEmpty s n).commit x = s.commit x := by simp [restartEmpty, upd, e]

theorem restartEmpty_commit_self : (restartEmpty s n).commit n = 0 := by simp [restartEmpty]

theorem restartEmpty_recon_other {x : Node} (e : x ≠ n) :
    (restartEmpty s n).recon x = s.recon x := by simp [restartEmpty, upd, e]

theorem restartEmpty_recon_self : (restartEmpty s n).recon n = 0 := by simp [restartEmpty]

theorem restartEmpty_sync (p r : Node) : (restartEmpty s n).sync p r =
    if p = n then false else if p = s.cfg.prim ∧ r = n then false else s.sync p r := by
  by_cases e : p = n
  · simp [restartEmpty, e]
  · by_cases ep : p = s.cfg.prim
    · subst ep
      by_cases er : r = n
      · simp [restartEmpty, e, er]
      · simp [restartEmpty, e, er, upd]
    · simp [restartEmpty, e, ep]

theorem restartEmpty_acked (p r : Node) : (restartEmpty s n).acked p r =
    if p = n then 0 else if p = s.cfg.prim ∧ r = n then 0 else s.acked p r := by
  by_cases e : p = n
  · simp [restartEmpty, e]
  · by_cases ep : p = s.cfg.prim
    · subst ep
      by_cases er : r = n
      · simp [restartEmpty, e, er]
      · simp [restartEmpty, e, er, upd]
    · simp [restartEmpty, e, ep]

theorem restartEmpty_acked_le (p r : Node) : (restartEmpty s n).acked p r ≤ s.acked p r := by
  rw [restartEmpty_acked]; split
  · exact Nat.zero_le _
  · split
    · exact Nat.zero_le _
    · exact Nat.le_refl _

theorem restartEmpty_sync_le {p r : Node} (hs : (restartEmpty s n).sync p r = true) :
    s.sync p r = true := by
  rw [restartEmpty_sync] at hs; split at hs
  · exact absurd hs (by decide)
  · split at hs
    · exact absurd hs (by decide)
    · exact hs

/-- A node that restarted empty does not think it is primary any more: its own
configuration records its old incarnation. (`hinc`: recorded incarnations never run
ahead of the real ones; `hkn`: `n` knew the latest configuration.) -/
theorem restartEmpty_thinksPrimary (hkn : s.known n = s.cfg) (hinc : s.cfg.inc n ≤ s.inc n)
    {x : Node} (hx : ThinksPrimary (restartEmpty s n) x) : x ≠ n ∧ ThinksPrimary s x := by
  obtain ⟨h1, h2⟩ := hx
  have hxn : x ≠ n := by
    intro e; subst e
    change (s.known x).inc x = (restartEmpty s x).inc x at h2
    rw [hkn, restartEmpty_inc_self] at h2; omega
  refine ⟨hxn, h1, ?_⟩
  change (s.known x).inc x = (restartEmpty s n).inc x at h2
  rw [restartEmpty_inc_other hxn] at h2; exact h2

/-- A node that restarted empty is not authoritative, and no node becomes so. -/
theorem restartEmpty_isAuthoritative (hinc : s.cfg.inc n ≤ s.inc n) {x : Node}
    (hx : IsAuthoritative (restartEmpty s n) x) : x ≠ n ∧ IsAuthoritative s x := by
  obtain ⟨h1, h2⟩ := hx
  have hxn : x ≠ n := by
    intro e; subst e
    change s.cfg.inc x = (restartEmpty s x).inc x at h2
    rw [restartEmpty_inc_self] at h2; omega
  refine ⟨hxn, h1, ?_⟩
  change s.cfg.inc x = (restartEmpty s n).inc x at h2
  rw [restartEmpty_inc_other hxn] at h2; exact h2

theorem restartEmpty_primaryAuthoritative (hinc : s.cfg.inc n ≤ s.inc n)
    (hp : PrimaryAuthoritative (restartEmpty s n)) : s.cfg.prim ≠ n ∧ PrimaryAuthoritative s := by
  have := restartEmpty_isAuthoritative hinc (x := s.cfg.prim) ⟨Or.inr rfl, hp⟩
  exact ⟨this.1, this.2.2⟩

end restartEmpty

/-! ## RestartKeep(n) (RebuildSync = TRUE)

`n` keeps its log and incarnation and loses its volatile state: commit point and
acknowledgements reset, and it must re-commit its whole log before serving. If it is
primary, its sync replica set is rebuilt from the configuration (F5). Who thinks it
is primary does not change. -/

section restartKeep
variable {n : Node}

theorem restartKeep_commit_other {x : Node} (e : x ≠ n) :
    (restartKeep true s n).commit x = s.commit x := by simp [restartKeep, upd, e]

theorem restartKeep_commit_self : (restartKeep true s n).commit n = 0 := by simp [restartKeep]

theorem restartKeep_recon_other {x : Node} (e : x ≠ n) :
    (restartKeep true s n).recon x = s.recon x := by simp [restartKeep, upd, e]

theorem restartKeep_recon_self : (restartKeep true s n).recon n = (s.log n).length := by
  simp [restartKeep]

theorem restartKeep_acked_other {x : Node} (e : x ≠ n) :
    (restartKeep true s n).acked x = s.acked x := by simp [restartKeep, upd, e]

theorem restartKeep_acked_self : (restartKeep true s n).acked n = fun _ => 0 := by
  simp [restartKeep]

theorem restartKeep_sync_other {x : Node} (e : x ≠ n) :
    (restartKeep true s n).sync x = s.sync x := by simp [restartKeep, upd, e]

/-- F5: a restarted primary's sync replica set is rebuilt from the configuration. -/
theorem restartKeep_sync_self : (restartKeep true s n).sync n =
    if (s.known n).prim = n ∧ (s.known n).inc n = s.inc n then rebuiltSync s n
    else fun _ => false := by
  simp [restartKeep]

theorem restartKeep_thinksPrimary (x : Node) :
    ThinksPrimary (restartKeep true s n) x ↔ ThinksPrimary s x := Iff.rfl

end restartKeep

end RestartProof
