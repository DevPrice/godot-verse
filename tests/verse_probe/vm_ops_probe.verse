using { /Godot.org/Godot }

# T1.9 (docs/web-vm/spec/ops.md): the per-op behaviour no other spec file probed -- integer
# boundaries crossed and crossed back, the ordinary (full-context) comparison ops beside the
# fast-fail ones, mixed kinds through `comparable`, Length through a var, CallSet's index and key
# rules, profile blocks, and a float subrange cast. One method per group; the driver calls them in
# declaration order and a method that raises prints nothing, so no method here is meant to raise.
#
#   bin/verse_probe.exe <engine>/Engine/Binaries/Win64/verse_host.dll <engine>/Engine \
#       <repo>/tests/verse_probe/vm_ops_probe.verse --class vm_ops_probe
#
# A condition that contains `set` gets a full failure context (failure.md §10), so the comparison
# inside it compiles to the ordinary op (`Lt`, `Lte`, `Neq`, ...) rather than its FastFail form;
# the same comparison without the `set` is the FastFail op. P02 and P03 pair them.

ops_unit := type{_X:float where 0.0 <= _X, _X <= 1.0}

vm_ops_probe := class(object):

	var Touch:int = 0


	# --- integers across the 32- and 64-bit boundaries, and back ---------------------------------
	P01_IntBoundaries<public>()<transacts>:void =
		Hi:int = 9223372036854775807
		Lo:int = -9223372036854775807 - 1
		Print("P01 max+1-1 = max: {if (Hi + 1 - 1 = Hi) then "succeeds" else "fails"}")
		Print("P01 min-1+1 = {Lo - 1 + 1}")
		Print("P01 min*-1 = max+1: {if (Lo * -1 = Hi + 1) then "succeeds" else "fails"}")
		Print("P01 (-min)+min = {(Lo * -1) + Lo}")
		A := array{10, 20, 30}
		if (E := A[(Hi + 1) - Hi]) then Print("P01 A[(max+1)-max] = {E}") else Print("P01 A[(max+1)-max] fails")
		if (E := A[(Hi + 3) - (Hi + 1)]) then Print("P01 A[(max+3)-(max+1)] = {E}") else Print("P01 A[(max+3)-(max+1)] fails")
		M := map{1 => "one"}
		if (V := M[(Hi + 2) - (Hi + 1)]) then Print("P01 map key via heap = {V}") else Print("P01 map key via heap fails")
		Print("P01 2^31 = {2147483647 + 1}, -2^31-1 = {-2147483647 - 2}")
		if (E := A[4294967296 - 4294967295]) then Print("P01 A[2^32-(2^32-1)] = {E}") else Print("P01 A[2^32-(2^32-1)] fails")
		if (E := A[4294967296]) then Print("P01 A[2^32] = {E}") else Print("P01 A[2^32] fails")
		Print("P01 max*max > max: {if (Hi * Hi > Hi) then "succeeds" else "fails"}")
		if (R := (Hi * Hi) / Hi) then Print("P01 floor((max*max)/max) = {Floor(R)}")
		Print("P01 -(min) - 1 = max: {if (-Lo - 1 = Hi) then "succeeds" else "fails"}")
		Print("P01 min * 1.0 = {Lo * 1.0}")
		Print("P01 max * 1.0 = {Hi * 1.0}")
		Print("P01 0 * -1.5 = {0 * -1.5}")

	# --- the ordinary comparison ops, in a full failure context ----------------------------------
	P02_FullContext<public>()<transacts>:void =
		N:float = NaN
		R1 := if (set Touch = 1, N <= N) then "succeeds" else "fails"
		R2 := if (set Touch = 2, N >= N) then "succeeds" else "fails"
		R3 := if (set Touch = 3, N < N) then "succeeds" else "fails"
		R4 := if (set Touch = 4, N <= 1.0) then "succeeds" else "fails"
		R5 := if (set Touch = 5, 1.0 >= N) then "succeeds" else "fails"
		Print("P02 full NaN<=NaN {R1}, NaN>=NaN {R2}, NaN<NaN {R3}, NaN<=1.0 {R4}, 1.0>=NaN {R5}")
		if (set Touch = 6, V := (3 < 5)) then Print("P02 full (3 < 5) answers {V}")
		if (set Touch = 7, V := (4 <> 5)) then Print("P02 full (4 <> 5) answers {V}")
		if (set Touch = 8, V := (2.5 >= 2.5)) then Print("P02 full (2.5 >= 2.5) answers {V}")
		I:comparable = 1
		F:comparable = 1.0
		R6 := if (set Touch = 9, I <> F) then "succeeds" else "fails"
		R7 := if (set Touch = 10, I = F) then "succeeds" else "fails"
		R8 := if (set Touch = 11, array{1, 2} <> array{1, 2, 3}) then "succeeds" else "fails"
		R9 := if (set Touch = 12, array{1, 2} <> array{1, 2}) then "succeeds" else "fails"
		Print("P02 full 1<>1.0 {R6}, 1=1.0 {R7}, [1,2]<>[1,2,3] {R8}, [1,2]<>[1,2] {R9}")
		Big := 9223372036854775807 + 1
		R10 := if (set Touch = 13, Big > 9223372036854775807) then "succeeds" else "fails"
		R11 := if (set Touch = 14, 1 < 2 < 2) then "succeeds" else "fails"
		Print("P02 full 2^63 > 2^63-1 {R10}, 1<2<2 {R11}")
		if (set Touch = 15, Q := option{3}?) then Print("P02 full option-3 query = {Q}")
		R12 := if (set Touch = 16, false?) then "succeeds" else "fails"
		Print("P02 full false? {R12}; Touch = {Touch}")

	# --- the fast-fail forms of the same, for comparison -----------------------------------------
	P03_FastForms<public>()<transacts>:void =
		N:float = NaN
		R1 := if (N <= N) then "succeeds" else "fails"
		R2 := if (N >= N) then "succeeds" else "fails"
		R3 := if (N < N) then "succeeds" else "fails"
		R4 := if (N <= 1.0) then "succeeds" else "fails"
		Print("P03 fast NaN<=NaN {R1}, NaN>=NaN {R2}, NaN<NaN {R3}, NaN<=1.0 {R4}")
		C:comparable = 'a'
		D:comparable = 0u0061
		R5 := if (C = D) then "succeeds" else "fails"
		R6 := if (C <> D) then "succeeds" else "fails"
		Print("P03 char a = char32 a {R5}, <> {R6}")
		X:comparable = 2
		R7 := if (Q := 4 / 2, X = Q) then "succeeds" else "fails"
		R8 := if (Q := 4 / 2, Q = X) then "succeeds" else "fails"
		R9 := if (Q := 5 / 2, X = Q) then "succeeds" else "fails"
		Print("P03 2 = 4/2 {R7}, 4/2 = 2 {R8}, 2 = 5/2 {R9}")
		O1:comparable = option{1}
		O2:comparable = option{1.0}
		R10 := if (O1 = O2) then "succeeds" else "fails"
		Print("P03 option-1 = option-1.0 {R10}")

	# --- Length through a var (LengthWithEffects) --------------------------------------------------
	P04_Length<public>()<transacts>:void =
		var A:[]int = array{1, 2}
		set A += array{3}
		Print("P04 var array Length = {A.Length}")
		var S:string = "ab"
		set S += "cd"
		Print("P04 var string Length = {S.Length}")
		var M:[int]int = map{}
		if (set M[5] = 1) {}
		if (set M[6] = 2) {}
		if (set M[5] = 3) {}
		Print("P04 var map Length = {M.Length}")
		var F:[]int = false
		Print("P04 var false Length = {F.Length}")

	# --- CallSet: indices that fail, and float keys -----------------------------------------------
	P05_CallSet<public>()<transacts>:void =
		var A:[]int = array{1, 2, 3}
		R1 := if (set A[-1] = 9) then "succeeds" else "fails"
		R2 := if (set A[3] = 9) then "succeeds" else "fails"
		Big := 9223372036854775807 + 1
		R3 := if (set A[Big] = 9) then "succeeds" else "fails"
		R4 := if (set A[Big - Big + 2] = 9) then "succeeds" else "fails"
		Print("P05 set A[-1] {R1}, A[3] {R2}, A[2^63] {R3}, A[2^63-2^63+2] {R4}")
		if (E := A[2]) then Print("P05 A[2] = {E}")
		var M:[float]string = map{}
		Zero:float = 0.0
		if (set M[NaN] = "nan-a") {}
		if (set M[Inf - Inf] = "nan-b") {}
		if (set M[Zero] = "pos") {}
		if (set M[-Zero] = "neg") {}
		Print("P05 float map Length = {M.Length}")
		for (K -> V : M):
			Print("P05 entry {K} => {V}")

	# --- profile blocks ---------------------------------------------------------------------------
	ProfValue()<transacts>:int =
		profile("vm_ops_value"):
			40 + 2

	ProfReturn()<transacts>:int =
		profile("vm_ops_return"):
			if (1 < 2):
				return 5
			6

	P06_Profile<public>()<transacts>:void =
		Print("P06 profile value = {ProfValue()}")
		Print("P06 profile with return = {ProfReturn()}")
		var T:int = 0
		profile("vm_ops_loop"):
			for (I := 1..3):
				set T += I
		Print("P06 T = {T}")

	# --- a float subrange as a cast target (TypeCastFastFail on a float type) ---------------------
	P07_FloatCast<public>()<transacts>:void =
		Vals:[]float = array{0.5, 0.0, -0.0, 1.0, 2.0, -1.0, NaN, Inf}
		for (V : Vals):
			R := if (S := ops_unit[V]) then "ok {S}" else "fail"
			Print("P07 unit[{V}] = {R}")

	# --- Add on strings and false -----------------------------------------------------------------
	P08_Concat<public>()<transacts>:void =
		E:string = ""
		F:string = false
		Print("P08 \"\" + \"\" Length = {(E + E).Length}")
		Print("P08 false + \"x\" = {F + "x"}, \"x\" + false = {"x" + F}")
		Print("P08 false + false Length = {(F + F).Length}")
