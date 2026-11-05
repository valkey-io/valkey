import Lake
open Lake DSL

package CoreProof

-- Mathlib, pinned to the release matching `lean-toolchain`. Fetch its prebuilt
-- oleans with `lake exe cache get` before the first build.
require mathlib from git
  "https://github.com/leanprover-community/mathlib4" @ "v4.35.0"

@[default_target]
lean_lib CoreProof

-- TLC-Lean cross-check of Core (see LEAN.md, "Cross-check against TLC").
lean_lib XCheck

lean_exe xcheck where
  root := `XCheck.Main

-- The same for CoreAck (held replies, okd); see LEAN.md, "Cross-check of CoreAck".
lean_exe xcheck_ack where
  root := `XCheck.AckMain

@[default_target]
lean_lib Lease

-- CoreAck: acknowledged writes are never lost (see LEAN.md, "Acknowledged writes").
@[default_target]
lean_lib AckProof

-- CoreRecon: safety for every min-sync-replicas value with the inherited-log
-- reconciliation rule (see LEAN.md, "Reconciliation against the inherited log").
@[default_target]
lean_lib ReconProof

-- CoreRestart: restarts with and without data, incarnations, F5 fix; acknowledged
-- writes survive while an authoritative node keeps its data (see LEAN.md, "Restarts").
@[default_target]
lean_lib RestartProof
