using { /Godot.org/Godot }

# Workloads for tools/run_vm_bench.py, which cooks this file once and times each method on the
# interpreter and on the UE runtime host. Each loops enough that the entry itself is noise, except
# Empty, which is the entry. Every loop is `loop`/`break` rather than `for`, because a `for` is a
# comprehension and would time building an array of its body's results as well. A sum stays inside
# 32 bits unless the workload is about leaving them.

vm_bench_point := class:
	var X:int = 0

	Bump(D:int)<transacts>:void =
		set X += D

VmBenchAdd(A:int, B:int)<computes>:int = A + B

vm_bench := class(node2d):
	var Counter:int = 0

	Empty<public>():int = 0

	IntArith<public>():int =
		var Sum:int = 0
		var I:int = 0
		loop:
			if (I >= 100000):
				break
			set Sum += I - 50000
			set I += 1
		Sum

	Int64Arith<public>():int =
		var Sum:int = 5000000000
		var I:int = 0
		loop:
			if (I >= 100000):
				break
			set Sum += 3
			set I += 1
		Sum

	FloatArith<public>():float =
		var X:float = 0.0
		var I:int = 0
		loop:
			if (I >= 100000):
				break
			set X = X * 0.5 + 1.25
			set I += 1
		X

	FunctionCalls<public>():int =
		var Sum:int = 0
		var I:int = 0
		loop:
			if (I >= 100000):
				break
			set Sum = VmBenchAdd(Sum, 1)
			set I += 1
		Sum

	MethodCalls<public>():int =
		Point := vm_bench_point{}
		var I:int = 0
		loop:
			if (I >= 100000):
				break
			Point.Bump(1)
			set I += 1
		Point.X

	SelfFields<public>():int =
		var I:int = 0
		loop:
			if (I >= 100000):
				break
			set Counter += 1
			set I += 1
		Counter

	ObjectCreation<public>():int =
		var Sum:int = 0
		var I:int = 0
		loop:
			if (I >= 100000):
				break
			Point := vm_bench_point{X := I}
			set Sum += Point.X - I
			set I += 1
		Sum

	VectorMath<public>():float =
		var V:vector2 = vector2{X := 0.0, Y := 0.0}
		var I:int = 0
		loop:
			if (I >= 100000):
				break
			set V = V + vector2{X := 1.0, Y := 2.0}
			set I += 1
		V.X

	Bitwise<public>():int =
		var Sum:int = 0
		var I:int = 0
		loop:
			if (I >= 10000):
				break
			set Sum += BitAnd(I, 255)
			set I += 1
		Sum

	Strings<public>():int =
		var Total:int = 0
		var I:int = 0
		loop:
			if (I >= 10000):
				break
			Text := "item {I}"
			set Total += Text.Length
			set I += 1
		Total

	Arrays<public>():int =
		Items := for (I := 0..9999) do I * 2
		var Sum:int = 0
		for (Item : Items):
			set Sum += Item
		Sum

	Maps<public>():int =
		Table := map{1 => 10, 2 => 20, 3 => 30, 4 => 40}
		var Sum:int = 0
		var I:int = 0
		loop:
			if (I >= 10000):
				break
			if (Value := Table[Mod[I, 4] + 1]):
				set Sum += Value
			set I += 1
		Sum

	GodotReads<public>():float =
		var Sum:float = 0.0
		var I:int = 0
		loop:
			if (I >= 10000):
				break
			P := Position
			set Sum += P.X
			set I += 1
		Sum

	GodotWrites<public>():int =
		var I:int = 0
		loop:
			if (I >= 10000):
				break
			set Position = vector2{X := 1.0, Y := 2.0}
			set I += 1
		I
