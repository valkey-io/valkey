/-! List facts used by the Core proof (Lean core only). -/

namespace RestartProof

theorem take_append_le {l m : List Nat} {k : Nat} (h : k ≤ l.length) :
    (l ++ m).take k = l.take k := List.take_append_of_le_length h

theorem take_prefix_take {l : List Nat} {k j : Nat} (h : k ≤ j) :
    l.take k <+: l.take j := by
  have : l.take k = (l.take j).take k := by simp [List.take_take]; omega
  rw [this]; exact List.take_prefix _ _

/-- A prefix and its list agree on any shorter take. -/
theorem prefix_take_eq {a l : List Nat} (h : a <+: l) {k : Nat} (hk : k ≤ a.length) :
    a.take k = l.take k := by
  obtain ⟨t, rfl⟩ := h
  exact (take_append_le hk).symm

/-- Two prefixes of the same list agree below both lengths. -/
theorem prefixes_agree {a b l : List Nat} (ha : a <+: l) (hb : b <+: l) {k : Nat}
    (hka : k ≤ a.length) (hkb : k ≤ b.length) : a.take k = b.take k := by
  rw [prefix_take_eq ha hka, prefix_take_eq hb hkb]

theorem length_take_of_le {l : List Nat} {k : Nat} (h : k ≤ l.length) :
    (l.take k).length = k := by simp [List.length_take]; omega

theorem take_succ_getD {l : List Nat} {c : Nat} (h : c < l.length) :
    l.take (c + 1) = l.take c ++ [l.getD c 0] := by
  rw [List.take_add_one]
  simp [List.getElem?_eq_getElem h, List.getD_eq_getElem?_getD]

/-- One replication step: a strict prefix extended by the next entry is still a prefix. -/
theorem prefix_extend {a l : List Nat} (h : a <+: l) (hl : a.length < l.length) :
    a ++ [l.getD a.length 0] = l.take (a.length + 1) := by
  rw [take_succ_getD hl]
  have := prefix_take_eq h (Nat.le_refl a.length)
  simp [List.take_length] at this
  rw [← this]

theorem prefix_extend' {a l : List Nat} (h : a <+: l) (hl : a.length < l.length) :
    a ++ [l.getD a.length 0] <+: l := by
  rw [prefix_extend h hl]; exact List.take_prefix _ _

/-- `take k src` is a prefix of `dst`, src grows: unchanged when k ≤ |src|. -/
theorem take_append_prefix {src d : List Nat} (m : List Nat) {k : Nat} (hk : k ≤ src.length)
    (h : src.take k <+: d) : (src ++ m).take k <+: d := by
  rw [take_append_le hk]; exact h

theorem prefix_append_right {a d : List Nat} (m : List Nat) (h : a <+: d) : a <+: d ++ m :=
  h.trans (List.prefix_append _ _)

/-- Shrinking a take that is a prefix. -/
theorem take_mono_prefix {src d : List Nat} {k j : Nat} (hkj : k ≤ j) (h : src.take j <+: d) :
    src.take k <+: d := (take_prefix_take hkj).trans h

/-- Transfer: if `src.take a` is a prefix of `mid` and `mid.take b` of `dst` with `a ≤ b`,
and `a ≤ |src|`, then `src.take a` is a prefix of `dst`. -/
theorem take_transfer {src mid dst : List Nat} {a b : Nat} (hab : a ≤ b) (ha : a ≤ src.length)
    (h1 : src.take a <+: mid) (h2 : mid.take b <+: dst) : src.take a <+: dst := by
  have hlen : (src.take a).length = a := length_take_of_le ha
  have e : src.take a = mid.take a := by
    have := prefix_take_eq h1 (k := a) (by omega)
    rw [← this, List.take_take]; simp
  rw [e]; exact take_mono_prefix hab h2

end RestartProof
