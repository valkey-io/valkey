import XCheck.Ack
import Std.Data.HashMap
import Std.Data.HashSet

/-! `lake exe xcheck_ack [dataDir]`: cross-checks TLC's state graph of
`spec/CoreAck_xcheck.cfg` (converted by `xcheck/dot2lean.py --ack`) against
`AckProof.AStep true false false` and `RestartProof.AInv` of the lifted state. -/

open CoreProof AckProof XCheck

def parseNatsA (line : String) : List Nat :=
  (line.splitOn " ").filter (· ≠ "") |>.map String.toNat!

def actNameA (l : String) : String := (l.splitOn "(").headD l

def reportA (title : String) (bad : Nat) (examples : Array String) : IO Unit := do
  IO.println s!"{title}: {if bad == 0 then "OK" else s!"{bad} MISMATCHES"}"
  for e in examples do IO.println s!"    {e}"

def main (args : List String) : IO UInt32 := do
  let dir := args.headD "XCheck/data_ack"
  let t0 ← IO.monoMsNow
  let sLines ← IO.FS.lines s!"{dir}/states.txt"
  let labels ← IO.FS.lines s!"{dir}/labels.txt"
  let mut states : Array A := #[]
  for i in [0:sLines.size] do
    match decodeA (parseNatsA sLines[i]!) with
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
  -- decoding: Lean's rendering of each decoded state is TLC's text, and TLC's set
  -- order is Lean's canonical order (so every decoded state satisfies cn a = a)
  let mut bad := 0
  let mut badC := 0
  let mut ex : Array String := #[]
  for i in [0:states.size] do
    if renderA states[i]! != labels[i]! then
      bad := bad + 1
      if ex.size < 3 then ex := ex.push s!"state {i}: lean {renderA states[i]!} / tlc {labels[i]!}"
    if cn states[i]! ≠ states[i]! then badC := badC + 1
  reportA s!"decode: renderA(decodeA(state)) = TLC text for {states.size} states" bad ex
  reportA "decode: every decoded state is canonical (cn a = a)" badC #[]
  fails := fails + bad + badC
  let mut idx : Std.HashMap (List Nat) Nat := {}
  for i in [0:states.size] do
    idx := idx.insert (encodeA states[i]!) i
  if idx.size != states.size then
    IO.println s!"encodeA not injective on TLC states: {idx.size} keys"; fails := fails + 1
  let mut edgeSet : Std.HashSet (Nat × Nat × String) := {}
  for e in edges do edgeSet := edgeSet.insert e
  IO.println s!"distinct labelled TLC edges: {edgeSet.size}"
  -- (a) every TLC edge is a Lean AStep with the same label
  bad := 0; ex := #[]
  for (a, b, lb) in edges do
    let s := states[a]!
    let t := states[b]!
    if !astepLB s lb t then
      bad := bad + 1
      if ex.size < 5 then
        let cands := (asuccs s).filter (·.1 == lb)
        let d := match cands with
          | (_, u) :: _ => s!"lean successor differs in {diffFieldsA (cn u) t}"
          | [] => "action not enabled in Lean"
        ex := ex.push s!"{a} -> {b} {lb}: {d}"
  reportA s!"(a) TLC edge is a Lean AStep with the same label (astepLB), {edges.size} edges" bad ex
  fails := fails + bad
  -- (b) every in-bounds Lean successor (other than reply-less AppendWrite) is a TLC edge
  bad := 0; ex := #[]
  let mut leanSet : Std.HashSet (Nat × Nat × String) := {}
  let mut nIn := 0
  let mut nOut := 0
  let mut outBy : Std.HashMap String Nat := {}
  let mut dupH := 0
  let mut dupO := 0
  let mut awIn := 0
  let mut awInTlc := 0
  let mut awOut := 0
  let mut awEx : Array String := #[]
  let mut tlcOOB := 0
  for i in [0:states.size] do
    let s := states[i]!
    if !inBounds s.core then tlcOOB := tlcOOB + 1
    for (lb, u) in asuccs s do
      if hasDup u.held then dupH := dupH + 1
      if hasDup u.okd then dupO := dupO + 1
      let t := cn u
      if lb.startsWith "AppendWrite" then
        if inBounds t.core then
          awIn := awIn + 1
          if idx.contains (encodeA t) then awInTlc := awInTlc + 1
          if awEx.size < 1 then awEx := awEx.push s!"{i} --{lb}--> {renderA t}"
        else awOut := awOut + 1
      else if inBounds t.core then
        nIn := nIn + 1
        match idx.get? (encodeA t) with
        | none =>
          bad := bad + 1
          if ex.size < 5 then ex := ex.push s!"{i} --{lb}--> state not in TLC graph: {renderA t}"
        | some j =>
          if states[j]! ≠ t then
            bad := bad + 1
            if ex.size < 5 then ex := ex.push s!"{i} --{lb}--> hash hit {j} but states differ"
          else if !edgeSet.contains (i, j, lb) then
            bad := bad + 1
            if ex.size < 5 then ex := ex.push s!"{i} --{lb}--> {j}: TLC has no such edge"
          else leanSet := leanSet.insert (i, j, lb)
      else
        nOut := nOut + 1
        let why := (if nodes.any (fun n => (t.core.log n).length > 1) then "log>MaxLen " else "") ++
          (if t.core.hist.length > 1 then "hist>MaxLen " else "") ++
          (if t.core.cfg.ver > 5 then "ver>MaxVer" else "")
        let k := s!"{actNameA lb}: {why}"
        outBy := outBy.insert k (outBy.getD k 0 + 1)
  reportA s!"(b) in-bounds Lean successors other than reply-less AppendWrite are TLC edges ({nIn} labelled successors)" bad ex
  fails := fails + bad
  if leanSet.size != edgeSet.size then
    IO.println s!"    set mismatch: {leanSet.size} distinct in-bounds Lean edges vs {edgeSet.size} TLC edges"
    fails := fails + 1
  else
    IO.println s!"    distinct in-bounds Lean edges = distinct TLC edges = {edgeSet.size}"
  IO.println s!"    TLC states outside bounds: {tlcOOB}"
  IO.println s!"    Lean successors outside bounds: {nOut}"
  for (k, v) in outBy.toList do IO.println s!"      {k} {v}"
  IO.println s!"    Lean successors whose held / okd list had a duplicate before cn: {dupH} / {dupO}"
  IO.println s!"(b') expected superset, AStep.core with AppendWrite (append with no held reply; not in CoreAck.tla):"
  IO.println s!"    in bounds: {awIn}, of which the post-state is a TLC state: {awInTlc}; outside bounds: {awOut}"
  for e in awEx do IO.println s!"    e.g. {e}"
  -- (c) initial state
  let okInit := decide (states[initIdx]! = cn (ainit (0 : XCheck.N)))
  reportA "(c) TLC initial state = AckProof.ainit n1" (if okInit then 0 else 1)
    (if okInit then #[] else #[s!"differs in {diffFieldsA states[initIdx]! (ainit 0)}"])
  fails := fails + (if okInit then 0 else 1)
  -- (d) invariants on every TLC state
  bad := 0; ex := #[]
  let mut badH := 0
  let mut badS := 0
  for i in [0:states.size] do
    if !ainvB states[i]! then
      bad := bad + 1
      if ex.size < 5 then ex := ex.push s!"state {i}"
    if !ackedHeldB states[i]! then badH := badH + 1
    if !safetyB states[i]!.core then badS := badS + 1
  reportA s!"(d) ainvB (RestartProof.AInv, lifted) on all {states.size} states" bad ex
  reportA "(d') ackedHeldB (AckProof.AckedHeld) on all states" badH #[]
  reportA "(d'') safetyB (CoreProof.Safety) on all states" badS #[]
  fails := fails + bad + badH + badS
  let t1 ← IO.monoMsNow
  IO.println s!"total failures: {fails}; time {t1 - t0} ms"
  return (if fails == 0 then 0 else 1)
