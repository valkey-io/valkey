import ReconProof.Model

/-! F1 regression for CoreRecon: with ReconcileFirst = FALSE (CatchUpPoint is the new
primary's own commit offset, no reconciliation), the same 11-step run on 3 nodes as
`CoreProof.noReconcile_unsafe` reaches a state violating EligibleHoldsCommitted, at
MinSync = 0. n0 = initial primary. -/

namespace ReconProof

abbrev N := Fin 3

instance (s : State N) (p : N) : Decidable (ThinksPrimary s p) :=
  inferInstanceAs (Decidable ((s.known p).prim = p))
instance (s : State N) (p : N) : Decidable (KnowsLatest s p) :=
  inferInstanceAs (Decidable ((s.known p).ver = s.cfg.ver))

def c0 : State N := init 0
def c1 := count c0 0 1          -- n0 counts n1
def c2 := makeElig c1 0 1       -- n1 eligible (ver 2)
def c3 := appendWrite c2 0      -- n0 appends a write
def c4 := learn c3 1            -- n1 learns ver 2
def c5 := replicate c4 1        -- n1 copies the write
def c6 := ack c5 1              -- n1 acks it
def c7 := commitNext c6 0          -- n0 commits it: hist = [1]
def c8 := takeover c7 1         -- n1 takes over (ver 3), commit[n1] = 0
def c9 := learn c8 2            -- n2 learns ver 3 and follows n1
def c10 := count c9 1 2         -- n1 counts empty n2: ack 0 ≥ commit 0 (no reconciliation)
def c11 := makeElig c10 1 2     -- n2 eligible with an empty log

theorem noRec (s : State N) (p : N) : Reconciled false s p := fun h => absurd h (by decide)

theorem w0 (s : State N) (p : N) : Writable 0 s p := by simp [Writable]

theorem noReconcile_reach : Reachable 0 false (0 : N) c11 := by
  have r0 : Reachable 0 false (0 : N) c0 := Reachable.init
  have r1 := r0.step (Step.count c0 0 1 (by decide) (by decide) (by decide) (by decide) (by decide))
  have r2 := r1.step (Step.makeElig c1 0 1 (by decide) (by decide) (by decide) (by decide))
  have r3 := r2.step (Step.append c2 0 (by decide) (w0 _ _) (noRec _ _))
  have r4 := r3.step (Step.learn c3 1 (by decide))
  have r5 := r4.step (Step.replicate c4 1 (by decide) (by decide) (by decide))
  have r6 := r5.step (Step.ack c5 1 (by decide) (by decide) (by decide))
  have r7 := r6.step (Step.commit c6 0 (by decide) (w0 _ _) (by decide) (by decide))
  have r8 := r7.step (Step.takeover c7 1 (by decide) (by decide) (by decide))
  have r9 := r8.step (Step.learn c8 2 (by decide))
  have r10 := r9.step (Step.count c9 1 2 (by decide) (by decide) (by decide) (by decide) (by decide))
  exact r10.step (Step.makeElig c10 1 2 (by decide) (by decide) (by decide) (by decide))

theorem noReconcile_unsafe : ∃ s, Reachable 0 false (0 : N) s ∧ ¬ Safety s := by
  refine ⟨c11, noReconcile_reach, ?_⟩
  intro ⟨hehc, _⟩
  have := hehc 2 (Or.inl (by decide))
  exact absurd this (by decide)

end ReconProof
