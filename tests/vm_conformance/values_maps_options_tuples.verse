using { /Godot.org/Godot }

# spec/values.md §8-10: maps (order, lookup, key identity), options, logic, tuples.
# spec/ops.md §15.1 item 2: false, "", an empty array and an empty map are one map key.

values_maps_enum := enum:
	Red
	Green

values_maps_struct := struct:
	A:int = 0

values_maps_unique := class<unique>:
	Tag:int = 0

values_maps_options_tuples := class(object):

	MapLiteralOrder<public>():void =
		M := map{3 => "c", 1 => "a", 2 => "b"}
		Print("Length={M.Length}")
		for (K -> V : M):
			Print("order {K} => {V}")
		if (V := M[1]) then Print("M[1]={V}")
		if (V := M[9]) then Print("M[9] succeeds") else Print("M[9] fails")

	MapDuplicateKeyOrder<public>():void =
		D := map{1 => "x", 2 => "y", 1 => "z"}
		Print("dup literal Length={D.Length}")
		for (K -> V : D):
			Print("dup order {K} => {V}")

	MapSetInPlace<public>():void =
		var W:[int]string = map{1 => "a", 2 => "b"}
		if (set W[1] = "A") {}
		if (set W[3] = "c") {}
		for (K -> V : W):
			Print("after set order {K} => {V}")

	MapConcat<public>():void =
		C := ConcatenateMaps(map{1 => "a", 2 => "b"}, map{2 => "B", 3 => "c"})
		for (K -> V : C):
			Print("concat order {K} => {V}")

	MapEquality<public>():void =
		Print("same order eq: {if (map{1 => 1, 2 => 2} = map{1 => 1, 2 => 2}) then "yes" else "no"}")
		Print("other order eq: {if (map{1 => 1, 2 => 2} = map{2 => 2, 1 => 1}) then "yes" else "no"}")
		E1:[int]int = map{}
		E2:[int]int = map{}
		Print("empty=empty: {if (E1 = E2) then "yes" else "no"}")

	MapKeyIdentity<public>():void =
		Zero := 0.0
		F2 := map{Zero => "pos", -Zero => "neg"}
		Print("0.0/-0.0 map Length={F2.Length}")
		SM := map{"ab" => 1}
		if (V := SM[array{'a', 'b'}]) then Print("string key via char array={V}") else Print("string key via char array fails")
		Big := 9223372036854775807 + 1
		IM := map{Big => "big"}
		if (V := IM[9223372036854775807 + 1]) then Print("heap int key={V}") else Print("heap int key fails")
		TM := map{(1, 2) => "t"}
		if (V := TM[(1, 2)]) then Print("tuple key={V}") else Print("tuple key fails")
		EM := map{values_maps_enum.Red => 1, values_maps_enum.Green => 2}
		if (V := EM[values_maps_enum.Green]) then Print("enum key={V}")
		StM := map{values_maps_struct{A := 1} => "s"}
		if (V := StM[values_maps_struct{A := 1}]) then Print("struct key={V}") else Print("struct key fails")
		OM := map{option{1} => "o1", false => "none"}
		if (V := OM[option{1}]) then Print("option key={V}") else Print("option key fails")
		U := values_maps_unique{}
		UM := map{U => "u"}
		if (V := UM[U]) then Print("unique key same={V}") else Print("unique key same fails")
		if (V := UM[values_maps_unique{}]) then Print("unique key other succeeds") else Print("unique key other fails")

	# false vs "" as a map key is excluded here: ops.md §15.1 item 2 makes the interpreter treat them
	# as one key, a deliberate divergence from the reference's non-deterministic hashing (values.md
	# §16.1 Q2) -- exactly what a differential fixture must not depend on.

	Options<public>():void =
		O:?int = option{5}
		N:?int = false
		if (X := O?) then Print("O?={X}")
		if (X := N?) then Print("N? succeeds") else Print("N? fails")
		Print("false=N: {if (false = N) then "yes" else "no"}")
		Print("opt5=opt5: {if (O = option{5}) then "yes" else "no"}")
		Print("opt5=opt6: {if (O = option{6}) then "yes" else "no"}")
		OO:??int = option{option{5}}
		OE:??int = option{false}
		Print("opt-opt5<>opt-false: {if (OO <> OE) then "yes" else "no"}")
		if (Inner := OE?) then Print("opt-false? succeeds, inner?={if (Inner?) then "yes" else "no"}")
		OFail:?int = option{N?}
		Print("opt of failure = false: {if (OFail = false) then "yes" else "no"}")

	Logic<public>():void =
		T:logic = true
		F:logic = false
		Print("true?={if (T?) then "yes" else "no"}")
		Print("false?={if (F?) then "yes" else "no"}")
		Print("true=true: {if (T = true) then "yes" else "no"}")
		Print("true<>false: {if (T <> F) then "yes" else "no"}")
		OL:?logic = option{false}
		OT:?logic = option{true}
		Print("opt-false=opt-false: {if (OL = option{false}) then "yes" else "no"}")
		Print("opt-true<>opt-false: {if (OT <> OL) then "yes" else "no"}")
		if (V := OL?) then Print("opt-false? succeeds, value?={if (V?) then "yes" else "no"}")

	Tuples<public>():void =
		T := (1, 2.5, "s")
		Print("T(0)={T(0)} T(1)={T(1)} T(2)={T(2)}")
		Print("(1,2)=(1,2): {if ((1, 2) = (1, 2)) then "yes" else "no"}")
		Print("(1,2)=(1,3): {if ((1, 2) = (1, 3)) then "yes" else "no"}")
		P:tuple(int, int) = (3, 4)
		Q:[]int = P
		Print("tuple as array Length={Q.Length}")
		Print("tuple=same array: {if (P = array{3, 4}) then "yes" else "no"}")
