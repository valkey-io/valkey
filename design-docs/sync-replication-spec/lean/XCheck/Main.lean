import XCheck.Data
import Std.Data.HashMap
import Std.Data.HashSet

/-! `lake exe xcheck [dataDir]`: cross-checks TLC's state graph of `spec/Core_xcheck.cfg`
(converted by `xcheck/dot2lean.py`) against `CoreProof.Step` and `RestartProof.Inv` of the lifted state. -/

open CoreProof XCheck

def parseNats (line : String) : List Nat :=
  (line.splitOn " ").filter (· ≠ "") |>.map String.toNat!

def actName (l : String) : String := (l.splitOn "(").headD l

def report (title : String) (bad : Nat) (examples : Array String) : IO Unit := do
  IO.println s!"{title}: {if bad == 0 then "OK" else s!"{bad} MISMATCHES"}"
  for e in examples do IO.println s!"    {e}"

def main (args : List String) : IO UInt32 := do
  let dir := args.headD "XCheck/data"
  let t0 ← IO.monoMsNow
  -- decode states
  let sLines ← IO.FS.lines s!"{dir}/states.txt"
  let labels ← IO.FS.lines s!"{dir}/labels.txt"
  let mut states : Array S := #[]
  for i in [0:sLines.size] do
    match decode (parseNats sLines[i]!) with
    | some s => states := states.push s
    | none => IO.println s!"decode failed at state {i}"; return 1
  let eLines ← IO.FS.lines s!"{dir}/edges.txt"
  let mut edges : Array (Nat × Nat × String) := #[]
  for l in eLines do
    match l.splitOn " " with
    | [a, b, lb] => edges := edges.push (a.toNat!, b.toNat!, lb)
    | _ => IO.println s!"bad edge line {l}"; return 1
  let initIdx := ((← IO.FS.readFile s!"{dir}/init.txt").trimAscii.toString).toNat!
  IO.println s!"{states.size} states, {edges.size} edges, init state {initIdx}"
  let mut fails := 0
  -- decoding check: Lean's rendering of each decoded state is TLC's text
  let mut bad := 0
  let mut ex : Array String := #[]
  for i in [0:states.size] do
    if render states[i]! != labels[i]! then
      bad := bad + 1
      if ex.size < 3 then ex := ex.push s!"state {i}: lean {render states[i]!} / tlc {labels[i]!}"
  report s!"decode: render(decode(state)) = TLC text for {states.size} states" bad ex
  fails := fails + bad
  -- index
  let mut idx : Std.HashMap (List Nat) Nat := {}
  for i in [0:states.size] do
    idx := idx.insert (encode states[i]!) i
  if idx.size != states.size then
    IO.println s!"encode not injective on TLC states: {idx.size} keys"; fails := fails + 1
  let mut edgeSet : Std.HashSet (Nat × Nat × String) := {}
  for e in edges do edgeSet := edgeSet.insert e
  IO.println s!"distinct labelled TLC edges: {edgeSet.size}"
  -- (a) every TLC edge is a Lean step, with the same action label
  bad := 0; ex := #[]
  let mut badL := 0
  for (a, b, lb) in edges do
    let s := states[a]!
    let t := states[b]!
    if !stepB s t then
      bad := bad + 1
      if ex.size < 5 then
        let cands := (succs s).filter (·.1 == lb)
        let d := match cands with
          | (_, u) :: _ => s!"lean successor differs in {diffFields u t}"
          | [] => "action not enabled in Lean"
        ex := ex.push s!"{a} -> {b} {lb}: {d}"
    else if !stepLB s lb t then
      badL := badL + 1
      if ex.size < 5 then ex := ex.push s!"{a} -> {b} {lb}: Lean step exists only with another label"
  report s!"(a) TLC edge is a Lean Step (stepB), {edges.size} edges" bad #[]
  report s!"(a') ... with the same action label (stepLB)" badL ex
  fails := fails + bad + badL
  -- (b) completeness within bounds
  bad := 0; ex := #[]
  let mut nIn := 0
  let mut nOut := 0
  let mut outBy : Std.HashMap String Nat := {}
  let mut tlcOOB := 0
  for i in [0:states.size] do
    let s := states[i]!
    if !inBounds s then tlcOOB := tlcOOB + 1
    for (lb, t) in succs s do
      if inBounds t then
        nIn := nIn + 1
        match idx.get? (encode t) with
        | none =>
          bad := bad + 1
          if ex.size < 5 then ex := ex.push s!"{i} --{lb}--> state not in TLC graph"
        | some j =>
          if states[j]! ≠ t then
            bad := bad + 1
            if ex.size < 5 then ex := ex.push s!"{i} --{lb}--> hash hit {j} but states differ"
          else if !edgeSet.contains (i, j, lb) then
            bad := bad + 1
            if ex.size < 5 then ex := ex.push s!"{i} --{lb}--> {j}: TLC has no such edge"
      else
        nOut := nOut + 1
        let why := (if nodes.any (fun n => (t.log n).length > 1) then "log>MaxLen " else "") ++
          (if t.hist.length > 1 then "hist>MaxLen " else "") ++
          (if t.cfg.ver > 5 then "ver>MaxVer" else "")
        let k := s!"{actName lb}: {why}"
        outBy := outBy.insert k (outBy.getD k 0 + 1)
  report s!"(b) in-bounds Lean successors are TLC edges ({nIn} labelled successors)" bad ex
  fails := fails + bad
  IO.println s!"    TLC states outside bounds: {tlcOOB}"
  IO.println s!"    Lean successors outside bounds: {nOut}"
  for (k, v) in outBy.toList do IO.println s!"      {k} {v}"
  if nIn != edgeSet.size then
    IO.println s!"    count mismatch: {nIn} in-bounds Lean successors vs {edgeSet.size} TLC edges"
    fails := fails + 1
  -- (c) initial state
  let okInit := decide (states[initIdx]! = init (0 : N))
  report "(c) TLC initial state = CoreProof.init n1" (if okInit then 0 else 1)
    (if okInit then #[] else #[s!"differs in {diffFields states[initIdx]! (init 0)}"])
  fails := fails + (if okInit then 0 else 1)
  -- (d) invariant and safety on every TLC state
  bad := 0; ex := #[]
  let mut badS := 0
  for i in [0:states.size] do
    if !invB states[i]! then
      bad := bad + 1
      if ex.size < 5 then ex := ex.push s!"state {i}"
    if !safetyB states[i]! then badS := badS + 1
  report s!"(d) invB (RestartProof.Inv, lifted) on all {states.size} states" bad ex
  report s!"(d') safetyB (CoreProof.Safety) on all states" badS #[]
  fails := fails + bad + badS
  let t1 ← IO.monoMsNow
  IO.println s!"total failures: {fails}; time {t1 - t0} ms"
  return (if fails == 0 then 0 else 1)
