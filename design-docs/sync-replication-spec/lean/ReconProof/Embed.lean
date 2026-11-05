import ReconProof.Model
import RestartProof.Model

/-! Embedding of CoreRecon (`ReconProof`) into CoreRestart (`RestartProof`).

A CoreRecon state is the CoreRestart state in which no node has restarted: every
incarnation, and every incarnation a configuration records, is 1 (CoreRestart's
initial value). Under this map every incarnation check holds, ThinksPrim, Writable,
Count and Takeover read as in CoreRecon, and MakeEligible records incarnation 1 again.
So every CoreRecon step is a CoreRestart step (`step_emb`), every CoreRecon reachable
state maps to a CoreRestart reachable state (`reach_emb`), and CoreRestart's Safety of
the image is CoreRecon's Safety (`safety_of_emb`). The restart actions are simply
never used. -/

namespace ReconProof

variable {Node : Type} [DecidableEq Node]

/-- A configuration that records incarnation 1 for every node. -/
def ecfg (c : Cfg Node) : RestartProof.Cfg Node := ⟨c.ver, c.prim, c.elig, fun _ => 1⟩

/-- The CoreRestart state with no restart so far. -/
def emb (s : State Node) : RestartProof.State Node where
  log    := s.log
  inc    := fun _ => 1
  cfg    := ecfg s.cfg
  known  := fun n => ecfg (s.known n)
  sync   := s.sync
  acked  := s.acked
  commit := s.commit
  recon  := s.recon
  term   := s.term
  hist   := s.hist

theorem emb_init (i : Node) : emb (init i) = RestartProof.init i := rfl

theorem ecfg_upd (f : Node → Cfg Node) (p : Node) (c : Cfg Node) :
    (fun n => ecfg (upd f p c n)) = RestartProof.upd (fun n => ecfg (f n)) p (ecfg c) := by
  funext n
  by_cases h : n = p
  · subst h; simp [upd, RestartProof.upd]
  · simp [upd, RestartProof.upd, h]

theorem tp_emb {s : State Node} {p : Node} : RestartProof.tp (emb s) p ↔ tp s p :=
  ⟨fun h => h.1, fun h => ⟨h, rfl⟩⟩

theorem writable_emb {m : Nat} {s : State Node} {p : Node} (h : writable m s p) :
    RestartProof.writable m (emb s) p := by
  obtain ⟨l, hn, hl, hr⟩ := h
  exact ⟨l, hn, hl, fun r hm => ⟨(hr r hm).1, (hr r hm).2, rfl⟩⟩

/-! ### Post-states commute with `emb` -/

section
variable (s : State Node)

theorem emb_appendWrite (p : Node) :
    emb (appendWrite s p) = RestartProof.appendWrite (emb s) p := rfl
theorem emb_replicate (r : Node) : emb (replicate s r) = RestartProof.replicate (emb s) r := rfl
theorem emb_ack (r : Node) : emb (ack s r) = RestartProof.ack (emb s) r := rfl
theorem emb_commitA (p : Node) : emb (commitA s p) = RestartProof.commitA (emb s) p := rfl
theorem emb_count (p r : Node) : emb (count s p r) = RestartProof.count (emb s) p r := rfl
theorem emb_resync (rf : Bool) (r : Node) :
    emb (resync rf s r) = RestartProof.resync rf (emb s) r := rfl

theorem emb_learn (n : Node) : emb (learn s n) = RestartProof.learn (emb s) n := by
  simp only [emb, learn, RestartProof.learn, ecfg_upd]; rfl

theorem emb_makeElig (p r : Node) :
    emb (makeElig s p r) = RestartProof.makeElig (emb s) p r := by
  have hc : RestartProof.eligCfg (emb s) r = ecfg (eligCfg s r true) := by
    simp only [RestartProof.eligCfg, emb, ecfg, eligCfg, RestartProof.Cfg.mk.injEq,
      true_and]
    constructor <;> funext x <;> by_cases h : x = r <;> simp [RestartProof.upd, upd, h]
  simp only [RestartProof.makeElig, hc]
  simp only [emb, makeElig, ecfg_upd]

theorem emb_makeInelig (p r : Node) :
    emb (makeInelig s p r) = RestartProof.makeInelig (emb s) p r := by
  have hc : RestartProof.ineligCfg (emb s) r = ecfg (eligCfg s r false) := rfl
  simp only [RestartProof.makeInelig, hc]
  simp only [emb, makeInelig, ecfg_upd]; rfl

theorem emb_takeover (n : Node) : emb (takeover s n) = RestartProof.takeover (emb s) n := by
  have hc : RestartProof.toCfg (emb s) n = ecfg (toCfg s n) := rfl
  have hs : RestartProof.newSync (emb s) n = newElig s n := by
    funext x
    simp only [RestartProof.newSync, RestartProof.newElig, newElig, emb, ecfg, decide_true,
      Bool.and_true]
    rfl
  simp only [RestartProof.takeover, hc, hs]
  simp only [emb, takeover, ecfg_upd]; rfl

end

/-! ### Steps, reachability and Safety -/

/-- Every CoreRecon step is a CoreRestart step (for either RebuildSync value). -/
theorem step_emb {m : Nat} {rf rb : Bool} {s s' : State Node} (h : Step m rf s s') :
    RestartProof.Step m rf rb (emb s) (emb s') := by
  cases h with
  | append p hp hw hr =>
    rw [emb_appendWrite]; exact .append _ p (tp_emb.2 hp) (writable_emb hw) hr
  | replicate r h1 h2 h3 => rw [emb_replicate]; exact .replicate _ r h1 h2 h3
  | ack r h1 h2 h3 => rw [emb_ack]; exact .ack _ r h1 h2 h3
  | commit p hp hw h1 h2 =>
    rw [emb_commitA]; exact .commit _ p (tp_emb.2 hp) (writable_emb hw) h1 h2
  | count p r hp h1 h2 h3 h4 =>
    rw [emb_count]; exact .count _ p r (tp_emb.2 hp) h1 h2 (fun _ => rfl) h3 h4
  | makeElig p r hp hk h1 h2 =>
    rw [emb_makeElig]; exact .makeElig _ p r (tp_emb.2 hp) hk h1 h2
  | makeInelig p r hp hk h1 =>
    rw [emb_makeInelig]; exact .makeInelig _ p r (tp_emb.2 hp) hk h1
  | learn n hk => rw [emb_learn]; exact .learn _ n hk
  | takeover n hk h1 h2 => rw [emb_takeover]; exact .takeover _ n hk h1 h2 rfl
  | resync r h1 h2 h3 h4 => rw [emb_resync]; exact .resync _ r h1 h2 h3 h4

theorem reach_emb {m : Nat} {rf rb : Bool} {i : Node} {s : State Node}
    (h : Reachable m rf i s) : RestartProof.Reachable m rf rb i (emb s) := by
  induction h with
  | init => rw [emb_init]; exact .init
  | step _ hs ih => exact .step ih (step_emb hs)

/-- With no restart every node's incarnation matches, so `auth` is "eligible or
primary" and CoreRestart's Safety is CoreRecon's. -/
theorem safety_of_emb {s : State Node} (h : RestartProof.Safety (emb s)) : Safety s :=
  ⟨fun n hn => h.1 n ⟨hn, rfl⟩, h.2⟩

end ReconProof
