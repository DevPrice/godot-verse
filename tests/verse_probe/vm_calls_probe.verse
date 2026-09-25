using { /Godot.org/Godot }

# T1.4 (docs/web-vm/spec/calls.md): what a call does, run rather than read. Every public method is
# zero-argument and is called by the probe driver in declaration order; each prints what it saw.
#
#   bin/verse_probe.exe <engine>/Engine/Binaries/Win64/verse_host.dll <engine>/Engine \
#       tests/verse_probe/vm_calls_probe.verse --class vm_calls_probe
#
# What it answered (2026-09-24, engine 203d764): every case compiles and runs. Two arguments pack
# into a tuple parameter and a tuple unpacks into two parameters; positional arguments evaluate left
# to right, named ones in the order written at the call site, and an unsupplied default is
# evaluated inside the callee after all of them; `(super:)` skips an ancestor that does not
# override; a local function captures an enclosing local; and a two-million-deep recursion
# completes with no depth limit and no diagnostic.

Side(Label:string, Value:int)<transacts>:int =
	Print("  eval {Label}")
	Value

TakesTuple(X:tuple(int, int))<transacts>:int = X(0) * 10 + X(1)
TakesTwo(A:int, B:int)<transacts>:int = A * 10 + B
Three(A:int, B:int, C:int)<transacts>:int = A * 100 + B * 10 + C

Defaults(A:int, ?B:int = 10)<transacts>:int = A * 100 + B
DefaultFromEarlier(A:int, ?B:int = A * 2)<transacts>:int = A * 100 + B
TwoNamed(?X:int = 1, ?Y:int = 2)<transacts>:int = X * 10 + Y
DefaultEffects(A:int, ?B:int = Side("default B", 7), ?C:int = Side("default C", 8))<transacts>:int = A * 100 + B * 10 + C

Nested(A:int, (X:int, Y:int))<transacts>:int = A * 100 + X * 10 + Y
NestedNamed(A:int, (B:int, ?C:int = 1))<transacts>:int = A * 100 + B * 10 + C

Apply(F:type{_(:int)<transacts>:int}, X:int)<transacts>:int = F(X)
Double(X:int)<transacts>:int = X * 2

Depth(N:int)<transacts>:int = if (N = 0) then 0 else 1 + Depth(N - 1)

(X:int).Twice()<transacts>:int = X * 2

calls_base := class:
	Name<public>:string = "base"
	Describe<public>()<transacts>:string = "base.Describe on {Name}"

calls_derived := class(calls_base):
	Describe<override>()<transacts>:string = "derived.Describe then {(super:)Describe()}"

calls_mid := class(calls_derived):
	Other<public>()<transacts>:string = "mid"

calls_leaf := class(calls_mid):
	Describe<override>()<transacts>:string = "leaf then {(super:)Describe()}"

vm_calls_probe := class(object):

	SuperSkipsALevel<public>()<transacts>:void =
		L := calls_leaf{Name := "l"}
		Print(L.Describe())

	LocalFunction<public>()<transacts>:void =
		Base := 40
		AddBase(X:int)<transacts>:int = X + Base
		Print("AddBase(2) = {AddBase(2)}")
		Print("Apply(AddBase, 3) = {Apply(AddBase, 3)}")

	TupleBoxing<public>()<transacts>:void =
		Print("TakesTuple(3, 5) = {TakesTuple(3, 5)}")
		T := (4, 6)
		Print("TakesTuple(T) = {TakesTuple(T)}")
		Print("TakesTwo(1, 2) = {TakesTwo(1, 2)}")
		U := (7, 8)
		Print("TakesTwo(U) = {TakesTwo(U)}")

	NamedAndDefaults<public>()<transacts>:void =
		Print("Defaults(1) = {Defaults(1)}")
		Print("Defaults(1, ?B := 2) = {Defaults(1, ?B := 2)}")
		Print("DefaultFromEarlier(3) = {DefaultFromEarlier(3)}")
		Print("TwoNamed() = {TwoNamed()}")
		Print("TwoNamed(?Y := 5, ?X := 6) = {TwoNamed(?Y := 5, ?X := 6)}")

	EvaluationOrder<public>()<transacts>:void =
		Print("positional:")
		R1 := Three(Side("a", 1), Side("b", 2), Side("c", 3))
		Print("  = {R1}")
		Print("named, one supplied:")
		R2 := DefaultEffects(Side("a", 1), ?C := Side("argument C", 3))
		Print("  = {R2}")
		Print("named, written out of order:")
		R3 := TwoNamed(?Y := Side("argument Y", 5), ?X := Side("argument X", 6))
		Print("  = {R3}")

	NestedParameters<public>()<transacts>:void =
		Print("Nested(1, (2, 3)) = {Nested(1, (2, 3))}")
		P := (4, 5)
		Print("Nested(1, P) = {Nested(1, P)}")
		Print("NestedNamed(1, 2) = {NestedNamed(1, 2)}")
		Print("NestedNamed(1, (4, ?C := 5)) = {NestedNamed(1, (4, ?C := 5))}")

	IndexAndLookup<public>()<transacts>:void =
		A := array{10, 20, 30}
		if (X := A[1]) then Print("A[1] = {X}") else Print("A[1] fails")
		if (X := A[3]) then Print("A[3] = {X}") else Print("A[3] fails")
		if (X := A[-1]) then Print("A[-1] = {X}") else Print("A[-1] fails")
		M := map{"one" => 1, "two" => 2}
		if (X := M["two"]) then Print("M[two] = {X}") else Print("M[two] fails")
		if (X := M["three"]) then Print("M[three] = {X}") else Print("M[three] fails")

	FunctionValues<public>()<transacts>:void =
		Print("Apply(Double, 21) = {Apply(Double, 21)}")
		Print("5.Twice() = {5.Twice()}")

	MethodValues<public>()<transacts>:void =
		D := calls_derived{Name := "d"}
		Print(D.Describe())
		B:calls_base = D
		Print(B.Describe())
		F := D.Describe
		Print("through a value: {F()}")

	RecursionShallow<public>()<transacts>:void =
		Print("Depth(1000) = {Depth(1000)}")

	RecursionDeep<public>()<transacts>:void =
		Print("Depth(100000) = {Depth(100000)}")

	RecursionVeryDeep<public>()<transacts>:void =
		Print("Depth(2000000) = {Depth(2000000)}")
