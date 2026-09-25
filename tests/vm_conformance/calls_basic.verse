using { /Godot.org/Godot }

# spec/calls.md: argument adaptation, named args and defaults, closures, methods, (super:).

CallsTakesTuple(A:int, B:int):int = A * 10 + B
CallsTakesTwo(A:int, B:int):int = A * 10 + B
CallsNested(A:int, (X:int, Y:int)):int = A * 100 + X * 10 + Y
CallsTwoNamed(?Y:int = 0, ?X:int = 0):int = X * 10 + Y
CallsDefaultFromEarlier(A:int, ?B:int = A * 2):int = A * 100 + B
CallsSide(Label:string, Value:int):int =
	Print("  eval {Label}")
	Value
CallsTwice(X:int)<transacts>:int = X * 2

calls_base := class:
	Describe()<transacts>:string = "base"

calls_derived := class(calls_base):
	Describe<override>()<transacts>:string = "derived"

calls_mid := class(calls_derived):

calls_leaf := class(calls_mid):
	Describe<override>()<transacts>:string = "leaf+{(super:)Describe()}"

calls_basic := class(object):

	TupleBoxing<public>():void =
		Print("TakesTuple(3,5)={CallsTakesTuple(3, 5)}")
		U := (7, 8)
		Print("TakesTwo(U)={CallsTakesTwo(U)}")

	NestedParameters<public>():void =
		Print("Nested(1,(2,3))={CallsNested(1, (2, 3))}")
		P := (2, 3)
		Print("Nested(1,P)={CallsNested(1, P)}")

	NamedAndDefaults<public>():void =
		Print("TwoNamed(?Y:=5,?X:=6)={CallsTwoNamed(?Y := 5, ?X := 6)}")
		Print("TwoNamed(default)={CallsTwoNamed()}")

	DefaultFromEarlier<public>():void =
		Print("DefaultFromEarlier(3)={CallsDefaultFromEarlier(3)}")
		Print("DefaultFromEarlier(3,?B:=9)={CallsDefaultFromEarlier(3, ?B := 9)}")

	EvaluationOrder<public>():void =
		Print("start")
		X := CallsSide("a", 1) + CallsSide("b", 2) + CallsSide("c", 3)
		Print("sum={X}")

	MethodDispatch<public>():void =
		B:calls_base = calls_derived{}
		Print("dispatch through base-typed var={B.Describe()}")

	MethodValueKeepsReceiver<public>():void =
		D := calls_derived{}
		F := D.Describe
		Print("bound method value={F()}")

	SuperSkipsALevel<public>():void =
		L := calls_leaf{}
		Print("leaf.Describe()={L.Describe()}")

	ExtensionMethodCall<public>():void =
		Print("5.Twice()={CallsTwice(5)}")

	ClosureCapture<public>():void =
		Base := 10
		AddBase(X:int):int = X + Base
		Print("closure AddBase(5)={AddBase(5)}")

	RecursionModestDepth<public>():void =
		Print("recursion sum(1..1000)={CallsSumTo(1000)}")

CallsSumTo(N:int)<transacts>:int =
	if (N <= 0) then 0 else N + CallsSumTo(N - 1)
