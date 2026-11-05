import RestartProof.Lists
import Mathlib.Data.Nat.Lattice
import RestartProof.Model

/-! The inductive invariant of CoreRestart. It is `ReconProof.Inv` with:
- `ehc` and `Fenced.eligHolds` over authoritative nodes (`IsAuthoritative`);
- `eligSync` (O1) only for eligible replicas whose recorded incarnation matches;
- `histLe` and the current-primary clauses of `Fenced` only while the current primary is
  authoritative (`PrimaryAuthoritative`): a primary that came back empty holds nothing, and nothing new
  is committed until an authoritative eligible replica takes over;
- a new conjunct `incLe`: the config never records an incarnation ahead of the node. -/

namespace RestartProof

variable {Node : Type} [DecidableEq Node]

/-!
## The fence of a deposed primary

A takeover does not stop the old primary: it may still think it is primary and keep
committing. What stops it is the commit rule. Some members of its sync replica set
have moved to the new primary, so they never acknowledge the old primary again, and
it can never commit past the smallest of their frozen acknowledgements. That offset
is the old primary's *fence*. The design's "Writes can't split-brain" (§5) is then: a
deposed primary only commits below its fence, and the new configuration already
holds its log up to the fence.
-/

/-- The members of `p`'s sync replica set that follow a different primary. -/
def leftSync (s : State Node) (p : Node) : Set Node :=
  {r | s.sync p r = true ∧ (s.known r).prim ≠ p}

/-- The fence of `p`: the smallest acknowledgement `p` has recorded from a sync
replica that has left it. It is `0` when no replica has left (`sInf ∅ = 0`); it is only
used through `Fenced`, which rules that case out. -/
noncomputable def fence (s : State Node) (p : Node) : Nat :=
  sInf (s.acked p '' leftSync s p)

/-- `p` is fenced: some sync replica has left it, and the new configuration holds
`p`'s log up to its fence. -/
structure Fenced (s : State Node) (p : Node) : Prop where
  /-- Some member of `p`'s sync replica set has moved to a different primary. -/
  left      : (leftSync s p).Nonempty
  /-- While the current primary is authoritative, it must reconcile at least up to the
  fence (F1). -/
  reconLe   : PrimaryAuthoritative s → fence s p ≤ s.recon s.cfg.prim
  /-- While the current primary is authoritative, it holds `p`'s log up to the fence. -/
  primHolds : PrimaryAuthoritative s → (s.log p).take (fence s p) <+: s.log s.cfg.prim
  /-- Every eligible replica holds `p`'s log up to the fence, or still follows `p`
  and is counted by it (so `p` cannot commit past what that replica holds). -/
  eligHolds : ∀ m, s.cfg.elig m = true → s.cfg.inc m = s.inc m →
    (s.log p).take (fence s p) <+: s.log m ∨ (s.sync p m = true ∧ (s.known m).prim = p)

theorem fence_le {s : State Node} {p r : Node} (hr : r ∈ leftSync s p) : fence s p ≤ s.acked p r :=
  Nat.sInf_le ⟨r, hr, rfl⟩

/-- To show `p` is fenced, name one sync replica `w` that has left it and show the
fence properties at `w`'s acknowledgement. The fence is at most that, and each
property still holds at a smaller offset. `XCheck.FencedAt` restates these
hypotheses as a decidable predicate: keep the two in sync. -/
theorem Fenced.of_left {s : State Node} {p w : Node} (hw : w ∈ leftSync s p)
    (hrec : PrimaryAuthoritative s → s.acked p w ≤ s.recon s.cfg.prim)
    (hprim : PrimaryAuthoritative s → (s.log p).take (s.acked p w) <+: s.log s.cfg.prim)
    (helig : ∀ m, s.cfg.elig m = true → s.cfg.inc m = s.inc m →
      (s.log p).take (s.acked p w) <+: s.log m ∨ (s.sync p m = true ∧ (s.known m).prim = p)) :
    Fenced s p := by
  have hle := fence_le hw
  refine ⟨⟨w, hw⟩, fun hp => hle.trans (hrec hp), fun hp => take_mono_prefix hle (hprim hp), ?_⟩
  intro m hm hi
  rcases helig m hm hi with d | d
  · exact Or.inl (take_mono_prefix hle d)
  · exact Or.inr d

/-- The fence is the acknowledgement of some replica that has left. -/
theorem Fenced.attained {s : State Node} {p : Node} (hF : Fenced s p) :
    ∃ w ∈ leftSync s p, s.acked p w = fence s p := Nat.sInf_mem (hF.left.image _)

/-- A step that only extends logs, may add sync replicas, and leaves the configuration,
the nodes' views of it and `p`'s acknowledgements from departed replicas alone keeps
`p` fenced (AppendWrite, Replicate, Ack, Commit, Count). -/
theorem Fenced.mono {s s' : State Node} {p : Node} (hF : Fenced s p)
    (hcfg : s'.cfg = s.cfg) (hknown : s'.known = s.known) (hrecon : s'.recon = s.recon)
    (hinc : s'.inc = s.inc) (hgrow : ∀ x, s.log x <+: s'.log x)
    (hlp : ∀ r, s.acked p r ≤ (s.log p).length)
    (hsync : ∀ r, s.sync p r = true → s'.sync p r = true)
    (hacked : ∀ r, (s.known r).prim ≠ p → s'.acked p r = s.acked p r) : Fenced s' p := by
  obtain ⟨w, ⟨hw1, hw2⟩, hwF⟩ := hF.attained
  have hpa : PrimaryAuthoritative s' → PrimaryAuthoritative s := by
    unfold PrimaryAuthoritative; rw [hcfg, hinc]; exact id
  have ha : s'.acked p w = fence s p := (hacked w hw2).trans hwF
  have e : (s'.log p).take (s'.acked p w) = (s.log p).take (fence s p) := by
    rw [ha]; exact (prefix_take_eq (hgrow p) (by rw [← hwF]; exact hlp w)).symm
  refine Fenced.of_left (w := w) ⟨hsync w hw1, by rw [hknown]; exact hw2⟩ ?_ ?_ ?_
  · intro hp; rw [ha, hrecon, hcfg]; exact hF.reconLe (hpa hp)
  · intro hp; rw [e, hcfg]; exact (hF.primHolds (hpa hp)).trans (hgrow _)
  · intro m hm hi
    rw [hcfg] at hm; rw [hcfg, hinc] at hi
    rcases hF.eligHolds m hm hi with d | ⟨d1, d2⟩
    · left; rw [e]; exact d.trans (hgrow m)
    · right; exact ⟨hsync m d1, by rw [hknown]; exact d2⟩

/-- A step that leaves `x`'s log, sync replica set and counted acknowledgements alone,
does not move who follows whom, and gives every newly authoritative eligible replica
`x`'s log up to the fence keeps `x` fenced (MakeEligible, MakeIneligible, Resync,
the restarts of other nodes). -/
theorem Fenced.transfer {s s' : State Node} {x : Node} (hF : Fenced s x)
    (hsync : s'.sync x = s.sync x) (hkn : ∀ y, (s'.known y).prim = (s.known y).prim)
    (hack : ∀ r, s.sync x r = true → s'.acked x r = s.acked x r)
    (hlog : s'.log x = s.log x) (hprim : s'.cfg.prim = s.cfg.prim)
    (hpa : PrimaryAuthoritative s' → PrimaryAuthoritative s)
    (hrec : PrimaryAuthoritative s' → s.recon s.cfg.prim ≤ s'.recon s.cfg.prim)
    (hlp : PrimaryAuthoritative s' → s'.log s.cfg.prim = s.log s.cfg.prim)
    (hcov : ∀ m, s'.cfg.elig m = true → s'.cfg.inc m = s'.inc m →
      (s.cfg.elig m = true ∧ s.cfg.inc m = s.inc m ∧ s'.log m = s.log m) ∨
        (s.log x).take (fence s x) <+: s'.log m) : Fenced s' x := by
  obtain ⟨w, ⟨hw1, hw2⟩, hwF⟩ := hF.attained
  have ha : s'.acked x w = fence s x := (hack w hw1).trans hwF
  refine Fenced.of_left (w := w) ⟨by rw [hsync]; exact hw1, by rw [hkn]; exact hw2⟩ ?_ ?_ ?_
  · intro hp; rw [ha, hprim]; exact (hF.reconLe (hpa hp)).trans (hrec hp)
  · intro hp; rw [ha, hlog, hprim, hlp hp]; exact hF.primHolds (hpa hp)
  · intro m hm hi
    rcases hcov m hm hi with ⟨he, hi0, hl⟩ | d
    · rcases hF.eligHolds m he hi0 with d | ⟨d1, d2⟩
      · left; rw [ha, hlog, hl]; exact d
      · right; rw [hsync, hkn]; exact ⟨d1, d2⟩
    · left; rw [ha, hlog]; exact d

/-- The fence is within `p`'s log when `p`'s recorded acknowledgements are. -/
theorem Fenced.le_len {s : State Node} {p : Node} (hF : Fenced s p)
    (hlp : ∀ r, s.acked p r ≤ (s.log p).length) : fence s p ≤ (s.log p).length := by
  obtain ⟨w, _, hwF⟩ := hF.attained; rw [← hwF]; exact hlp w

/-!
## The inductive invariant, in layers

`Inv` is the conjunction of five layers. Each layer is about one part of the design,
and each is stated in terms of the layers listed before it, with one exception:
`MembershipInv.syncNew` already uses the reconciliation point that `CommitInv`
constrains. Read them in order; the last
field of the last layer, `ehc`, is the property we want.
-/

/-- **Configuration.** Cluster V2 holds one totally ordered configuration; each node's
copy may be behind it but never ahead, and incarnation records never run ahead of the
nodes they describe (design §6). -/
structure ConfigInv (s : State Node) : Prop where
  /-- No node has seen a configuration newer than the latest one. -/
  knownLe    : ∀ n, (s.known n).ver ≤ s.cfg.ver
  /-- A node that has seen the latest version has the latest configuration. -/
  knownEq    : ∀ n, (s.known n).ver = s.cfg.ver → s.known n = s.cfg
  /-- The current primary knows the configuration that made it primary. -/
  primKnows  : s.known s.cfg.prim = s.cfg
  /-- A recorded incarnation is never newer than the node's own, so a node that came
  back empty never matches its old record ("Restarted nodes rejoin as ineligible"). -/
  incLe      : ∀ n, s.cfg.inc n ≤ s.inc n
  /-- The same for every node's copy of the configuration. -/
  knownIncLe : ∀ x n, (s.known x).inc n ≤ s.inc n

/-- **Replication.** An acknowledgement is real: a primary's record of a replica's
acknowledged offset never exceeds what the replica holds of the primary's log, and a
primary's committed offset never passes a sync replica's acknowledgement (design §2,
commit rule). -/
structure ReplInv (s : State Node) : Prop where
  /-- A node is never in its own sync replica set. -/
  noSelfSync : ∀ p, s.sync p p = false
  /-- The current primary has no acknowledgement recorded from a node that follows a
  different primary. -/
  ackedPrim  : ∀ r, (s.known r).prim ≠ s.cfg.prim → s.acked s.cfg.prim r = 0
  /-- A recorded acknowledgement is within the primary's own log. -/
  ackedLe    : ∀ p r, ThinksPrimary s p → s.acked p r ≤ (s.log p).length
  /-- A follower holds its primary's log up to the acknowledged offset. -/
  ackHolds   : ∀ p n, ThinksPrimary s p → n ≠ p → (s.known n).prim = p →
                 (s.log p).take (s.acked p n) <+: s.log n
  /-- Commit rule: a primary never commits past a sync replica's acknowledgement. -/
  syncCommit : ∀ q r, ThinksPrimary s q → s.sync q r = true → s.commit q ≤ s.acked q r

/-- **Membership.** The two ordering rules of promotion and demotion (design §4): a
replica counts toward commits before it becomes eligible, and stops being eligible
before a write commits without it. -/
structure MembershipInv (s : State Node) : Prop where
  /-- Every eligible replica with a matching incarnation is counted ("must count toward
  commits **before** it becomes eligible"). F5 is what keeps this across a restart. -/
  eligSync   : ∀ p r, ThinksPrimary s p → (s.known p).elig r = true → (s.known p).inc r = s.inc r →
                 s.sync p r = true
  /-- A replica the current primary counts but has not made eligible follows it and has
  acknowledged at least the reconciliation point (promotion steps 1-2; F1). -/
  syncNew    : ∀ r, s.sync s.cfg.prim r = true → s.cfg.elig r = false →
                 (s.known r).prim = s.cfg.prim ∧ s.recon s.cfg.prim ≤ s.acked s.cfg.prim r
  /-- An eligible replica that follows an old primary is still eligible in that
  primary's configuration. -/
  eligFol    : ∀ m, s.cfg.elig m = true → (s.known m).prim ≠ s.cfg.prim →
                 (s.known (s.known m).prim).elig m = true

/-- **Commit.** Committed prefixes agree with the committed history, and the current
primary's catch-up point covers it (design §2, "committed offset"; §5, reconciliation). -/
structure CommitInv (s : State Node) : Prop where
  /-- A committed offset is within the node's log. -/
  caLen      : ∀ p, s.commit p ≤ (s.log p).length
  /-- ... and within the committed history. -/
  caHist     : ∀ p, s.commit p ≤ s.hist.length
  /-- What a node has committed is exactly the start of the committed history. -/
  caEq       : ∀ p, (s.log p).take (s.commit p) = s.hist.take (s.commit p)
  /-- The current primary's catch-up point is within its log. -/
  reconLen   : s.recon s.cfg.prim ≤ (s.log s.cfg.prim).length
  /-- While the current primary is authoritative, the whole committed history is below
  its commit point or its catch-up point: a new primary reconciles against all of it (F1). -/
  histLe     : PrimaryAuthoritative s → s.hist.length ≤ s.commit s.cfg.prim ∨ s.hist.length ≤ s.recon s.cfg.prim

/-- **Failover.** A deposed primary cannot commit anything the new configuration does
not hold ("Writes can't split-brain", design §5), and some node keeps the data (the
survival condition). -/
structure FailoverInv (s : State Node) : Prop where
  /-- A node that still thinks it is primary under an old configuration is fenced:
  it can only commit below its `fence`, and the new configuration holds its log up
  to there. -/
  stale      : ∀ p, ThinksPrimary s p → (s.known p).ver < s.cfg.ver → Fenced s p
  /-- Survival: some node is authoritative (kept by the RestartEmpty guard). -/
  authEx     : ∃ n, IsAuthoritative s n

/-! ### Frame lemmas: a layer is kept by any step that leaves its variables alone. -/

/-- A step that changes neither the configuration, nor any node's copy of it, nor any
incarnation keeps `ConfigInv`. -/
theorem ConfigInv.frame {s s' : State Node} (h : ConfigInv s)
    (hcfg : s'.cfg = s.cfg) (hknown : s'.known = s.known) (hinc : s'.inc = s.inc) :
    ConfigInv s' := by
  obtain ⟨h1, h2, h3, h4, h5⟩ := h
  exact ⟨by rw [hcfg, hknown]; exact h1, by rw [hcfg, hknown]; exact h2,
    by rw [hcfg, hknown]; exact h3, by rw [hcfg, hinc]; exact h4, by rw [hknown, hinc]; exact h5⟩

/-- A step that changes no commit point, catch-up point, log, history, configuration or
incarnation keeps `CommitInv`. -/
theorem CommitInv.frame {s s' : State Node} (h : CommitInv s)
    (hcommit : s'.commit = s.commit) (hrecon : s'.recon = s.recon) (hlog : s'.log = s.log)
    (hhist : s'.hist = s.hist) (hcfg : s'.cfg = s.cfg) (hinc : s'.inc = s.inc) : CommitInv s' := by
  obtain ⟨h1, h2, h3, h4, h5⟩ := h
  refine ⟨by rw [hcommit, hlog]; exact h1, by rw [hcommit, hhist]; exact h2,
    by rw [hcommit, hlog, hhist]; exact h3, by rw [hrecon, hlog, hcfg]; exact h4, ?_⟩
  intro hpa; rw [hhist, hcommit, hrecon, hcfg]
  exact h5 (by unfold PrimaryAuthoritative at hpa ⊢; rw [hcfg, hinc] at hpa; exact hpa)

/-- A step that changes no sync replica set, acknowledgement, configuration, copy of it,
incarnation or catch-up point keeps `MembershipInv`. -/
theorem MembershipInv.frame {s s' : State Node} (h : MembershipInv s)
    (hsync : s'.sync = s.sync) (hacked : s'.acked = s.acked) (hcfg : s'.cfg = s.cfg)
    (hknown : s'.known = s.known) (hinc : s'.inc = s.inc) (hrecon : s'.recon = s.recon) :
    MembershipInv s' := by
  obtain ⟨h1, h2, h3⟩ := h
  have htp : ∀ p, ThinksPrimary s' p ↔ ThinksPrimary s p := by
    intro p; unfold ThinksPrimary; rw [hknown, hinc]
  refine ⟨?_, ?_, ?_⟩
  · intro p r hp; rw [hknown, hinc, hsync]; exact h1 p r ((htp p).1 hp)
  · intro r; rw [hcfg, hsync, hknown, hrecon, hacked]; exact h2 r
  · intro m; rw [hcfg, hknown]; exact h3 m

/-- The inductive invariant of CoreRestart: the five layers plus the goal. -/
structure Inv (s : State Node) : Prop
    extends ConfigInv s, ReplInv s, MembershipInv s, CommitInv s, FailoverInv s where
  /-- **Goal**, EligibleHoldsCommitted: every authoritative node holds the whole
  committed history. -/
  ehc        : ∀ n, IsAuthoritative s n → s.hist <+: s.log n

theorem inv_safety {s : State Node} (h : Inv s) : Safety s :=
  ⟨h.ehc, fun p => ⟨h.caLen p, h.caEq p⟩⟩

theorem inv_init (i : Node) : Inv (init i) where
  ehc := by intro n _; simp [init]
  caLen := by intro p; simp [init]
  caHist := by intro p; simp [init]
  caEq := by intro p; simp [init]
  knownLe := by intro n; simp [init]
  knownEq := by intro n _; rfl
  primKnows := rfl
  incLe := by intro n; simp [init, initCfg]
  authEx := ⟨i, Or.inr rfl, rfl⟩
  knownIncLe := by intro x n; simp [init, initCfg]
  eligSync := by intro p r _ h; simp [init, initCfg] at h
  noSelfSync := by intro p; rfl
  ackedPrim := by intro r _; rfl
  ackedLe := by intro p r _; simp [init]
  ackHolds := by intro p n _ _ _; simp [init]
  syncNew := by intro r h; simp [init] at h
  reconLen := by simp [init]
  syncCommit := by intro q r _ _; simp [init]
  eligFol := by intro m h; simp [init, initCfg] at h
  histLe := by intro _; simp [init]
  stale := by intro p _ h; simp [init, initCfg] at h

/-! ### Derived facts -/

section
variable {s : State Node} (h : Inv s)
include h

theorem thinksPrimary_prim (hpa : PrimaryAuthoritative s) : ThinksPrimary s s.cfg.prim := by
  unfold ThinksPrimary; rw [h.primKnows]; exact ⟨rfl, hpa⟩

theorem thinksPrimary_knowsLatest {p : Node} (hp : ThinksPrimary s p) (hk : KnowsLatest s p) : s.known p = s.cfg ∧ s.cfg.prim = p := by
  have e := h.knownEq p hk
  refine ⟨e, ?_⟩
  have := hp.1; rw [e] at this; exact this

theorem thinksPrimary_cases {p : Node} (hp : ThinksPrimary s p) :
    (p = s.cfg.prim ∧ s.known p = s.cfg) ∨ (s.known p).ver < s.cfg.ver := by
  rcases Nat.lt_or_ge (s.known p).ver s.cfg.ver with hl | hg
  · exact Or.inr hl
  · have hk : KnowsLatest s p := Nat.le_antisymm (h.knownLe p) hg
    have := thinksPrimary_knowsLatest h hp hk
    exact Or.inl ⟨this.2.symm, this.1⟩

omit h in
/-- A node that thinks it is primary and knows the latest config is the current,
authoritative primary. -/
theorem thinksPrimary_primaryAuthoritative {p : Node} (hp : ThinksPrimary s p) (hk : s.known p = s.cfg) : p = s.cfg.prim ∧ PrimaryAuthoritative s := by
  have e1 := hp.1; have e2 := hp.2
  rw [hk] at e1 e2
  subst e1; exact ⟨rfl, e2⟩

theorem stale_ne_prim {p : Node} (hv : (s.known p).ver < s.cfg.ver) : p ≠ s.cfg.prim := by
  intro e; subst e; rw [h.primKnows] at hv; omega

theorem knowsLatest_iff_prim {p : Node} (hp : ThinksPrimary s p) (hk : KnowsLatest s p) : p = s.cfg.prim :=
  (thinksPrimary_knowsLatest h hp hk).2.symm

theorem prim_holds_sync (hpa : PrimaryAuthoritative s) {m : Node} (hs : s.sync s.cfg.prim m = true) :
    (s.log s.cfg.prim).take (s.acked s.cfg.prim m) <+: s.log m := by
  by_cases hf : (s.known m).prim = s.cfg.prim
  · have hne : m ≠ s.cfg.prim := by
      intro e; rw [e, h.noSelfSync] at hs; exact Bool.false_ne_true hs
    exact h.ackHolds _ _ (thinksPrimary_prim h hpa) hne hf
  · rw [h.ackedPrim m hf]; simp

/-- Core commit fact: if `p` may commit offset `c` by the commit rule, every
authoritative node holds the first `c+1` entries of `p`. -/
theorem commit_holds {p : Node} (hp : ThinksPrimary s p) (hlen : s.commit p < (s.log p).length)
    (hcap : ∀ r, s.sync p r = true → s.commit p < s.acked p r) :
    ∀ m, IsAuthoritative s m → (s.log p).take (s.commit p + 1) <+: s.log m := by
  intro m ⟨hm, hi⟩
  rcases thinksPrimary_cases h hp with ⟨rfl, hk⟩ | hv
  · have hpa : PrimaryAuthoritative s := (thinksPrimary_primaryAuthoritative hp hk).2
    rcases hm with he | rfl
    · have hs : s.sync s.cfg.prim m = true :=
        h.eligSync _ _ hp (by rw [hk]; exact he) (by rw [hk]; exact hi)
      exact take_mono_prefix (hcap m hs) (prim_holds_sync h hpa hs)
    · exact List.take_prefix _ _
  · -- `p` is deposed. By the commit rule it commits below its fence, and the new
    -- configuration holds its log up to the fence.
    have hF := h.stale p hp hv
    obtain ⟨w, ⟨hw1, _⟩, hwF⟩ := hF.attained
    have hcw : s.commit p + 1 ≤ fence s p := hwF ▸ hcap w hw1
    rcases hm with he | rfl
    · rcases hF.eligHolds m he hi with hd | ⟨hs, hf⟩
      · exact take_mono_prefix hcw hd
      · by_cases hmp : m = p
        · subst hmp; exact List.take_prefix _ _
        · exact take_mono_prefix (hcap m hs) (h.ackHolds _ _ hp hmp hf)
    · exact take_mono_prefix hcw (hF.primHolds hi)

end


/-! ### CatchUpPoint facts -/

/-- CatchUpPoint is at least the committed offset. -/
theorem commit_le_catchUpPoint {s : State Node} (h : Inv s) (p : Node) : s.commit p ≤ catchUpPoint true s p := by
  have := h.caLen p
  unfold catchUpPoint; split
  · rename_i c; omega
  · exact Nat.le_refl _

/-- For the current primary, CatchUpPoint covers the reconciliation point. -/
theorem recon_le_catchUpPoint {s : State Node} (h : Inv s) : s.recon s.cfg.prim ≤ catchUpPoint true s s.cfg.prim := by
  have := h.reconLen
  unfold catchUpPoint; split
  · omega
  · rename_i c; simp at c; exact c

theorem catchUpPoint_le_len {s : State Node} (h : Inv s) (p : Node) : catchUpPoint true s p ≤ (s.log p).length := by
  unfold catchUpPoint; split
  · exact Nat.min_le_right _ _
  · exact h.caLen p

end RestartProof
