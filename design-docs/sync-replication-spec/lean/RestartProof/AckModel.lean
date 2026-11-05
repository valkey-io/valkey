import RestartProof.Safety

/-! Encoding of `spec/CoreRestart.tla`: the replication part (`RestartProof.Step`) plus
held client replies, as in `ReconProof.AckModel`. A restart drops the held replies of
the restarted node (`restartE`, `restartK`): they are lost with the client connections,
so they are neither acknowledged nor answered UNCOMMITTED.

A held reply is `(p, o, t)`: primary `p`, 1-based offset `o`, tag `t = term[p]` at the
append. In Core a log entry is the appending primary's term, so `t` is also the value
written. `okd` holds the acknowledged `(offset, value)` pairs.

`AStep.core` allows every replication step with `held`/`okd` unchanged, including
`AppendWrite` without a reply and a restart that keeps the node's held replies.
CoreRestart.tla always records a reply on append and drops them on restart, so this
model has more behaviours than the TLA+ one; a safety theorem for it covers
CoreRestart.tla. `MaxLen` / `MaxVer` / `MaxRestarts` are dropped. -/

namespace RestartProof


/-- (primary, offset, tag) -/
abbrev Held (Node : Type) := Node × Nat × Nat

structure AState (Node : Type) [DecidableEq Node] where
  core : State Node
  held : List (Held Node)
  okd  : List (Nat × Nat)

variable {Node : Type} [DecidableEq Node]

def ainit (i : Node) : AState Node := ⟨init i, [], []⟩

/-- `Write(p)`: Core's AppendWrite plus a held reply at the new length. -/
def write (a : AState Node) (p : Node) : AState Node :=
  ⟨appendWrite a.core p, (p, (a.core.log p).length + 1, a.core.term p) :: a.held, a.okd⟩

/-- `Fenced(h)`: the primaryship that set the reply boundary is still current
(F2 fix). -/
def fenced (s : State Node) (h : Held Node) : Prop := ThinksPrimary s h.1 ∧ s.term h.1 = h.2.2

/-- `Release(h)`: OK. -/
def release (a : AState Node) (h : Held Node) : AState Node :=
  ⟨a.core, a.held.erase h, (h.2.1, h.2.2) :: a.okd⟩

/-- `Abort(h)`: UNCOMMITTED. -/
def abort (a : AState Node) (h : Held Node) : AState Node :=
  ⟨a.core, a.held.erase h, a.okd⟩

/-- The held replies left after `n` restarts. -/
def dropHeld (held : List (Held Node)) (n : Node) : List (Held Node) :=
  held.filter (fun x => x.1 != n)

/-- `m` = MinSync, `rf` = ReconcileFirst, `nf` = NoReplyFence, `rb` = RebuildSync. -/
inductive AStep (m : Nat) (rf nf rb : Bool) : AState Node → AState Node → Prop
  | core (a : AState Node) (c : State Node) : Step m rf rb a.core c →
      AStep m rf nf rb a ⟨c, a.held, a.okd⟩
  | write (a : AState Node) (p : Node) : ThinksPrimary a.core p → Writable m a.core p →
      Reconciled rf a.core p → AStep m rf nf rb a (write a p)
  | release (a : AState Node) (h : Held Node) : h ∈ a.held → h.2.1 ≤ a.core.commit h.1 →
      (nf = false → fenced a.core h) → AStep m rf nf rb a (release a h)
  | abort (a : AState Node) (h : Held Node) : h ∈ a.held → ¬ fenced a.core h →
      AStep m rf nf rb a (abort a h)
  | restartE (a : AState Node) (n : Node) : KnowsLatest a.core n → (∃ m, m ≠ n ∧ IsAuthoritative a.core m) →
      AStep m rf nf rb a ⟨restartEmpty a.core n, dropHeld a.held n, a.okd⟩
  | restartK (a : AState Node) (n : Node) : KnowsLatest a.core n →
      AStep m rf nf rb a ⟨restartKeep rb a.core n, dropHeld a.held n, a.okd⟩

inductive AReachable (m : Nat) (rf nf rb : Bool) (i : Node) : AState Node → Prop
  | init : AReachable m rf nf rb i (ainit i)
  | step {a a'} : AReachable m rf nf rb i a → AStep m rf nf rb a a' → AReachable m rf nf rb i a'

/-- AckedDurable: `Len(hist) >= o /\ hist[o] = v` (TLA `hist[o]` is `getD (o-1)`). -/
def AckedDurable (a : AState Node) : Prop :=
  ∀ x ∈ a.okd, 1 ≤ x.1 ∧ x.1 ≤ a.core.hist.length ∧ a.core.hist.getD (x.1 - 1) 0 = x.2

/-- AckedHeld: every authoritative node holds every acknowledged write. -/
def AckedHeld (a : AState Node) : Prop :=
  ∀ x ∈ a.okd, ∀ n, IsAuthoritative a.core n →
    x.1 ≤ (a.core.log n).length ∧ (a.core.log n).getD (x.1 - 1) 0 = x.2

end RestartProof
