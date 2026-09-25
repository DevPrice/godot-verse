using { /Godot.org/Godot }

# T1.3: the refusals behind docs/web-vm/spec/values.md. This file compiles NOTHING on purpose; the
# text of each diagnostic is the result. Each method holds one line the compiler is expected to
# refuse, so every refusal reports at its own line. Five of them were NOT refused -- RationalEqual,
# Char32Print, MapEqualsArray, ArrayFromFalse and IntLiteralMin -- and their runtime behaviour is
# measured in vm_values_probe.verse's Accepted* methods instead.

vm_values_reject_plain := class:
	Tag:int = 0

vm_values_reject := class(object):

	MixedAdd<public>()<transacts>:void =
		X := 1 + 1.0

	MixedSub<public>()<transacts>:void =
		X := 1.0 - 1

	MixedDiv<public>()<transacts>:void =
		X := 1.0 / 2

	CharOrder<public>()<transacts>:void =
		if ('a' < 'b') {}

	StringOrder<public>()<transacts>:void =
		if ("a" < "b") {}

	RationalEqual<public>()<transacts>:void =
		if (A := 7 / 2, B := 14 / 4, A = B) {}

	RationalOrder<public>()<transacts>:void =
		if (A := 7 / 2, B := 1 / 2, A > B) {}

	RationalPrint<public>()<transacts>:void =
		if (A := 7 / 2) then Print(ToString(A))

	RationalAdd<public>()<transacts>:void =
		if (A := 7 / 2) then X := A + 1

	Char32Print<public>()<transacts>:void =
		C:char32 = 'é'
		Print("{C}")

	NonUniqueEqual<public>()<transacts>:void =
		P := vm_values_reject_plain{}
		if (P = P) {}

	MapEqualsArray<public>()<transacts>:void =
		M:[int]int = map{}
		A:[]int = array{}
		if (M = A) {}

	ArrayFromFalse<public>()<transacts>:void =
		A:[]int = false

	FloatLiteralRange<public>()<transacts>:void =
		X := 1.0e400

	IntLiteralMin<public>()<transacts>:void =
		X := -9223372036854775808

	LogicToString<public>()<transacts>:void =
		Print("{true}")

	FloatMethodToString<public>()<transacts>:void =
		Print(1.5.ToString())
