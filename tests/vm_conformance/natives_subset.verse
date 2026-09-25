using { /Godot.org/Godot }

# spec/natives.md §5.10: classifiable_subset and classifiable_subset_var, observed through the
# library's own type queries, since a set's Elements are internal to /Verse.org/Verse.

subset_item := class<castable>:
	Name:string

subset_special := class<castable>(subset_item):

natives_subset := class(object):

	Has(Label:string, Items:classifiable_subset(subset_item)):void =
		if (Items.Contains[subset_special]) then Print("  {Label}: has a special") else Print("  {Label}: has no special")

	S1_Make<public>():void =
		Plain := MakeClassifiableSubset(array{subset_item{Name := "a"}, subset_item{Name := "b"}})
		Has("plain", Plain)
		Mixed := MakeClassifiableSubset(array{subset_item{Name := "a"}, subset_special{Name := "b"}})
		Has("mixed", Mixed)
		if (Mixed.ContainsAll[array{subset_special, subset_item}]) then Print("  mixed: has all") else Print("  mixed: lacks one")
		if (Plain.ContainsAny[array{subset_special}]) then Print("  plain: has some") else Print("  plain: has none")

	S2_FilterByType<public>():void =
		Mixed := MakeClassifiableSubset(array{subset_item{Name := "a"}, subset_special{Name := "b"}})
		Special := Mixed.FilterByType(subset_special)
		if (Special.Contains[subset_special]) then Print("  filtered: has a special") else Print("  filtered: has no special")
		Plain := MakeClassifiableSubset(array{subset_item{Name := "a"}})
		Empty := Plain.FilterByType(subset_special)
		if (Empty.Contains[subset_item]) then Print("  filtered plain: not empty") else Print("  filtered plain: empty")

	S3_Concatenate<public>():void =
		Left := MakeClassifiableSubset(array{subset_item{Name := "a"}})
		Right := MakeClassifiableSubset(array{subset_special{Name := "b"}})
		Has("left", Left)
		Has("left + right", Left + Right)

	S4_Var<public>():void =
		Var := MakeClassifiableSubsetVar(array{subset_item{Name := "a"}})
		Has("var", Var.Read())
		Key := Var.Add(subset_special{Name := "b"})
		Has("after add", Var.Read())
		if (Var.Remove[Key]) then Print("  removed") else Print("  remove failed (never)")
		Has("after remove", Var.Read())
		if (Var.Remove[Key]) then Print("  removed twice (never)") else Print("  second remove fails")
		Var.Write(MakeClassifiableSubset(array{subset_special{Name := "w"}}))
		Has("after write", Var.Read())

	S5_FailedAddRollsBack<public>():void =
		Var := MakeClassifiableSubsetVar(array{subset_item{Name := "a"}})
		if (Key := Var.Add(subset_special{Name := "lost"}), Var.Contains[subset_item], 1 > 2) {}
		Has("after failed add", Var.Read())
