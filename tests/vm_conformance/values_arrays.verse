using { /Godot.org/Godot }

# spec/values.md §6: arrays, indexing, concatenation, construction, value semantics.

values_arrays := class(object):

	ArrayLengthAndIndex<public>():void =
		A := array{10, 20, 30}
		Print("Length={A.Length}")
		if (X := A[0]) then Print("A[0]={X}")
		if (X := A[3]) then Print("A[3] succeeds") else Print("A[3] fails")
		if (X := A[-1]) then Print("A[-1] succeeds") else Print("A[-1] fails")

	ArrayIndexHugeFails<public>():void =
		A := array{10, 20, 30}
		Big := 9223372036854775807 + 1
		if (X := A[Big]) then Print("A[2^63] succeeds") else Print("A[2^63] fails")
		if (X := A[4294967296]) then Print("A[2^32] succeeds") else Print("A[2^32] fails")

	ArrayConcat<public>():void =
		A := array{10, 20, 30}
		B := A + array{40}
		Print("(A+[40]).Length={B.Length}")
		Print("false + [1,2]: Length={(false + array{1, 2}).Length}")

	ArrayEquality<public>():void =
		Print("[1,2]=[1,2]: {if (array{1, 2} = array{1, 2}) then "yes" else "no"}")
		Print("[1,2]=[2,1]: {if (array{1, 2} = array{2, 1}) then "yes" else "no"}")
		E:[]int = array{}
		F:[]int = array{}
		Print("empty=empty: {if (E = F) then "yes" else "no"}")

	ArrayFor<public>():void =
		Squares := for (I := 1..4) do I * I
		Print("squares.Length={Squares.Length}")
		if (L := Squares[3]) then Print("Squares[3]={L}")
		Filtered := for (I := 1..6, Mod[I, 2] = 0) do I
		Print("filtered.Length={Filtered.Length}")

	ArrayAsFalse<public>():void =
		F:[]int = false
		Print("false-as-array Length={F.Length}")
		if (X := F[0]) then Print("F[0] succeeds") else Print("F[0] fails")

	ArrayValueSemantics<public>():void =
		var A:[]int = array{1, 2, 3}
		B := A
		if (set A[0] = 9) {}
		Print("after set A[0]: B[0]={if (X := B[0]) then X else -1} A[0]={if (X := A[0]) then X else -1}")
		set A += array{4, 5}
		Print("after +=: A.Length={A.Length} B.Length={B.Length}")
		var C:[]int = A
		set C += array{6}
		Print("C.Length={C.Length} A.Length={A.Length}")
		if (set A[10] = 1) then Print("set A[10] succeeds") else Print("set A[10] fails")

	ArrayNestedCopy<public>():void =
		var M:[][]int = array{array{1, 2}, array{3}}
		N := M
		if (set M[0][1] = 7) {}
		Print("N[0][1]={if (X := N[0][1]) then X else -1} M[0][1]={if (X := M[0][1]) then X else -1}")

	MixedElementArray<public>():void =
		Mixed := array{1, 2.5, "s"}
		Print("mixed.Length={Mixed.Length}")

	StringOrArrayFalse<public>():void =
		Print("false+array: 0-elem absorbed on left too")
		L := array{1, 2} + false
		Print("[1,2]+false Length={L.Length}")
