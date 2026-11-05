import ReconProof.Embed
import RestartProof.Safety

/-! Main theorem: every reachable state of CoreRecon's replication part
(ReconcileFirst = TRUE) satisfies Safety, for EVERY MinSync `m`, any node type,
unbounded logs and versions. Derived from `RestartProof.safety` through the
embedding `emb` (a CoreRecon run is a CoreRestart run with no restart). -/

namespace ReconProof

variable {Node : Type} [DecidableEq Node]

/-- Safety for every min-sync-replicas value `m`. -/
theorem safety {m : Nat} {i : Node} {s : State Node} (h : Reachable m true i s) : Safety s :=
  safety_of_emb (RestartProof.safety (reach_emb (rb := true) h))

end ReconProof
