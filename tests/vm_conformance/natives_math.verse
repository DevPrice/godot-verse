using { /Godot.org/Godot }
using { /Verse.org/Random }

# spec/natives.md: the $BuiltIn intrinsics, the Verse-library math and string natives.
# Random draws are excluded except the deterministic edge cases (Low=High); GetSecondsSinceEpoch
# is tested only for same-process stability, never for its value, since both are non-reproducible
# across processes (natives.md §5.6, §6).

NativesHello<localizes>:message = "Hello"

natives_math := class(object):

	AbsIntFloat<public>():void =
		Print("Abs(-5)={Abs(-5)}")
		Print("Abs(-2.5)={Abs(-2.5)}")
		Lo := -9223372036854775807 - 1
		Print("Abs(min) gt max: {if (Abs(Lo) > 9223372036854775807) then "yes" else "no"}")

	BitOps<public>():void =
		Print("BitAnd(12,10)={BitAnd(12, 10)}")
		Print("BitOr(12,10)={BitOr(12, 10)}")
		Print("BitXor(-1,5)={BitXor(-1, 5)}")
		Print("BitNot(0)={BitNot(0)}")
		Big := 9223372036854775807 + 1
		Print("BitAnd(-1,2^63)=2^63: {if (BitAnd(-1, Big) = Big) then "yes" else "no"}")
		Print("BitXor(2^63,2^63)={BitXor(Big, Big)}")

	QuotientModEdges<public>():void =
		if (M := Mod[7, 2], Q := Quotient[7, 2]):
			Print("Mod[7,2]={M} Quotient[7,2]={Q}")
		if (M := Mod[-7, 2], Q := Quotient[-7, 2]):
			Print("Mod[-7,2]={M} Quotient[-7,2]={Q}")
		if (M := Mod[7, -2], Q := Quotient[7, -2]):
			Print("Mod[7,-2]={M} Quotient[7,-2]={Q}")
		if (M := Mod[-7, -2], Q := Quotient[-7, -2]):
			Print("Mod[-7,-2]={M} Quotient[-7,-2]={Q}")
		if (M := Mod[7, 0]) then Print("Mod[7,0]={M}") else Print("Mod[7,0] fails")
		if (Q := Quotient[7, 0]) then Print("Quotient[7,0]={Q}") else Print("Quotient[7,0] fails")

	QuotientOverflowRaises<public>():void =
		Lo := -9223372036854775807 - 1
		if (Q := Quotient[Lo, -1]) then Print("Quotient[min,-1]={Q}")

	ClampInt<public>():void =
		Print("Clamp(5,10,0)={Clamp(5, 10, 0)}")
		Print("Clamp(-5,10,0)={Clamp(-5, 10, 0)}")
		Print("Clamp(15,10,0)={Clamp(15, 10, 0)}")

	JoinStrings<public>():void =
		Print("Join(a,b,c)={Join(array{"a", "b", "c"}, ", ")}")
		Print("Join(empty)={Join(array{}, "x")}")
		Print("Join(one)={Join(array{"a"}, "-")}")

	Char32ToString<public>():void =
		Print("é={ToString(0u00e9)}")
		Print("emoji={ToString(0u1f600)}")

	SqrtAndTrig<public>():void =
		Print("Sqrt(-1)={Sqrt(-1.0)}")
		Print("Sqrt(Inf)={Sqrt(Inf)}")
		Print("Sin(Inf)={Sin(Inf)}")
		Print("Cos(Inf)={Cos(Inf)}")
		Print("Tan(Inf)={Tan(Inf)}")

	ArcTrig<public>():void =
		Print("ArcSin(2.0)={ArcSin(2.0)}")
		Print("ArcSin(-2.0)={ArcSin(-2.0)}")
		Print("ArcCos(2.0)={ArcCos(2.0)}")
		Print("ArcSin(NaN)={ArcSin(NaN)}")
		Print("ArcCos(NaN)={ArcCos(NaN)}")
		Print("ArcTan(0.0,0.0)={ArcTan(0.0, 0.0)}")
		Print("ArcTan(0.0,-1.0)={ArcTan(0.0, -1.0)}")
		Print("ArcTan(NaN,0.0)={ArcTan(NaN, 0.0)}")

	HyperbolicAndPow<public>():void =
		Print("ArSinh(Inf)={ArSinh(Inf)}")
		Print("ArTanh(1.0)={ArTanh(1.0)}")
		Print("ArCosh(0.5)={ArCosh(0.5)}")
		Print("ArTanh(2.0)={ArTanh(2.0)}")
		Print("Pow(0,0)={Pow(0.0, 0.0)}")
		Print("Pow(-2,3)={Pow(-2.0, 3.0)}")

	ExpLnLerp<public>():void =
		Print("Exp(1000)={Exp(1000.0)}")
		Print("Exp(-1000)={Exp(-1000.0)}")
		Print("Ln(0)={Ln(0.0)}")
		Print("Ln(-1)={Ln(-1.0)}")
		Print("Lerp(0,1,2)={Lerp(0.0, 1.0, 2.0)}")
		Print("Lerp(0,Inf,0)={Lerp(0.0, Inf, 0.0)}")
		Print("Lerp(1,2,NaN)={Lerp(1.0, 2.0, NaN)}")

	Diagnostics<public>():void =
		Print("diag(42)={ToDiagnostic(42).String}")
		Print("diag(1.5)={ToDiagnostic(1.5).String}")
		Print("diag(true)={ToDiagnostic(true).String}")
		Print("diag(opt1)={ToDiagnostic(option{1}).String}")

	LocalizeBasic<public>():void =
		Print("Localize(Hello)={Localize(NativesHello)}")

	CanAccessEpicInternal<public>():void =
		Print("CanCallerAccessEpicInternal={if (CanCallerAccessEpicInternal()?) then "yes" else "no"}")

	RandomEdgeCases<public>():void =
		Print("GetRandomInt(5,5)={GetRandomInt(5, 5)}")
		Print("GetRandomFloat(2.0,2.0)={GetRandomFloat(2.0, 2.0)}")
		Print("GetRandomFloat(-Inf,Inf)={GetRandomFloat(-Inf, Inf)}")
		Print("GetRandomFloat(0.0,NaN)={GetRandomFloat(0.0, NaN)}")
		Print("GetRandomFloat(NaN,1.0)={GetRandomFloat(NaN, 1.0)}")

	EpochIsStableWithinProcess<public>():void =
		A := GetSecondsSinceEpoch()
		B := GetSecondsSinceEpoch()
		Print("epoch stable: {if (A = B) then "yes" else "no"}")
