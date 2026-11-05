import Mathlib.Data.List.Infix
import Mathlib.Data.Set.Card

/-! List-prefix facts used by the invariant proof, on top of the core and Mathlib
lemmas about `List.IsPrefix` (`<+:`) and `List.take`, and the link between a set's
cardinality and duplicate-free lists of its members (used to relate `Writable` to its
earlier list form). -/

namespace RestartProof

variable {α : Type}

/-- A set has at least `m` elements iff some duplicate-free list of length at least
`m` consists of its members. Holds for any type, finite or not. -/
theorem le_encard_iff_list (m : Nat) (S : Set α) :
    (m : ℕ∞) ≤ S.encard ↔ ∃ l : List α, l.Nodup ∧ m ≤ l.length ∧ ∀ r ∈ l, r ∈ S := by
  classical
  constructor
  · intro h
    obtain ⟨t, hts, htc⟩ := Set.exists_subset_encard_eq h
    have hfin : t.Finite := Set.finite_of_encard_eq_coe htc
    refine ⟨hfin.toFinset.toList, Finset.nodup_toList _, ?_, fun r hr => hts (by simpa using hr)⟩
    rw [Finset.length_toList]
    have := hfin.encard_eq_coe_toFinset_card
    rw [htc] at this
    exact_mod_cast this.le
  · rintro ⟨l, hn, hl, hr⟩
    calc (m : ℕ∞) ≤ (l.length : ℕ∞) := by exact_mod_cast hl
      _ = ((l.toFinset : Finset α) : Set α).encard := by
          rw [Set.encard_coe_eq_coe_finsetCard, List.toFinset_card_of_nodup hn]
      _ ≤ S.encard := Set.encard_le_encard (fun r hr' => hr r (by simpa using hr'))

/-- A prefix and its list agree on any shorter take. -/
theorem prefix_take_eq {a l : List α} (h : a <+: l) {k : Nat} (hk : k ≤ a.length) :
    a.take k = l.take k := by
  obtain ⟨t, rfl⟩ := h
  exact (List.take_append_of_le_length hk).symm

/-- Two prefixes of the same list agree below both lengths. -/
theorem prefixes_agree {a b l : List α} (ha : a <+: l) (hb : b <+: l) {k : Nat}
    (hka : k ≤ a.length) (hkb : k ≤ b.length) : a.take k = b.take k := by
  rw [prefix_take_eq ha hka, prefix_take_eq hb hkb]

theorem take_succ_getD {l : List Nat} {c : Nat} (h : c < l.length) :
    l.take (c + 1) = l.take c ++ [l.getD c 0] := by
  rw [List.take_add_one]
  simp [List.getElem?_eq_getElem h, List.getD_eq_getElem?_getD]

/-- Shrinking a take that is a prefix. -/
theorem take_mono_prefix {src d : List α} {k j : Nat} (hkj : k ≤ j) (h : src.take j <+: d) :
    src.take k <+: d := (List.take_prefix_take_left hkj).trans h

/-- Transfer: if `src.take a` is a prefix of `mid` and `mid.take b` of `dst` with `a ≤ b`,
and `a ≤ |src|`, then `src.take a` is a prefix of `dst`. -/
theorem take_transfer {src mid dst : List α} {a b : Nat} (hab : a ≤ b) (ha : a ≤ src.length)
    (h1 : src.take a <+: mid) (h2 : mid.take b <+: dst) : src.take a <+: dst := by
  have e : src.take a = mid.take a := by
    rw [← prefix_take_eq h1 (k := a) (by simp; omega), List.take_take]; simp
  rw [e]; exact take_mono_prefix hab h2

end RestartProof
