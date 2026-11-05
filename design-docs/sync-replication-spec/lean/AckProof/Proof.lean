import AckProof.Model
import CoreProof.Safety
import ReconProof.AckProof

/-! Unbounded proof that no acknowledged write is lost in CoreAck (ReconcileFirst =
TRUE, EarlyDrop = FALSE, NoReplyFence = FALSE). Derived from `ReconProof.ackSafety`
at MinSync 0: a CoreAck state is a CoreRecon state with held replies (`atoRecon`),
and every CoreAck step is a CoreRecon step (`astep_toRecon`). `ReconProof.ackSafety`
is in turn derived from `RestartProof.ackSafety`. -/

namespace AckProof

open CoreProof

variable {Node : Type} [DecidableEq Node]

def atoRecon (a : AState Node) : ReconProof.AState Node := ⟨toRecon a.core, a.held, a.okd⟩

theorem fenced_toRecon {s : State Node} {h : Held Node} :
    ReconProof.fenced (toRecon s) h ↔ fenced s h := Iff.rfl

theorem astep_toRecon {a a' : AState Node} (h : AStep true false false a a') :
    ReconProof.AStep 0 true false (atoRecon a) (atoRecon a') := by
  cases h with
  | core c hc => exact .core (atoRecon a) (toRecon c) (step_toRecon hc)
  | write p hp hr => exact .write (atoRecon a) p hp (writable_zero _ p) hr
  | release x hm hc hf => exact .release (atoRecon a) x hm hc hf
  | abort x hm hf => exact .abort (atoRecon a) x hm hf

theorem areach_toRecon {i : Node} {a : AState Node} (h : AReachable true false false i a) :
    ReconProof.AReachable 0 true false i (atoRecon a) := by
  induction h with
  | init => exact .init
  | step _ hs ih => exact .step ih (astep_toRecon hs)

/-- No acknowledged write is lost, for any number of nodes, log length and
configuration changes; with the F1 fix (ReconcileFirst) and the F2 fix (reply fence),
without any lease or `cfg.prim = p` fence (F8). -/
theorem ackSafety {i : Node} {a : AState Node} (h : AReachable true false false i a) :
    AckedDurable a ∧ Safety a.core :=
  ReconProof.ackSafety (areach_toRecon h)

/-- The primary and every eligible replica hold every acknowledged write. -/
theorem ackedHeld {i : Node} {a : AState Node} (h : AReachable true false false i a) :
    AckedHeld a :=
  ReconProof.ackedHeld (areach_toRecon h)

end AckProof
