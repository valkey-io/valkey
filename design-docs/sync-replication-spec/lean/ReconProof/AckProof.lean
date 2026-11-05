import ReconProof.AckModel
import ReconProof.Safety
import RestartProof.AckProof

/-! No acknowledged write is lost in CoreRecon, for every MinSync (ReconcileFirst =
TRUE, NoReplyFence = FALSE). Derived from `RestartProof.ackSafety`: a CoreRecon
state with held replies is a CoreRestart state with no restart (`aemb`), every
CoreRecon step is a CoreRestart step (`astep_emb`), and the properties read the
same on the image. -/

namespace ReconProof

variable {Node : Type} [DecidableEq Node]

/-- `emb` on the replication part; held replies and acknowledged pairs unchanged. -/
def aemb (a : AState Node) : RestartProof.AState Node := ⟨emb a.core, a.held, a.okd⟩

theorem fenced_emb {s : State Node} {h : Held Node} :
    RestartProof.fenced (emb s) h ↔ fenced s h :=
  ⟨fun ⟨h1, h2⟩ => ⟨tp_emb.1 h1, h2⟩, fun ⟨h1, h2⟩ => ⟨tp_emb.2 h1, h2⟩⟩

theorem astep_emb {m : Nat} {rf nf rb : Bool} {a a' : AState Node} (h : AStep m rf nf a a') :
    RestartProof.AStep m rf nf rb (aemb a) (aemb a') := by
  cases h with
  | core c hc => exact .core (aemb a) (emb c) (step_emb hc)
  | write p hp hw hr => exact .write (aemb a) p (tp_emb.2 hp) (writable_emb hw) hr
  | release x hm hc hf =>
    exact .release (aemb a) x hm hc (fun e => fenced_emb.2 (hf e))
  | abort x hm hf => exact .abort (aemb a) x hm (fun e => hf (fenced_emb.1 e))

theorem areach_emb {m : Nat} {rf nf rb : Bool} {i : Node} {a : AState Node}
    (h : AReachable m rf nf i a) : RestartProof.AReachable m rf nf rb i (aemb a) := by
  induction h with
  | init => exact .init
  | step _ hs ih => exact .step ih (astep_emb hs)

/-- No acknowledged write is lost, for any number of nodes, log length and
configuration changes, and EVERY min-sync-replicas value `m`; with reconciliation
against the inherited log (A1) and the F2 fix (reply fence). -/
theorem ackSafety {i : Node} {a : AState Node} {m : Nat} (h : AReachable m true false i a) :
    AckedDurable a ∧ Safety a.core :=
  let hr := RestartProof.ackSafety (areach_emb (rb := true) h)
  ⟨hr.1, safety_of_emb hr.2⟩

/-- The primary and every eligible replica hold every acknowledged write. -/
theorem ackedHeld {i : Node} {a : AState Node} {m : Nat} (h : AReachable m true false i a) :
    AckedHeld a := fun x hx n hn =>
  RestartProof.ackedHeld (areach_emb (rb := true) h) x hx n ⟨hn, rfl⟩

end ReconProof
