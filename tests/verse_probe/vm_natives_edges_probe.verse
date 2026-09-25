using { /Godot.org/Godot }
using { /Verse.org/Random }

# T1.8, second round: the edges vm_natives_probe.verse left open. Same driver, same rules.
#
#   bin/verse_probe.exe <engine>/Engine/Binaries/Win64/verse_host.dll <engine>/Engine \
#       <absolute path>/tests/verse_probe/vm_natives_edges_probe.verse --class vm_natives_edges_probe

Ratio<localizes>(F:float):message = "{F}"

vm_natives_edges_probe := class(object):

	# A negative zero made at run time, from an operand the compiler cannot see.
	NegZero()<transacts>:float =
		One := GetRandomFloat(1.0, 1.0)
		0.0 * -One

	Sign(X:float)<transacts>:string = if (ArcTan(X, -1.0) < 0.0) then "negative" else "positive"

	A01_NegativeZero<public>()<transacts>:void =
		Z := NegZero()
		Print("runtime -0.0 sign seen by ArcTan: {Sign(Z)}")
		Print("ToString(runtime -0.0) = {ToString(Z)}")
		Print("Sqrt(-0.0) sign: {Sign(Sqrt(Z))}, Tan(-0.0) sign: {Sign(Tan(Z))}, Sin(-0.0) sign: {Sign(Sin(Z))}, Abs(-0.0) sign: {Sign(Abs(Z))}")
		Print("ArcTan(-0.0, -0.0) = {ArcTan(Z, Z)}, ArcTan(-0.0, 0.0) = {ArcTan(Z, 0.0)}")
		Print("Lerp(0.0, Inf, 0.0) = {Lerp(0.0, Inf, 0.0)}, Lerp(1.0, 2.0, NaN) = {Lerp(1.0, 2.0, NaN)}")

	A02_RoundAt2p63<public>()<transacts>:void =
		if (V := Round[9223372036854775808.0]) then Print("Round[2^63] = {V}") else Print("Round[2^63] fails")
		if (V := Round[-9223372036854775808.0]) then Print("Round[-2^63] = {V}") else Print("Round[-2^63] fails")
		if (V := Round[9.2e18]) then Print("Round[9.2e18] = {V}") else Print("Round[9.2e18] fails")
		if (V := Int[-9223372036854775808.0]) then Print("Int[-2^63] = {V}") else Print("Int[-2^63] fails")

	RoundOne(X:float, Label:string)<transacts>:void =
		if (V := Round[X]) then Print("Round[{Label}] = {V}") else Print("Round[{Label}] fails")

	A02b_RoundBeyond<public>()<transacts>:void =
		RoundOne(9.3e18, "9.3e18")
		RoundOne(1.2e19, "1.2e19")
		RoundOne(1.8e19, "1.8e19")
		RoundOne(1.9e19, "1.9e19")
		RoundOne(3.0e19, "3e19")
		RoundOne(1.0e30, "1e30")
		RoundOne(-9.3e18, "-9.3e18")
		RoundOne(-1.2e19, "-1.2e19")
		RoundOne(-2.0e19, "-2e19")
		RoundOne(-1.0e30, "-1e30")

	A02c_NaNDomains<public>()<transacts>:void =
		Print("ArcSin(NaN) = {ArcSin(NaN)}, ArcCos(NaN) = {ArcCos(NaN)}, ArcTan(NaN, 0.0) = {ArcTan(NaN, 0.0)}, Sqrt(NaN) = {Sqrt(NaN)}")
		Print("ToString(-0.0000001) = {ToString(-0.0000001)}, ToString(-0.0000005) = {ToString(-0.0000005)}, ToString(0.0000005) = {ToString(0.0000005)}")

	A03_CeilAt2p63<public>()<transacts>:void =
		if (V := Ceil[9223372036854775808.0]) then Print("Ceil[2^63] = {V}") else Print("Ceil[2^63] fails")

	A04_BigBits<public>()<transacts>:void =
		Big := 9223372036854775807 + 1
		Print("BitAnd(-1, 2^63) = 2^63: {if (BitAnd(-1, Big) = Big) then "yes" else "no"}")
		Print("BitNot(2^63) < 0: {if (BitNot(Big) < 0) then "yes" else "no"}, BitNot(2^63) + 2^63 = -1: {if (BitNot(Big) + Big = -1) then "yes" else "no"}")
		Print("BitXor(2^63, 2^63) = 0: {if (BitXor(Big, Big) = 0) then "yes" else "no"}")
		if (Q := Big / 3):
			Print("Floor(2^63/3) > 0: {if (Floor(Q) > 0) then "yes" else "no"}")

	A05_CharStrings<public>()<transacts>:void =
		S := ToString(0o80)
		Print("ToString(0o80).Length = {S.Length}")
		Print("ToString(0u10ffff).Length = {ToString(0u10ffff).Length}")

	A06_LocalizeFloats<public>():void =
		Print("[{Localize(Ratio(0.5))}] [{Localize(Ratio(2.0))}] [{Localize(Ratio(-1234.5))}] [{Localize(Ratio(0.0001))}] [{Localize(Ratio(0.0005))}] [{Localize(Ratio(1.0e20))}] [{Localize(Ratio(NaN))}] [{Localize(Ratio(Inf))}]")

	A07_RandomSpread<public>()<transacts>:void =
		var Seen:[]int = array{0, 0, 0}
		for (I := 1..300):
			R := GetRandomInt(0, 2)
			if (Old := Seen[R], set Seen[R] = Old + 1) {}
		Print("GetRandomInt(0, 2) over 300 draws, every value seen: {if (A := Seen[0], B := Seen[1], C := Seen[2], A > 0, B > 0, C > 0) then "yes" else "no"}")
