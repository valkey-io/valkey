/-!
# Lease timing, additive drift model (FINDINGS F11)

Real time and local clock readings are integers (ticks). A clock is a function
`c : Int → Int` from real time to local reading. The additive drift bound is the one in
`spec/Leases.tla`: measured from an anchor `a` (the start of one lease interval), local
elapsed time differs from real elapsed time by at most `D`.

One lease instance: the holder sends a message at real time `s`, the grantor receives it
at real time `r` with `s ≤ r`. The delay `r - s` is unbounded.

* The holder believes its lease valid at `t` while its local elapsed time since `s` is
  `< W`.
* The grantor's promise is over at `t` once its local elapsed time since `r` is `≥ G`.

The asymmetric rule (the model's) is `W = Dur - M`, `G = Dur`. The doc's literal reading
("both sides subtract a margin") is `W = Dur - M`, `G = Dur - M`.
-/

namespace Lease

/-- Additive drift from anchor `a`: for every real time `t ≥ a`, local elapsed time is
within `D` of real elapsed time. -/
def AddDrift (c : Int → Int) (a D : Int) : Prop :=
  ∀ t, a ≤ t → t - a - D ≤ c t - c a ∧ c t - c a ≤ t - a + D

/-- A monotone (non-decreasing) clock. -/
def Mono (c : Int → Int) : Prop := ∀ t1 t2, t1 ≤ t2 → c t1 ≤ c t2

/-- The holder believes its lease valid at real time `t`: local elapsed since `s` is `< W`. -/
def Valid (cH : Int → Int) (s W t : Int) : Prop := s ≤ t ∧ cH t - cH s < W

/-- The grantor's promise is over at real time `t`: local elapsed since `r` is `≥ G`. -/
def Over (cG : Int → Int) (r G t : Int) : Prop := r ≤ t ∧ G ≤ cG t - cG r

/-- An overlap: some real time at which the holder still believes its lease valid and the
grantor already considers its promise over. -/
def Overlap (cH cG : Int → Int) (s r W G : Int) : Prop := ∃ t, Valid cH s W t ∧ Over cG r G t

/-! ## General form -/

/-- If `G ≥ W + 2D`, no overlap: for any delay, any clocks within the drift bound
(monotone or not), any real time. -/
theorem no_overlap (cH cG : Int → Int) (s r W G D : Int) (hsr : s ≤ r)
    (hH : AddDrift cH s D) (hG : AddDrift cG r D) (hc : W + 2 * D ≤ G) :
    ¬ Overlap cH cG s r W G := by
  rintro ⟨t, ⟨h1, h2⟩, ⟨h3, h4⟩⟩
  have := hH t h1
  have := hG t h3
  omega

/-- A holder with a monotone clock and `W ≤ 0` never believes its lease valid. -/
theorem never_valid (cH : Int → Int) (s W t : Int) (hm : Mono cH) (hW : W ≤ 0) :
    ¬ Valid cH s W t := by
  rintro ⟨h1, h2⟩
  have := hm s t h1
  omega

/-- The slow witness clock: runs `D` behind real time from 0 on (stopped on `[0, D]`). -/
def slowClock (D : Int) (t : Int) : Int := t - min (max t 0) D
/-- The fast witness clock: runs `D` ahead of real time from 0 on (double rate on `[0, D]`). -/
def fastClock (D : Int) (t : Int) : Int := t + min (max t 0) D

theorem slowClock_mono (D : Int) (hD : 0 ≤ D) : Mono (slowClock D) := by
  intro a b h; simp only [slowClock]; omega
theorem fastClock_mono (D : Int) (hD : 0 ≤ D) : Mono (fastClock D) := by
  intro a b h; simp only [fastClock]; omega
theorem slowClock_drift (D : Int) (hD : 0 ≤ D) : AddDrift (slowClock D) 0 D := by
  intro t h; simp only [slowClock]; omega
theorem fastClock_drift (D : Int) (hD : 0 ≤ D) : AddDrift (fastClock D) 0 D := by
  intro t h; simp only [fastClock]; omega

/-- Tightness: if `G < W + 2D` and `W > 0`, there is an overlap with zero delay
(`s = r = 0`), monotone clocks within the drift bound, the holder slow and the grantor
fast, at real time `max D (G - D)`. -/
theorem overlap_witness (W G D : Int) (hD : 0 ≤ D) (hW : 0 < W) (hc : G < W + 2 * D) :
    Overlap (slowClock D) (fastClock D) 0 0 W G := by
  refine ⟨max D (G - D), ⟨?_, ?_⟩, ⟨?_, ?_⟩⟩ <;> (try simp only [slowClock, fastClock]) <;> omega

/-- Exact condition. For `D ≥ 0`, every lease instance with monotone clocks within the
drift bound is free of overlap if and only if `W + 2D ≤ G` or `W ≤ 0` (the holder's lease
is never valid). -/
theorem safe_iff (W G D : Int) (hD : 0 ≤ D) :
    (∀ (cH cG : Int → Int) (s r : Int), s ≤ r → Mono cH → Mono cG →
        AddDrift cH s D → AddDrift cG r D → ¬ Overlap cH cG s r W G)
    ↔ (W + 2 * D ≤ G ∨ W ≤ 0) := by
  constructor
  · intro h
    by_cases hc : W + 2 * D ≤ G
    · exact Or.inl hc
    · by_cases hW : W ≤ 0
      · exact Or.inr hW
      · exact absurd (overlap_witness W G D hD (by omega) (by omega))
          (h _ _ 0 0 (Int.le_refl 0) (slowClock_mono D hD) (fastClock_mono D hD)
            (slowClock_drift D hD) (fastClock_drift D hD))
  · rintro hc cH cG s r hsr hmH _ hH hG ⟨t, hv, ho⟩
    cases hc with
    | inl hc => exact no_overlap cH cG s r W G D hsr hH hG hc ⟨t, hv, ho⟩
    | inr hW => exact never_valid cH s W t hmH hW hv

/-! ## Theorem 1: safety of the asymmetric rule -/

/-- **Safety (asymmetric rule).** Holder valid while local elapsed since `s` is
`< Dur - M`; grantor's promise over once local elapsed since `r` is `≥ Dur`. If
`M ≥ 2 * D`, then at no real time are both true, for every `s ≤ r` (unbounded delay),
every clock behaviour within the additive drift bound, and every `Dur`, `D`, `M`. -/
theorem asym_safe (cH cG : Int → Int) (s r Dur D M t : Int) (hsr : s ≤ r)
    (hH : AddDrift cH s D) (hG : AddDrift cG r D) (hM : 2 * D ≤ M) :
    ¬ (Valid cH s (Dur - M) t ∧ Over cG r Dur t) := fun h =>
  no_overlap cH cG s r (Dur - M) Dur D hsr hH hG (by omega) ⟨t, h⟩

/-! ## Theorem 2: tightness -/

/-- **Tightness.** If `M < 2 * D` (and `M < Dur`, so the holder's lease can be valid at
all), there is an overlap: zero delay, holder clock `D` slow, grantor clock `D` fast. -/
theorem asym_tight (Dur D M : Int) (hD : 0 ≤ D) (hM : M < 2 * D) (hDur : M < Dur) :
    Overlap (slowClock D) (fastClock D) 0 0 (Dur - M) Dur :=
  overlap_witness (Dur - M) Dur D hD (by omega) (by omega)

/-- A fully concrete instance of tightness: `Dur = 10`, `D = 2`, `M = 3`, overlap at real
time 8 (holder local elapsed 6 < 7, grantor local elapsed 10 ≥ 10). -/
example : Valid (slowClock 2) 0 (10 - 3) 8 ∧ Over (fastClock 2) 0 10 8 := by
  refine ⟨⟨?_, ?_⟩, ?_, ?_⟩ <;> (try simp only [slowClock, fastClock]) <;> omega

/-- Exact condition for the asymmetric rule: safe for all clocks iff `M ≥ 2D` or `Dur ≤ M`. -/
theorem asym_safe_iff (Dur D M : Int) (hD : 0 ≤ D) :
    (∀ (cH cG : Int → Int) (s r : Int), s ≤ r → Mono cH → Mono cG →
        AddDrift cH s D → AddDrift cG r D → ¬ Overlap cH cG s r (Dur - M) Dur)
    ↔ (2 * D ≤ M ∨ Dur ≤ M) := by
  rw [safe_iff _ _ _ hD]; omega

/-! ## Theorem 3: the doc's literal reading (F11) -/

/-- **F11.** With both sides subtracting the margin (grantor's promise over once local
elapsed since `r` is `≥ Dur - M`), there is an overlap for every margin `M` as soon as
`D > 0` (and `M < Dur`, so the holder's lease is ever valid). -/
theorem literal_unsafe (Dur D M : Int) (hD : 0 < D) (hDur : M < Dur) :
    Overlap (slowClock D) (fastClock D) 0 0 (Dur - M) (Dur - M) :=
  overlap_witness (Dur - M) (Dur - M) D (by omega) (by omega) (by omega)

/-- Exact condition for the literal reading: safe for all clocks iff `D = 0` or
`Dur ≤ M`. The margin's size never matters. -/
theorem literal_safe_iff (Dur D M : Int) (hD : 0 ≤ D) :
    (∀ (cH cG : Int → Int) (s r : Int), s ≤ r → Mono cH → Mono cG →
        AddDrift cH s D → AddDrift cG r D → ¬ Overlap cH cG s r (Dur - M) (Dur - M))
    ↔ (D = 0 ∨ Dur ≤ M) := by
  rw [safe_iff _ _ _ hD]; omega

/-- Concrete F11 instance: `Dur = 10`, `M = 4`, `D = 1`, overlap at real time 6 (holder
local elapsed 5 < 6, grantor local elapsed 7 ≥ 6). -/
example : Valid (slowClock 1) 0 (10 - 4) 6 ∧ Over (fastClock 1) 0 (10 - 4) 6 := by
  refine ⟨⟨?_, ?_⟩, ?_, ?_⟩ <;> (try simp only [slowClock, fastClock]) <;> omega

end Lease
