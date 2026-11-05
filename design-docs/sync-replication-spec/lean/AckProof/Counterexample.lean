import AckProof.Model

/-! F2 regression: with NoReplyFence = TRUE (release on `commit[p] >= o` alone), a
14-step run on 3 nodes acknowledges a write that is not in the history. Same scenario
as TLC's `CoreAck_noReplyFence` trace. n0 = initial primary. -/

namespace AckProof

open CoreProof

instance (s : State N) (p : N) : Decidable (reconciled true s p) :=
  inferInstanceAs (Decidable (true = true → s.recon p ≤ s.commit p))

def b0 : AState N := ainit 0
def b1 : AState N := ⟨count b0.core 0 1, b0.held, b0.okd⟩       -- n0 counts n1
def b2 : AState N := ⟨makeElig b1.core 0 1, b1.held, b1.okd⟩    -- n1 eligible (ver 2)
def b3 : AState N := write b2 0                                 -- n0 writes at offset 1, held (n0,1,1)
def b4 : AState N := ⟨learn b3.core 1, b3.held, b3.okd⟩         -- n1 learns ver 2
def b5 : AState N := ⟨takeover b4.core 1, b4.held, b4.okd⟩      -- n1 takes over (ver 3), empty log
def b6 : AState N := ⟨learn b5.core 0, b5.held, b5.okd⟩         -- n0 learns it is deposed
def b7 : AState N := ⟨resync b6.core 0, b6.held, b6.okd⟩        -- n0 resynced to the empty log
def b8 : AState N := ⟨count b7.core 1 0, b7.held, b7.okd⟩       -- n1 counts n0
def b9 : AState N := ⟨makeElig b8.core 1 0, b8.held, b8.okd⟩    -- n0 eligible (ver 4)
def b10 : AState N := ⟨learn b9.core 0, b9.held, b9.okd⟩        -- n0 learns ver 4
def b11 : AState N := ⟨takeover b10.core 0, b10.held, b10.okd⟩  -- n0 takes over (ver 5)
def b12 : AState N := write b11 0                               -- n0 writes term 5 at offset 1
def b13 : AState N := ⟨commitA b12.core 0, b12.held, b12.okd⟩   -- commits alone: hist = [5]
def b14 : AState N := release b13 (0, 1, 1)                     -- old reply released OK

theorem noReplyFence_reach : AReachable true false true (0 : N) b14 := by
  have r0 : AReachable true false true (0 : N) b0 := AReachable.init
  have r1 := r0.step (AStep.core b0 _ (Step.count b0.core 0 1 (by decide) (by decide) (by decide) (by decide) (by decide) (by decide)))
  have r2 := r1.step (AStep.core b1 _ (Step.makeElig b1.core 0 1 (by decide) (by decide) (by decide) (by decide)))
  have r3 := r2.step (AStep.write b2 0 (by decide) (by decide))
  have r4 := r3.step (AStep.core b3 _ (Step.learn b3.core 1 (by decide)))
  have r5 := r4.step (AStep.core b4 _ (Step.takeover b4.core 1 (by decide) (by decide) (by decide)))
  have r6 := r5.step (AStep.core b5 _ (Step.learn b5.core 0 (by decide)))
  have r7 := r6.step (AStep.core b6 _ (Step.resync b6.core 0 (by decide) (by decide) (by decide) (by decide) (by decide)))
  have r8 := r7.step (AStep.core b7 _ (Step.count b7.core 1 0 (by decide) (by decide) (by decide) (by decide) (by decide) (by decide)))
  have r9 := r8.step (AStep.core b8 _ (Step.makeElig b8.core 1 0 (by decide) (by decide) (by decide) (by decide)))
  have r10 := r9.step (AStep.core b9 _ (Step.learn b9.core 0 (by decide)))
  have r11 := r10.step (AStep.core b10 _ (Step.takeover b10.core 0 (by decide) (by decide) (by decide)))
  have r12 := r11.step (AStep.write b11 0 (by decide) (by decide))
  have r13 := r12.step (AStep.core b12 _ (Step.commit b12.core 0 (by decide) (by decide) (by decide)))
  exact r13.step (AStep.release b13 (0, 1, 1) (by decide) (by decide) (fun hc => absurd hc (by decide)))

theorem noReplyFence_unsafe : ∃ a, AReachable true false true (0 : N) a ∧ ¬ AckedDurable a := by
  refine ⟨b14, noReplyFence_reach, ?_⟩
  intro hd
  have := (hd (1, 1) (by decide)).2.2
  exact absurd this (by decide)

end AckProof
