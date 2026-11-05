import XCheck.Basic

/-! Decoding of the numeric state encoding written by `xcheck/dot2lean.py`, its inverse
`encode`, and `render`, which prints a state in TLC's syntax (whitespace removed).
`XCheck.Main` checks `render (decode line_i) = label_i` for every state, where `label_i`
is TLC's own text for state `i`. That test makes the decoding trustworthy independently
of the Python parser. -/

namespace XCheck
open CoreProof

instance : Inhabited S := ⟨init 0⟩

/-- Read one Nat from the front of the list. -/
def take1 : List Nat → Option (Nat × List Nat)
  | x :: xs => some (x, xs)
  | [] => none

def takeN : Nat → List Nat → Option (List Nat × List Nat)
  | 0, l => some ([], l)
  | k + 1, l => do
    let (x, l) ← take1 l
    let (xs, l) ← takeN k l
    pure (x :: xs, l)

def takeSeq (l : List Nat) : Option (List Nat × List Nat) := do
  let (n, l) ← take1 l
  takeN n l

def toNode (n : Nat) : Option N := if h : n < 3 then some ⟨n, h⟩ else none

def maskFn (m : Nat) : N → Bool := fun i => m.testBit i.val

def fn3 {α : Type} (a b c : α) : N → α := fun i => if i.val = 0 then a else if i.val = 1 then b else c

def takeCfg (l : List Nat) : Option (Cfg N × List Nat) := do
  let (v, l) ← take1 l
  let (p, l) ← take1 l
  let (e, l) ← take1 l
  let p ← toNode p
  if e < 8 then pure (⟨v, p, maskFn e⟩, l) else none

def take3 {α : Type} (f : List Nat → Option (α × List Nat)) (l : List Nat) :
    Option ((N → α) × List Nat) := do
  let (a, l) ← f l
  let (b, l) ← f l
  let (c, l) ← f l
  pure (fn3 a b c, l)

def takeMask (l : List Nat) : Option ((N → Bool) × List Nat) := do
  let (m, l) ← take1 l
  if m < 8 then pure (maskFn m, l) else none

def decode (l : List Nat) : Option S := do
  let (log, l) ← take3 takeSeq l
  let (cfg, l) ← takeCfg l
  let (known, l) ← take3 takeCfg l
  let (sync, l) ← take3 takeMask l
  let (acked, l) ← take3 (take3 take1) l
  let (commit, l) ← take3 take1 l
  let (recon, l) ← take3 take1 l
  let (term, l) ← take3 take1 l
  let (hist, l) ← takeSeq l
  if l.isEmpty then
    pure { log, cfg, known, sync, acked, commit, recon, term, hist }
  else none

def maskOf (f : N → Bool) : Nat := nodes.foldr (fun i acc => acc + if f i then 2 ^ i.val else 0) 0

def encCfg (c : Cfg N) : List Nat := [c.ver, c.prim.val, maskOf c.elig]
def encSeq (l : List Nat) : List Nat := l.length :: l

/-- Same layout as `decode`; used as hash key. -/
def encode (s : S) : List Nat :=
  nodes.flatMap (fun n => encSeq (s.log n)) ++ encCfg s.cfg ++
  nodes.flatMap (fun n => encCfg (s.known n)) ++ nodes.map (fun n => maskOf (s.sync n)) ++
  nodes.flatMap (fun n => nodes.map (s.acked n)) ++ nodes.map s.commit ++
  nodes.map s.recon ++ nodes.map s.term ++ encSeq s.hist

/-! ### TLC syntax -/

def sep (xs : List String) (d : String) : String := d.intercalate xs
def rSet (f : N → Bool) : String := "{" ++ sep ((nodes.filter f).map nodeName) "," ++ "}"
def rSeq (l : List Nat) : String := "<<" ++ sep (l.map toString) "," ++ ">>"
def rFn {α : Type} (r : α → String) (f : N → α) : String :=
  "(" ++ sep (nodes.map (fun n => nodeName n ++ ":>" ++ r (f n))) "@@" ++ ")"
def rCfg (c : Cfg N) : String :=
  "[prim|->" ++ nodeName c.prim ++ ",ver|->" ++ toString c.ver ++ ",elig|->" ++ rSet c.elig ++ "]"

/-- TLC's variable order in the dump. -/
def render (s : S) : String :=
  "/\\sync=" ++ rFn rSet s.sync ++ "/\\cfg=" ++ rCfg s.cfg ++
  "/\\term=" ++ rFn toString s.term ++ "/\\recon=" ++ rFn toString s.recon ++
  "/\\known=" ++ rFn rCfg s.known ++ "/\\commit=" ++ rFn toString s.commit ++
  "/\\hist=" ++ rSeq s.hist ++ "/\\log=" ++ rFn rSeq s.log ++
  "/\\acked=" ++ rFn (rFn toString) s.acked

/-- Fields in which two states differ (for mismatch reports). -/
def diffFields (a b : S) : List String :=
  (if a.log = b.log then [] else ["log"]) ++ (if a.cfg = b.cfg then [] else ["cfg"]) ++
  (if a.known = b.known then [] else ["known"]) ++ (if a.sync = b.sync then [] else ["sync"]) ++
  (if a.acked = b.acked then [] else ["acked"]) ++ (if a.commit = b.commit then [] else ["commit"]) ++
  (if a.recon = b.recon then [] else ["recon"]) ++ (if a.term = b.term then [] else ["term"]) ++
  (if a.hist = b.hist then [] else ["hist"])

end XCheck
