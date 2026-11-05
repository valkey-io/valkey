import Lease.Additive

/-!
# Lease durations (doc section 5 "Lease durations"), additive drift model

A sync replica R fails. Timeline, in real time:
* `sp`: the primary P sends the stream data at offset X. P's primary lease from R runs
  from `sp` (P is the holder, R the grantor).
* `sa`: R sends its REPLCONF ACK for X (`sp ≤ sa`). R's read lease runs from `sa`
  (R is the holder, P the grantor).
* `ra`: P receives that ACK (`sa ≤ ra`). It is the last ACK from R. The round trip
  `ra - sp` is at most `Δ`.
* `w`: P's wait for R's read lease ends: the earliest time with local elapsed since `ra`
  `≥ ReadDur` (asymmetric rule). P then proposes the demotion to the cluster bus.
* `d`: the demotion commits, at most `B` real ticks later.

The doc's claim: if read lease + bus commit time + drift margin < primary lease, then the
demotion commits before P's own lease from R would expire. With the additive model the
sufficient condition is `ReadDur + B + Δ + M + 2D < PrimDur`.
-/

namespace Lease

/-- **Lease durations.** Under `ReadDur + B + Δ + M + 2 * D < PrimDur`, at the commit
time `d` of the demotion: P's primary lease from R is still valid (so P need not stop
serving reads), and R's read lease is already invalid (if `M ≥ 2D`). -/
theorem demotion_in_time (cP cR : Int → Int) (sp sa ra w d ReadDur PrimDur B Δ D M : Int)
    (hmP : Mono cP)
    (hPs : AddDrift cP sp D) (hPr : AddDrift cP ra D) (hR : AddDrift cR sa D)
    (h1 : sp ≤ sa) (h2 : sa ≤ ra) (hΔ : ra - sp ≤ Δ)
    (hw : ra ≤ w) (hwait : ReadDur ≤ cP w - cP ra)
    (hmin : ∀ w', ra ≤ w' → w' < w → cP w' - cP ra < ReadDur)
    (hd : w ≤ d) (hB : d - w ≤ B) (hRead : 0 ≤ ReadDur)
    (hM : 2 * D ≤ M) (hcond : ReadDur + B + Δ + M + 2 * D < PrimDur) :
    Valid cP sp (PrimDur - M) d ∧ ¬ Valid cR sa (ReadDur - M) d := by
  -- the wait ends within ReadDur + D real ticks of ra
  have hwr : w - ra ≤ ReadDur + D := by
    by_cases he : w = ra
    · have := (hPr ra (Int.le_refl _)).1; omega
    · have h := hmin (w - 1) (by omega) (by omega)
      have := (hPr (w - 1) (by omega)).1
      omega
  refine ⟨⟨by omega, ?_⟩, ?_⟩
  · have := (hPs d (by omega)).2
    omega
  · intro hv
    have hm := hmP w d hd
    exact asym_safe cR cP sa ra ReadDur D M d h2 hR hPr hM ⟨hv, ⟨by omega, by omega⟩⟩

/-- The doc's condition (`ReadDur + B + M < PrimDur`, here with `M = 2D`, zero round
trip) is not enough in the additive model: `D = 1`, `M = 2`, `ReadDur = 2`, `B = 1`,
`PrimDur = 6`. P's clock runs 1 slow while waiting (the wait ends at real 3), then 1 fast,
so at the commit time 4 its local elapsed is 5 ≥ `PrimDur - M` = 4: the lease has expired. -/
def cex1 (t : Int) : Int := if t ≤ 0 then t else if t ≤ 3 then t - 1 else t + 1

theorem doc_condition_insufficient_drift :
    Mono cex1 ∧ AddDrift cex1 0 1 ∧
    (∀ w', 0 ≤ w' → w' < 3 → cex1 w' - cex1 0 < 2) ∧ 2 ≤ cex1 3 - cex1 0 ∧
    2 + 1 + 2 < 6 ∧ ¬ Valid cex1 0 (6 - 2) 4 := by
  refine ⟨?_, ?_, ?_, by decide, by decide, by unfold Valid cex1; decide⟩
  · intro a b h; simp only [cex1]; split <;> split <;> (try split) <;> (try split) <;> omega
  · intro t h; simp only [cex1]; split <;> (try split) <;> omega
  · intro w' h1 h2; simp only [cex1]; split <;> (try split) <;> omega

/-- The round trip also matters: with no drift (`D = M = 0`), `ReadDur = 2`, `B = 1`,
`PrimDur = 4` satisfy the doc's condition, but with round trip 2 (`sp = 0`, `ra = 2`) the
wait ends at 4 and the commit at 5, after the lease from `sp` expired at 4. -/
def idClock (t : Int) : Int := t

theorem doc_condition_insufficient_rtt :
    2 + 1 + 0 < 4 ∧ AddDrift idClock 0 0 ∧ AddDrift idClock 2 0 ∧
    (∀ w', 2 ≤ w' → w' < 4 → idClock w' - idClock 2 < 2) ∧ 2 ≤ idClock 4 - idClock 2 ∧
    ¬ Valid idClock 0 (4 - 0) 5 := by
  refine ⟨by decide, ?_, ?_, ?_, by decide, by unfold Valid idClock; decide⟩
  · intro t _; simp only [idClock]; omega
  · intro t _; simp only [idClock]; omega
  · intro w' _ _; simp only [idClock]; omega

end Lease
