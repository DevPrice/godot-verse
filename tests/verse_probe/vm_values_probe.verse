using { /Godot.org/Godot }

# T1.3 (docs/web-vm/spec/values.md): observable behaviour of every value kind, run rather than read.
# Every public zero-argument method is called by the probe driver in declaration order; a method
# that raises prints nothing at all (a raise rolls back its Prints), so each risky line has a method
# of its own. The spec cites each method by name beside the claim it settles.

vm_values_struct := struct:
	A:int = 0
	B:float = 0.0

vm_values_enum := enum:
	Red
	Green

vm_values_unique := class<unique>:
	Tag:int = 0

vm_values_probe := class(object):

	# --- ints --------------------------------------------------------------------------------------
	IntHeapRoundTrip<public>()<transacts>:void =
		Big := 9223372036854775807 + 1
		Back := Big - 1
		Print("int: (2^63-1)+1-1 = {Back}")
		Lo := -9223372036854775807 - 1
		Print("int: int64 min computed = {Lo}")
		Sq := 4294967296 * 4294967296
		Print("int: 2^64 > 2^63-1: {if (Sq > 9223372036854775807) then "succeeds" else "fails"}")
		Print("int: 2^64 / 2^32 back = {if (R := Sq / 4294967296) then Floor(R) else -1}")
		Print("int: -(int64 min) > int64 max: {if (-Lo > 9223372036854775807) then "succeeds" else "fails"}")
		Print("int: 2^64 = 2^64: {if (Sq = 4294967296 * 4294967296) then "succeeds" else "fails"}")
		Print("int: 2^64 <> 2^64+1: {if (Sq <> Sq + 1) then "succeeds" else "fails"}")

	IntNegMinToString<public>()<transacts>:void =
		Lo := -9223372036854775807 - 1
		Print("int: -(int64 min) printed = {-Lo}")

	IntBelowMinToString<public>()<transacts>:void =
		Lo := -9223372036854775807 - 1
		Print("int: int64 min - 1 printed = {Lo - 1}")

	IntModHeap<public>()<transacts>:void =
		Big := 9223372036854775807 + 1
		if (M := Mod[Big, 7]) then Print("int: Mod[2^63, 7] = {M}") else Print("int: Mod[2^63, 7] fails")

	IntModMinNegOne<public>()<transacts>:void =
		Lo := -9223372036854775807 - 1
		if (M := Mod[Lo, -1]) then Print("int: Mod[min, -1] = {M}") else Print("int: Mod[min, -1] fails")

	IntRational<public>()<transacts>:void =
		if (R := 7 / 2) then Print("rat: 7/2 floor {Floor(R)} ceil {Ceil(R)}")
		if (R := -7 / 2) then Print("rat: -7/2 floor {Floor(R)} ceil {Ceil(R)}")
		if (R := 7 / -2) then Print("rat: 7/-2 floor {Floor(R)} ceil {Ceil(R)}")
		if (R := -7 / -2) then Print("rat: -7/-2 floor {Floor(R)} ceil {Ceil(R)}")
		if (R := 6 / 3) then Print("rat: 6/3 floor {Floor(R)} ceil {Ceil(R)}")
		if (R := 0 / 5) then Print("rat: 0/5 floor {Floor(R)} ceil {Ceil(R)}")
		if (R := 7 / 0) then Print("rat: 7/0 succeeds") else Print("rat: 7/0 fails")
		if (R := 0 / 0) then Print("rat: 0/0 succeeds") else Print("rat: 0/0 fails")
		Big := 9223372036854775807 + 1
		if (R := Big / 3) then Print("rat: 2^63/3 floor {Floor(R)}")
		Print("rat: Floor(5) = {Floor(5)}, Ceil(-5) = {Ceil(-5)}")

	IntTimesFloat<public>()<transacts>:void =
		Print("mix: 3 * 1.5 = {3 * 1.5}")
		Print("mix: 1.5 * 3 = {1.5 * 3}")
		Print("mix: 9007199254740993 * 1.0 = {9007199254740993 * 1.0}")
		Big := 9223372036854775807 + 1
		Print("mix: 2^63 * 1.0 = {Big * 1.0}")
		Huge := Big * Big * Big * Big * Big * Big * Big * Big * Big * Big * Big * Big * Big * Big * Big * Big * Big
		Print("mix: 2^1071 * 1.0 = {Huge * 1.0}")
		Print("mix: 0 * -1.0 = {0 * -1.0}")

	# --- floats ------------------------------------------------------------------------------------
	FloatPrintA<public>()<transacts>:void =
		Print("f: 1/128 = {1.0 / 128.0}")
		Print("f: 3/128 = {3.0 / 128.0}")
		Print("f: 5/128 = {5.0 / 128.0}")
		Print("f: 0.5 = {0.5}")
		Print("f: 2.5 = {2.5}")
		Print("f: 0.0000005 = {0.0000005}")
		Print("f: 0.0000015 = {0.0000015}")
		Print("f: -0.0000001 = {-0.0000001}")
		Print("f: -0.0000004 = {-0.0000004}")
		Print("f: 0.1 + 0.2 = {0.1 + 0.2}")
		Print("f: 123.4567895 = {123.4567895}")
		Print("f: 999999.9999995 = {999999.9999995}")
		Print("f: 1e15+0.3 = {1.0e15 + 0.3}")

	FloatPrintB<public>()<transacts>:void =
		Print("f: 1e300 = {1.0e300}")
		Print("f: max = {1.7976931348623157e308}")
		Print("f: -max = {-1.7976931348623157e308}")
		Print("f: 2^-1074 = {4.9406564584124654e-324}")
		Print("f: 2^-1022 = {2.2250738585072014e-308}")
		Print("f: 1e22 = {1.0e22}")
		Print("f: 1e23 = {1.0e23}")
		Print("f: 2^53+1 literal = {9007199254740993.0}")

	FloatSpecials<public>()<transacts>:void =
		Zero := 0.0
		NegZero := -Zero
		Print("f: -Zero = {NegZero}")
		Print("f: 1/-Zero = {1.0 / NegZero}")
		Print("f: -1/-Zero = {-1.0 / NegZero}")
		Print("f: 1/-0.0 literal = {1.0 / -0.0}")
		Print("f: Inf - Inf = {Inf - Inf}")
		Print("f: -NaN = {-NaN}")
		Print("f: Inf * 0.0 = {Inf * 0.0}")
		Print("f: Inf = Inf: {if (Inf = Inf) then "succeeds" else "fails"}")
		Print("f: -Inf < -1e308: {if (-Inf < -1.0e308) then "succeeds" else "fails"}")
		Print("f: NaN > Inf: {if (NaN > Inf) then "succeeds" else "fails"}")
		Print("f: -Zero = Zero: {if (NegZero = Zero) then "succeeds" else "fails"}")
		Print("f: NegZero * -1.0 = {NegZero * -1.0}")
		Print("f: Abs(-Zero) = {Abs(NegZero)}, Abs(-2.5) = {Abs(-2.5)}")

	FloatToIntTooBig<public>()<transacts>:void =
		if (I := Floor[1.0e19]) then Print("f: Floor[1e19] = {I}") else Print("f: Floor[1e19] fails")

	FloatToIntEdges<public>()<transacts>:void =
		if (I := Floor[Inf]) then Print("f: Floor[Inf] = {I}") else Print("f: Floor[Inf] fails")
		if (I := Floor[NaN]) then Print("f: Floor[NaN] = {I}") else Print("f: Floor[NaN] fails")
		if (I := Floor[-9.2233720368547758e18]) then Print("f: Floor[-2^63] = {I}") else Print("f: Floor[-2^63] fails")
		if (I := Int[-2.7]) then Print("f: Int[-2.7] = {I}") else Print("f: Int[-2.7] fails")

	# --- chars and strings -------------------------------------------------------------------------
	Chars<public>()<transacts>:void =
		C:char = 'a'
		D:char = 'b'
		Print("c: ToString('a') = {ToString(C)}, interp = {C}")
		Print("c: 'a' = 'a': {if (C = 'a') then "succeeds" else "fails"}")
		Print("c: 'a' <> 'b': {if (C <> D) then "succeeds" else "fails"}")
		S := "héllo"
		Print("s: Length of h-e-acute-llo = {S.Length}")
		if (B := S[1]) then Print("s: S[1] = 'a'? {if (B = 'a') then "succeeds" else "fails"}")
		Print("s: \"ab\" + \"cd\" = {"ab" + "cd"}")
		Print("s: \"ab\" = \"ab\": {if ("ab" = "ab") then "succeeds" else "fails"}")
		Print("s: \"ab\" <> \"abc\": {if ("ab" <> "abc") then "succeeds" else "fails"}")
		Print("s: array of a,b = \"ab\": {if (array{'a', 'b'} = "ab") then "succeeds" else "fails"}")
		Built:string = array{'x', 'y'}
		Print("s: built from chars = {Built}")
		Print("s: \"\".Length = {"".Length}")
		if (X := "abc"[3]) then Print("s: \"abc\"[3] succeeds") else Print("s: \"abc\"[3] fails")
		Print("s: interp char literal = {'z'}!")
		Print("s: nested interp = {"in{1}ner"}")
		Print("s: empty interp = {}")

	# --- arrays ------------------------------------------------------------------------------------
	Arrays<public>()<transacts>:void =
		A := array{10, 20, 30}
		Print("a: Length = {A.Length}")
		if (X := A[0]) then Print("a: A[0] = {X}")
		if (X := A[3]) then Print("a: A[3] succeeds") else Print("a: A[3] fails")
		if (X := A[-1]) then Print("a: A[-1] succeeds") else Print("a: A[-1] fails")
		Big := 9223372036854775807 + 1
		if (X := A[Big]) then Print("a: A[2^63] succeeds") else Print("a: A[2^63] fails")
		if (X := A[4294967296]) then Print("a: A[2^32] succeeds") else Print("a: A[2^32] fails")
		B := A + array{40}
		Print("a: (A + array of 40).Length = {B.Length}")
		Print("a: array 1,2 = array 1,2: {if (array{1, 2} = array{1, 2}) then "succeeds" else "fails"}")
		Print("a: array 1,2 = array 2,1: {if (array{1, 2} = array{2, 1}) then "succeeds" else "fails"}")
		E:[]int = array{}
		F:[]int = array{}
		Print("a: empty = empty: {if (E = F) then "succeeds" else "fails"}")
		Mixed := array{1, 2.5, "s"}
		Print("a: mixed Length = {Mixed.Length}")
		Squares := for (I := 1..4) do I * I
		Print("a: for squares Length = {Squares.Length}")
		if (L := Squares[3]) then Print("a: Squares[3] = {L}")
		Filtered := for (I := 1..6, Mod[I, 2] = 0) do I
		Print("a: filtered Length = {Filtered.Length}")

	ArrayValueSemantics<public>()<transacts>:void =
		var A:[]int = array{1, 2, 3}
		B := A
		if (set A[0] = 9) {}
		Print("v: after set A[0], B[0] = {if (X := B[0]) then X else -1}, A[0] = {if (X := A[0]) then X else -1}")
		var M:[][]int = array{array{1, 2}, array{3}}
		N := M
		if (set M[0][1] = 7) {}
		Print("v: nested after set, N[0][1] = {if (X := N[0][1]) then X else -1}, M[0][1] = {if (X := M[0][1]) then X else -1}")
		set A += array{4, 5}
		Print("v: after +=, A.Length = {A.Length}, B.Length = {B.Length}")
		var C:[]int = A
		set C += array{6}
		Print("v: C.Length = {C.Length}, A.Length = {A.Length}")
		if (set A[10] = 1) then Print("v: set A[10] succeeds") else Print("v: set A[10] fails")

	# --- maps --------------------------------------------------------------------------------------
	Maps<public>()<transacts>:void =
		M := map{3 => "c", 1 => "a", 2 => "b"}
		Print("m: Length = {M.Length}")
		for (K -> V : M):
			Print("m: literal order {K} => {V}")
		if (V := M[1]) then Print("m: M[1] = {V}")
		if (V := M[9]) then Print("m: M[9] succeeds") else Print("m: M[9] fails")
		D := map{1 => "x", 2 => "y", 1 => "z"}
		Print("m: dup literal Length = {D.Length}")
		for (K -> V : D):
			Print("m: dup literal order {K} => {V}")
		var W:[int]string = map{1 => "a", 2 => "b"}
		if (set W[1] = "A") {}
		if (set W[3] = "c") {}
		for (K -> V : W):
			Print("m: after set order {K} => {V}")
		C := ConcatenateMaps(map{1 => "a", 2 => "b"}, map{2 => "B", 3 => "c"})
		for (K -> V : C):
			Print("m: concat order {K} => {V}")
		Print("m: same order equal: {if (map{1 => 1, 2 => 2} = map{1 => 1, 2 => 2}) then "succeeds" else "fails"}")
		Print("m: other order equal: {if (map{1 => 1, 2 => 2} = map{2 => 2, 1 => 1}) then "succeeds" else "fails"}")
		E1:[int]int = map{}
		E2:[int]int = map{}
		Print("m: empty = empty: {if (E1 = E2) then "succeeds" else "fails"}")

	MapKeys<public>()<transacts>:void =
		Zero := 0.0
		FM := map{Zero => "zero", NaN => "nan"}
		if (V := FM[-Zero]) then Print("k: FM[-0.0] = {V}") else Print("k: FM[-0.0] fails")
		if (V := FM[Inf - Inf]) then Print("k: FM[Inf-Inf] = {V}") else Print("k: FM[Inf-Inf] fails")
		F2 := map{Zero => "pos", -Zero => "neg"}
		Print("k: map of 0.0 and -0.0 Length = {F2.Length}")
		for (K -> V : F2):
			Print("k: float key {K} => {V}")
		SM := map{"ab" => 1}
		if (V := SM[array{'a', 'b'}]) then Print("k: string key via char array = {V}") else Print("k: string key via char array fails")
		Big := 9223372036854775807 + 1
		IM := map{Big => "big"}
		if (V := IM[9223372036854775807 + 1]) then Print("k: heap int key = {V}") else Print("k: heap int key fails")
		TM := map{(1, 2) => "t"}
		if (V := TM[(1, 2)]) then Print("k: tuple key = {V}") else Print("k: tuple key fails")
		EM := map{vm_values_enum.Red => 1, vm_values_enum.Green => 2}
		if (V := EM[vm_values_enum.Green]) then Print("k: enum key = {V}")
		StM := map{vm_values_struct{A := 1} => "s"}
		if (V := StM[vm_values_struct{A := 1}]) then Print("k: struct key = {V}") else Print("k: struct key fails")
		OM := map{option{1} => "o1", false => "none"}
		if (V := OM[option{1}]) then Print("k: option key = {V}") else Print("k: option key fails")
		U := vm_values_unique{}
		UM := map{U => "u"}
		if (V := UM[U]) then Print("k: unique key same = {V}") else Print("k: unique key same fails")
		if (V := UM[vm_values_unique{}]) then Print("k: unique key other succeeds") else Print("k: unique key other fails")

	# --- options, logic, tuples ----------------------------------------------------------------------
	Options<public>()<transacts>:void =
		O:?int = option{5}
		N:?int = false
		if (X := O?) then Print("o: O? = {X}")
		if (X := N?) then Print("o: N? succeeds") else Print("o: N? fails")
		Print("o: false = N: {if (false = N) then "succeeds" else "fails"}")
		Print("o: opt 5 = opt 5: {if (O = option{5}) then "succeeds" else "fails"}")
		Print("o: opt 5 = opt 6: {if (O = option{6}) then "succeeds" else "fails"}")
		Print("o: opt 5 = false: {if (O = N) then "succeeds" else "fails"}")
		OO:??int = option{option{5}}
		OE:??int = option{false}
		Print("o: opt opt 5 <> opt false: {if (OO <> OE) then "succeeds" else "fails"}")
		if (Inner := OE?) then Print("o: opt false? succeeds, inner? {if (Inner?) then "succeeds" else "fails"}")
		OF:?int = option{if (X := N?) then X else 7}
		if (X := OF?) then Print("o: computed option = {X}")
		OFail:?int = option{N?}
		Print("o: opt of failure = false: {if (OFail = false) then "succeeds" else "fails"}")

	Logic<public>()<transacts>:void =
		T:logic = true
		F:logic = false
		Print("l: true? {if (T?) then "succeeds" else "fails"}")
		Print("l: false? {if (F?) then "succeeds" else "fails"}")
		Print("l: true = true: {if (T = true) then "succeeds" else "fails"}")
		Print("l: true <> false: {if (T <> F) then "succeeds" else "fails"}")
		OL:?logic = option{false}
		OT:?logic = option{true}
		Print("l: opt false = opt false: {if (OL = option{false}) then "succeeds" else "fails"}")
		Print("l: opt true <> opt false: {if (OT <> OL) then "succeeds" else "fails"}")
		if (V := OL?) then Print("l: opt false? succeeds, value? {if (V?) then "true" else "false"}")

	Tuples<public>()<transacts>:void =
		T := (1, 2.5, "s")
		Print("t: T(0) = {T(0)}, T(1) = {T(1)}, T(2) = {T(2)}")
		Print("t: (1,2) = (1,2): {if ((1, 2) = (1, 2)) then "succeeds" else "fails"}")
		Print("t: (1,2) = (1,3): {if ((1, 2) = (1, 3)) then "succeeds" else "fails"}")
		P:tuple(int, int) = (3, 4)
		Q:[]int = P
		Print("t: tuple as array Length = {Q.Length}")
		Print("t: tuple = same array: {if (P = array{3, 4}) then "succeeds" else "fails"}")

	# --- structs, classes, enums ---------------------------------------------------------------------
	Identity<public>()<transacts>:void =
		S1 := vm_values_struct{A := 1, B := 2.0}
		S2 := vm_values_struct{A := 1, B := 2.0}
		S3 := vm_values_struct{A := 1, B := 3.0}
		Print("i: struct fieldwise equal: {if (S1 = S2) then "succeeds" else "fails"}")
		Print("i: struct fieldwise differ: {if (S1 = S3) then "succeeds" else "fails"}")
		SN1 := vm_values_struct{B := NaN}
		SN2 := vm_values_struct{B := Inf - Inf}
		Print("i: struct NaN fields equal: {if (SN1 = SN2) then "succeeds" else "fails"}")
		U1 := vm_values_unique{}
		U2 := vm_values_unique{}
		Print("i: unique same: {if (U1 = U1) then "succeeds" else "fails"}")
		Print("i: unique other: {if (U1 = U2) then "succeeds" else "fails"}")
		Print("i: unique <> other: {if (U1 <> U2) then "succeeds" else "fails"}")
		Print("i: enum same: {if (vm_values_enum.Red = vm_values_enum.Red) then "succeeds" else "fails"}")
		Print("i: enum other: {if (vm_values_enum.Red = vm_values_enum.Green) then "succeeds" else "fails"}")

	# --- lines vm_values_reject.verse expected refused and the compiler accepted ----------------------
	AcceptedRationalEquality<public>()<transacts>:void =
		Print("q: 7/2 = 14/4: {if (A := 7 / 2, B := 14 / 4, A = B) then "succeeds" else "fails"}")
		Print("q: 1/2 = 1/3: {if (A := 1 / 2, B := 1 / 3, A = B) then "succeeds" else "fails"}")
		Print("q: 6/3 = 2: {if (A := 6 / 3, A = 2) then "succeeds" else "fails"}")
		Print("q: 7/2 = 3: {if (A := 7 / 2, A = 3) then "succeeds" else "fails"}")
		Print("q: -1/-2 = 1/2: {if (A := -1 / -2, B := 1 / 2, A = B) then "succeeds" else "fails"}")

	AcceptedChar32<public>()<transacts>:void =
		C:char32 = 'é'
		D:char32 = 'é'
		Print("c32: interp = {C}, ToString = {ToString(C)}")
		Print("c32: equal: {if (C = D) then "succeeds" else "fails"}")
		S:string = "é"
		Print("c32: string of it Length = {S.Length}")

	AcceptedEmptyContainers<public>()<transacts>:void =
		M:[int]int = map{}
		A:[]int = array{}
		Print("e: empty map = empty array: {if (M = A) then "succeeds" else "fails"}")
		F:[]int = false
		Print("e: false as array Length = {F.Length}")
		G := F + array{1, 2}
		Print("e: false + array Length = {G.Length}")
		if (X := F[0]) then Print("e: false[0] succeeds") else Print("e: false[0] fails")
		Print("e: false-array = empty array: {if (F = A) then "succeeds" else "fails"}")
		for (X : F):
			Print("e: iterated a false array element")
		Print("e: done iterating false array")

	AcceptedIntLiteralMin<public>()<transacts>:void =
		X := -9223372036854775808
		Print("lit: -9223372036854775808 = {X}")

	MapConcatDuplicateOrder<public>()<transacts>:void =
		C := ConcatenateMaps(map{1 => "a", 2 => "b"}, map{1 => "A"})
		for (K -> V : C):
			Print("m: concat dup order {K} => {V}")
		var W:[int]string = map{1 => "a", 2 => "b"}
		set W = ConcatenateMaps(W, map{1 => "X"})
		for (K -> V : W):
			Print("m: var concat dup order {K} => {V}")

	CrossKind<public>()<transacts>:void =
		I:comparable = 1
		F:comparable = 1.0
		C:comparable = 'a'
		S:comparable = "a"
		E:comparable = ""
		L:comparable = false
		W:comparable = 'a'
		Print("x: 1 = 1.0: {if (I = F) then "succeeds" else "fails"}")
		Print("x: char a = string a: {if (C = S) then "succeeds" else "fails"}")
		Print("x: empty string = false: {if (E = L) then "succeeds" else "fails"}")
		Print("x: false = empty string: {if (L = E) then "succeeds" else "fails"}")
		Print("x: 1 <> 1.0: {if (I <> F) then "succeeds" else "fails"}")
		Print("x: char a = char a via comparable: {if (C = W) then "succeeds" else "fails"}")
		OF:comparable = option{false}
		T:comparable = true
		Print("x: opt false = true via comparable: {if (OF = T) then "succeeds" else "fails"}")
		Print("x: true = opt false via comparable: {if (T = OF) then "succeeds" else "fails"}")
		FS:string = false
		SM := map{"" => "empty-key"}
		if (V := SM[FS]) then Print("x: false-string finds empty-string key = {V}") else Print("x: false-string finds empty-string key fails")
		FM := map{FS => "false-key"}
		if (V := FM[""]) then Print("x: empty string finds false-string key = {V}") else Print("x: empty string finds false-string key fails")
		Print("x: false-string Length = {FS.Length}, interp = [{FS}]")
		Squares := for (N := 1..3) do N
		Print("x: loop-built = literal: {if (Squares = array{1, 2, 3}) then "succeeds" else "fails"}")
		# `[[]int]string` is refused: error 3502, "Use of '[]int' as a map key is not yet
		# implemented." A string key ([]char) and a tuple key are accepted (MapKeys, above).

	# --- what a comparison answers --------------------------------------------------------------------
	ComparisonResults<public>()<transacts>:void =
		if (V := (3 < 5)) then Print("r: (3 < 5) answers {V}")
		if (V := (5 > 3)) then Print("r: (5 > 3) answers {V}")
		if (V := (2.5 <= 2.5)) then Print("r: (2.5 <= 2.5) answers {V}")
		if (V := (4 <> 5)) then Print("r: (4 <> 5) answers {V}")
		if (V := (4 = 4)) then Print("r: (4 = 4) answers {V}")
		Big := 9223372036854775807 + 1
		Print("r: 2^63 > 2^63-1 > 0: {if (Big > 9223372036854775807 > 0) then "succeeds" else "fails"}")
		Print("r: 1 < 2 < 2: {if (1 < 2 < 2) then "succeeds" else "fails"}")
