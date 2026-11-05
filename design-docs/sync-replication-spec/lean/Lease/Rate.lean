import Lease.Additive

/-!
# Lease timing, rate-bounded clock model

The drift rate is `ρ = a / b` with integers `0 ≤ a < b`. Lean core has no ordered-field
library for `Rat`, so everything is scaled to integers: a local clock `c : Int → Int`
reads in units of `1/b` real tick, and the rate bound
`(1 - ρ)(t2 - t1) ≤ c(t2) - c(t1) ≤ (1 + ρ)(t2 - t1)` becomes, after multiplying by `b`,
`(b - a)(t2 - t1) ≤ c t2 - c t1 ≤ (b + a)(t2 - t1)` for all real `t1 ≤ t2`.
A duration `X` real ticks corresponds to `b * X` local units. Such clocks are monotone
(`a < b`).
-/

namespace Lease

/-- Rate-bounded clock with drift rate `a/b`, reading in units of `1/b` tick. -/
def RateClock (a b : Int) (c : Int → Int) : Prop :=
  ∀ t1 t2, t1 ≤ t2 → (b - a) * (t2 - t1) ≤ c t2 - c t1 ∧ c t2 - c t1 ≤ (b + a) * (t2 - t1)

/-- Holder valid at `t`: local elapsed since `s` below `W` real ticks (i.e. `b * W` units). -/
def RValid (b : Int) (cH : Int → Int) (s W t : Int) : Prop := s ≤ t ∧ cH t - cH s < b * W
/-- Grantor's promise over at `t`: local elapsed since `r` at least `G` real ticks. -/
def ROver (b : Int) (cG : Int → Int) (r G t : Int) : Prop := r ≤ t ∧ b * G ≤ cG t - cG r

theorem rate_mono (a b : Int) (c : Int → Int) (hab : a < b) (h : RateClock a b c) : Mono c := by
  intro t1 t2 h12
  have h1 := (h t1 t2 h12).1
  have : 0 ≤ (b - a) * (t2 - t1) := Int.mul_nonneg (by omega) (by omega)
  omega

/-- General rate-form safety: if `(b + a) * W ≤ (b - a) * G`, i.e. `W/(1-ρ) ≤ G/(1+ρ)`
in real time, there is no overlap, for every delay `s ≤ r` and all rate-bounded clocks. -/
theorem rate_no_overlap (a b : Int) (cH cG : Int → Int) (s r W G t : Int)
    (ha : 0 ≤ a) (hab : a < b) (hsr : s ≤ r)
    (hH : RateClock a b cH) (hG : RateClock a b cG) (hc : (b + a) * W ≤ (b - a) * G) :
    ¬ (RValid b cH s W t ∧ ROver b cG r G t) := by
  rintro ⟨⟨h1, h2⟩, ⟨h3, h4⟩⟩
  have e1 := (hH s t h1).1
  have e2 := (hG r t h3).2
  -- x = t - s, y = t - r, y ≤ x
  have q1 : (b - a) * (t - s) < b * W := Int.lt_of_le_of_lt e1 h2
  have q2 : b * G ≤ (b + a) * (t - s) :=
    Int.le_trans (Int.le_trans h4 e2) (Int.mul_le_mul_of_nonneg_left (by omega) (by omega))
  have p1 : (b + a) * ((b - a) * (t - s)) < (b + a) * (b * W) :=
    Int.mul_lt_mul_of_pos_left q1 (by omega)
  have p2 : b * ((b + a) * W) ≤ b * ((b - a) * G) :=
    Int.mul_le_mul_of_nonneg_left hc (by omega)
  have p3 : (b - a) * (b * G) ≤ (b - a) * ((b + a) * (t - s)) :=
    Int.mul_le_mul_of_nonneg_left q2 (by omega)
  have r1 : (b + a) * ((b - a) * (t - s)) = (b - a) * ((b + a) * (t - s)) := Int.mul_left_comm ..
  have r2 : (b + a) * (b * W) = b * ((b + a) * W) := Int.mul_left_comm ..
  have r3 : b * ((b - a) * G) = (b - a) * (b * G) := Int.mul_left_comm ..
  omega

/-- **Safety (asymmetric rule), rate form.** Holder valid below `Dur - M`, grantor over at
`Dur`. If `2 * a * Dur ≤ (b + a) * M`, i.e. `M ≥ 2ρ·Dur / (1 + ρ)`, there is no overlap. -/
theorem rate_asym_safe (a b : Int) (cH cG : Int → Int) (s r Dur M t : Int)
    (ha : 0 ≤ a) (hab : a < b) (hsr : s ≤ r)
    (hH : RateClock a b cH) (hG : RateClock a b cG) (hM : 2 * a * Dur ≤ (b + a) * M) :
    ¬ (RValid b cH s (Dur - M) t ∧ ROver b cG r Dur t) := by
  apply rate_no_overlap a b cH cG s r (Dur - M) Dur t ha hab hsr hH hG
  have e1 : (b + a) * (Dur - M) = (b + a) * Dur - (b + a) * M := Int.mul_sub ..
  have e2 : (b - a) * Dur = b * Dur - a * Dur := Int.sub_mul ..
  have e3 : (b + a) * Dur = b * Dur + a * Dur := Int.add_mul ..
  have e4 : 2 * a * Dur = 2 * (a * Dur) := Int.mul_assoc ..
  omega

/-- Witness clocks: holder runs at rate `1 - ρ`, grantor at `1 + ρ`. -/
def slowR (a b : Int) (t : Int) : Int := (b - a) * t
def fastR (a b : Int) (t : Int) : Int := (b + a) * t

theorem slowR_rate (a b : Int) (ha : 0 ≤ a) : RateClock a b (slowR a b) := by
  intro t1 t2 h
  have := Int.mul_le_mul_of_nonneg_right (show b - a ≤ b + a by omega) (show 0 ≤ t2 - t1 by omega)
  simp only [slowR]; rw [← Int.mul_sub]; omega
theorem fastR_rate (a b : Int) (ha : 0 ≤ a) : RateClock a b (fastR a b) := by
  intro t1 t2 h
  have := Int.mul_le_mul_of_nonneg_right (show b - a ≤ b + a by omega) (show 0 ≤ t2 - t1 by omega)
  simp only [fastR]; rw [← Int.mul_sub]; omega

/-- General rate-form tightness, up to refining the real-time tick. If
`(b - a) * G < (b + a) * W` (with `0 ≤ G`, `0 ≤ a < b`), then after scaling every
duration by `k = (b - a) * (b + a)` (the same physical scenario measured with a `k` times
finer real tick) there is an overlap: zero delay, holder at rate `1 - ρ`, grantor at
`1 + ρ`, at real time `b * (b - a) * G`. The scaling is needed because with integer real
time the overlap window can be shorter than one tick. -/
theorem rate_overlap_witness (a b W G : Int) (ha : 0 ≤ a) (hab : a < b) (hG : 0 ≤ G)
    (hc : (b - a) * G < (b + a) * W) :
    RateClock a b (slowR a b) ∧ RateClock a b (fastR a b) ∧
    RValid b (slowR a b) 0 ((b - a) * (b + a) * W) (b * (b - a) * G) ∧
    ROver b (fastR a b) 0 ((b - a) * (b + a) * G) (b * (b - a) * G) := by
  have hb : 0 < b * (b - a) := Int.mul_pos (by omega) (by omega)
  have p := Int.mul_lt_mul_of_pos_left hc hb
  have hn : 0 ≤ b * (b - a) * G := Int.mul_nonneg (Int.le_of_lt hb) hG
  refine ⟨slowR_rate a b ha, fastR_rate a b ha, ⟨hn, ?_⟩, ⟨hn, ?_⟩⟩ <;> simp only [slowR, fastR, Int.mul_zero, Int.sub_zero]
  · -- (b-a) * (b(b-a)G) < b * ((b-a)(b+a)W)
    have l : (b - a) * (b * (b - a) * G) = b * (b - a) * ((b - a) * G) := by
      simp only [Int.mul_comm, Int.mul_left_comm]
    have r : b * ((b - a) * (b + a) * W) = b * (b - a) * ((b + a) * W) := by
      simp only [Int.mul_assoc]
    omega
  · -- b * ((b-a)(b+a)G) ≤ (b+a) * (b(b-a)G)
    have l : b * ((b - a) * (b + a) * G) = (b + a) * (b * (b - a) * G) := by
      simp only [Int.mul_comm, Int.mul_left_comm]
    omega

/-- **Tightness, rate form.** If `(b + a) * M < 2 * a * Dur` (margin below `2ρ·Dur/(1+ρ)`),
`0 ≤ Dur`, there is an overlap (after the tick refinement of `rate_overlap_witness`). -/
theorem rate_asym_tight (a b Dur M : Int) (ha : 0 ≤ a) (hab : a < b) (hDur : 0 ≤ Dur)
    (hM : (b + a) * M < 2 * a * Dur) :
    let k := (b - a) * (b + a)
    RateClock a b (slowR a b) ∧ RateClock a b (fastR a b) ∧
    RValid b (slowR a b) 0 (k * (Dur - M)) (b * (b - a) * Dur) ∧
    ROver b (fastR a b) 0 (k * Dur) (b * (b - a) * Dur) := by
  apply rate_overlap_witness a b (Dur - M) Dur ha hab hDur
  have e1 : (b + a) * (Dur - M) = (b + a) * Dur - (b + a) * M := Int.mul_sub ..
  have e2 : (b - a) * Dur = b * Dur - a * Dur := Int.sub_mul ..
  have e3 : (b + a) * Dur = b * Dur + a * Dur := Int.add_mul ..
  have e4 : 2 * a * Dur = 2 * (a * Dur) := Int.mul_assoc ..
  omega

/-- **F11, rate form.** With both sides subtracting the margin (`G = W = Dur - M`), any
drift rate `a > 0` gives an overlap whenever `M < Dur`, whatever the margin. -/
theorem rate_literal_unsafe (a b Dur M : Int) (ha : 0 < a) (hab : a < b) (hDur : M < Dur) :
    let k := (b - a) * (b + a)
    RateClock a b (slowR a b) ∧ RateClock a b (fastR a b) ∧
    RValid b (slowR a b) 0 (k * (Dur - M)) (b * (b - a) * (Dur - M)) ∧
    ROver b (fastR a b) 0 (k * (Dur - M)) (b * (b - a) * (Dur - M)) := by
  apply rate_overlap_witness a b (Dur - M) (Dur - M) (by omega) hab (by omega)
  have : 0 < a * (Dur - M) := Int.mul_pos ha (by omega)
  have e2 : (b - a) * (Dur - M) = b * (Dur - M) - a * (Dur - M) := Int.sub_mul ..
  have e3 : (b + a) * (Dur - M) = b * (Dur - M) + a * (Dur - M) := Int.add_mul ..
  omega

/-- With `a = 0` (no drift) the literal reading is safe, so for the rate form too the
literal reading is safe exactly when there is no drift. -/
theorem rate_literal_safe_nodrift (b : Int) (cH cG : Int → Int) (s r Dur M t : Int)
    (hb : 0 < b) (hsr : s ≤ r) (hH : RateClock 0 b cH) (hG : RateClock 0 b cG) :
    ¬ (RValid b cH s (Dur - M) t ∧ ROver b cG r (Dur - M) t) :=
  rate_no_overlap 0 b cH cG s r (Dur - M) (Dur - M) t (Int.le_refl 0) hb hsr hH hG
    (by simp)

end Lease
