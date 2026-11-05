import CoreProof.Embed
import ReconProof.Safety

/-! Main theorem: every reachable state of Core (ReconcileFirst = TRUE,
EarlyDrop = FALSE) satisfies Safety, for any node type and unbounded logs/versions.
Derived through the chain Core → CoreRecon at MinSync 0 (`toRecon`) → CoreRestart
with no restart (`ReconProof.emb`), whose invariant proof is `RestartProof`. -/

namespace CoreProof

variable {Node : Type} [DecidableEq Node]

/-- Core_safe, unbounded: any number of nodes (any type with decidable equality),
any log length, any number of configuration changes. -/
theorem safety {i : Node} {s : State Node} (h : Reachable true false i s) : Safety s :=
  safety_of_toRecon (ReconProof.safety (reach_toRecon h))

end CoreProof
