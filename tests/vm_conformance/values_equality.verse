using { /Godot.org/Godot }

# spec/values.md §11: the equality rules, cross-kind cases (not map-key hashing, which is
# values_maps_options_tuples.verse's and excludes the deliberately-divergent false/"" key case).

values_equality_struct := struct:
	A:int = 0
	B:float = 0.0

values_equality := class(object):

	IntVsFloat<public>():void =
		Print("1=1.0: {if (1 = 1.0) then "yes" else "no"}")
		Print("1<>1.0: {if (1 <> 1.0) then "yes" else "no"}")

	IntVsRational<public>():void =
		if (R := 4 / 2):
			Print("4/2=2: {if (R = 2) then "yes" else "no"}")
		if (R := 3 / 2):
			Print("3/2=1: {if (R = 1) then "yes" else "no"}")

	EmptyContainersCrossKind<public>():void =
		Print("\"\"=[]: {if ("" = array{}) then "yes" else "no"}")
		Print("[]=map{}: {if (array{} = map{}) then "yes" else "no"}")
		Print("false=[]: {if (false = array{}) then "yes" else "no"}")
		Print("false=map{}: {if (false = map{}) then "yes" else "no"}")
		Print("false=\"\": {if (false = "") then "yes" else "no"}")

	StructFieldNaNEquality<public>():void =
		S1 := values_equality_struct{A := 1, B := NaN}
		S2 := values_equality_struct{A := 1, B := NaN}
		Print("struct with NaN field: eq={if (S1 = S2) then "yes" else "no"}")

	StructLayoutIndependence<public>():void =
		# Same struct, one field explicit, one default -- equality does not depend on how the
		# layout was reached (objects.md §4.3, §10.2).
		S1 := values_equality_struct{}
		S2 := values_equality_struct{A := 0, B := 0.0}
		Print("default vs explicit struct: eq={if (S1 = S2) then "yes" else "no"}")

	CharVsChar32<public>():void =
		C:char = 'a'
		C32 := 0u0061
		Print("'a' char vs char32 same codepoint: {if (C = C32) then "yes" else "no"}")
