using { /Godot.org/Godot }
using { /Verse.org/Concurrency }
using { /Verse.org/Random }

# T1.8 (docs/web-vm/spec/natives.md): the Verse-library natives a Godot script reaches, run rather
# than read. Every public zero-argument non-suspending method is called by the driver in
# declaration order with a tick between calls, so a method that raises loses only its own Prints.
#
#   bin/verse_probe.exe <engine>/Engine/Binaries/Win64/verse_host.dll <engine>/Engine \
#       <absolute path>/tests/verse_probe/vm_natives_probe.verse --class vm_natives_probe

vm_natives_probe := class(object):

	Ev:event(int) = event(int){}
	Sub:subscribable_event_intrnl(int) = subscribable_event_intrnl(int){}
	var Held:?task(int) = false
	var Order:[]int = array{}

	# --- float functions and their domain edges -----------------------------------------------------
	A01_Roots<public>()<transacts>:void =
		NegZero := 0.0 * -1.0
		Print("Sqrt(2.0) = {Sqrt(2.0)}")
		Print("Sqrt(-1.0) = {Sqrt(-1.0)}")
		Print("Sqrt(NegZero) = {Sqrt(NegZero)}, 1/Sqrt(NegZero) = {1.0 / Sqrt(NegZero)}")
		Print("Sqrt(Inf) = {Sqrt(1.0 / 0.0)}")
		Print("1.0 / NegZero = {1.0 / NegZero}")
		Print("ToString(NegZero) = {ToString(NegZero)}")
		Print("ArcTan(NegZero, -1.0) = {ArcTan(NegZero, -1.0)}")

	A02_Trig<public>()<transacts>:void =
		Print("Sin(Inf) = {Sin(Inf)}, Cos(Inf) = {Cos(Inf)}, Tan(Inf) = {Tan(Inf)}")
		Print("Sin(1.0) = {Sin(1.0)}, Cos(1.0) = {Cos(1.0)}, Tan(1.0) = {Tan(1.0)}")
		Print("ArcSin(2.0) = {ArcSin(2.0)}, ArcCos(2.0) = {ArcCos(2.0)}, ArcSin(-2.0) = {ArcSin(-2.0)}")
		Print("ArcTan(Inf) = {ArcTan(Inf)}")
		Print("ArcTan(0.0, 0.0) = {ArcTan(0.0, 0.0)}, ArcTan(0.0, -1.0) = {ArcTan(0.0, -1.0)}, ArcTan(1.0, 0.0) = {ArcTan(1.0, 0.0)}")
		Print("Sinh(1.0) = {Sinh(1.0)}, Cosh(1.0) = {Cosh(1.0)}, Tanh(1.0) = {Tanh(1.0)}")
		Print("ArSinh(Inf) = {ArSinh(Inf)}, ArCosh(0.5) = {ArCosh(0.5)}, ArTanh(1.0) = {ArTanh(1.0)}, ArTanh(2.0) = {ArTanh(2.0)}")

	A03_ExpLog<public>()<transacts>:void =
		Print("Exp(1.0) = {Exp(1.0)}, Exp(1000.0) = {Exp(1000.0)}, Exp(-1000.0) = {Exp(-1000.0)}")
		Print("Ln(0.0) = {Ln(0.0)}, Ln(-1.0) = {Ln(-1.0)}, Ln(0.0 * -1.0) = {Ln(0.0 * -1.0)}")
		Print("Log(10.0, 1000.0) = {Log(10.0, 1000.0)}")
		Print("Pow(0.0, 0.0) = {Pow(0.0, 0.0)}, Pow(-8.0, 1.0 / 3.0) = {Pow(-8.0, 1.0 / 3.0)}, Pow(2.0, 0.5) = {Pow(2.0, 0.5)}, Pow(-2.0, 3.0) = {Pow(-2.0, 3.0)}")
		Print("Lerp(1.0, 3.0, 0.5) = {Lerp(1.0, 3.0, 0.5)}, Lerp(0.0, 1.0, 2.0) = {Lerp(0.0, 1.0, 2.0)}")

	A04_IntNatives<public>()<transacts>:void =
		Print("Clamp(5, 10, 0) = {Clamp(5, 10, 0)}, Clamp(-5, 10, 0) = {Clamp(-5, 10, 0)}, Clamp(15, 10, 0) = {Clamp(15, 10, 0)}")
		Print("Clamp(1.5, 1.0, 0.0) = {Clamp(1.5, 1.0, 0.0)}")
		Print("Abs(-5) = {Abs(-5)}, Abs(-2.5) = {Abs(-2.5)}")
		Print("BitAnd(12, 10) = {BitAnd(12, 10)}, BitOr(12, 10) = {BitOr(12, 10)}, BitXor(-1, 5) = {BitXor(-1, 5)}, BitNot(0) = {BitNot(0)}")
		if (P := 7/2, N := -7/2):
			Print("Floor(7/2) = {Floor(P)}, Floor(-7/2) = {Floor(N)}, Ceil(-7/2) = {Ceil(N)}, Ceil(7/2) = {Ceil(P)}")
		Big := 9223372036854775807 + 1
		Print("Abs(-Big) > int64 max: {if (Abs(-Big) > 9223372036854775807) then "yes" else "no"}")

	A05_FloatToInt<public>()<transacts>:void =
		if (V := Int[-2.7]) then Print("Int[-2.7] = {V}") else Print("Int[-2.7] fails")
		if (V := Round[3.5]) then Print("Round[3.5] = {V}") else Print("Round[3.5] fails")
		if (V := Floor[1.0 / 0.0]) then Print("Floor[Inf] = {V}") else Print("Floor[Inf] fails")
		if (V := Round[0.0 / 0.0]) then Print("Round[NaN] fails? no: {V}") else Print("Round[NaN] fails")

	A06_RoundHuge<public>()<transacts>:void =
		if (V := Round[1.0e19]) then Print("Round[1e19] = {V}") else Print("Round[1e19] fails")

	A07_RoundHugeNeg<public>()<transacts>:void =
		if (V := Round[-1.0e19]) then Print("Round[-1e19] = {V}") else Print("Round[-1e19] fails")

	A08_FloorHuge<public>()<transacts>:void =
		if (V := Floor[1.0e19]) then Print("Floor[1e19] = {V}") else Print("Floor[1e19] fails")

	A09_QuotientOverflow<public>()<transacts>:void =
		Lowest := -9223372036854775807 - 1
		if (Q := Quotient[Lowest, -1]) then Print("Quotient[min, -1] = {Q}") else Print("Quotient[min, -1] fails")

	A10_ModBig<public>()<transacts>:void =
		Big := 9223372036854775807 + 1
		if (M := Mod[Big, 3]) then Print("Mod[2^63, 3] = {M}") else Print("Mod[2^63, 3] fails")

	# --- strings ----------------------------------------------------------------------------------
	B01_Strings<public>()<transacts>:void =
		Print("ToString('a') = [{ToString('a')}]")
		Print("ToString(0u00e9) = [{ToString(0u00e9)}], length {ToString(0u00e9).Length}")
		Print("ToString(0u1f600) length {ToString(0u1f600).Length}")
		Print("Join = [{Join(array{"a", "b", "c"}, ", ")}], empty = [{Join(array{}, "x")}], one = [{Join(array{"a"}, "-")}]")
		Print("ToString(0.0078125) = {ToString(0.0078125)}, ToString(0.0234375) = {ToString(0.0234375)}, ToString(2.5e-7) = {ToString(0.00000025)}")
		Print("ToString(-1.5) = {ToString(-1.5)}, ToString(5e-324) = {ToString(0.0000001 * 0.0)}")

	B02_Err<public>()<transacts>:void =
		Print("before Err")
		Err("boom")

	B03_Warn<public>()<transacts>:void =
		Warn("careful")
		Print("after Warn")

	B04_ErrEmpty<public>()<transacts>:void =
		Err("")

	# --- time and randomness ----------------------------------------------------------------------
	C01_Epoch<public>()<transacts>:void =
		T1 := GetSecondsSinceEpoch()
		T2 := GetSecondsSinceEpoch()
		Print("epoch 1 = {T1}, same within call: {if (T1 = T2) then "yes" else "no"}")

	C02_EpochAgain<public>()<transacts>:void =
		Print("epoch 2 = {GetSecondsSinceEpoch()}")

	C03_Random<public>()<transacts>:void =
		Print("GetRandomInt(5, 5) = {GetRandomInt(5, 5)}")
		R := GetRandomInt(3, 1)
		Print("GetRandomInt(3, 1) in [1,3]: {if (R >= 1, R <= 3) then "yes" else "no"}")
		Print("GetRandomFloat(2.0, 2.0) = {GetRandomFloat(2.0, 2.0)}")
		Print("GetRandomFloat(-Inf, Inf) = {GetRandomFloat(-1.0 / 0.0, 1.0 / 0.0)}")
		Print("GetRandomFloat(0.0, NaN) = {GetRandomFloat(0.0, 0.0 / 0.0)}")
		Print("GetRandomFloat(NaN, 1.0) = {GetRandomFloat(0.0 / 0.0, 1.0)}")

	C04_RandomRollback<public>()<transacts>:void =
		var Kept:int = 0
		if (X := GetRandomInt(1, 1000000), X < 0):
			set Kept = X
		Print("random under failure kept {Kept}")

	# --- maps -------------------------------------------------------------------------------------
	D01_Maps<public>()<transacts>:void =
		M := ConcatenateMaps(map{"0" => 0, "1" => 1}, map{"1" => 10, "0" => 11, "2" => 12})
		for (K -> V : M):
			Print("concat key {K} => {V}")

	# --- event(t) and subscribable_event_intrnl(t) --------------------------------------------------
	Waiter(Tag:int)<suspends>:void =
		X := Ev.Await()
		Print("waiter {Tag} got {X}")

	E01_StartWaiters<public>():void =
		spawn{Waiter(1)}
		spawn{Waiter(2)}
		spawn{Waiter(3)}
		Print("three waiters started")

	E02_Signal<public>():void =
		Ev.Signal(7)
		Print("signal returned")
		Ev.Signal(8)
		Print("second signal returned (no waiters)")

	OnA(X:int):void = set Order += array{1}
	OnB(X:int):void = set Order += array{2}
	OnC(X:int):void = set Order += array{3}
	OnD(X:int):void = set Order += array{4}
	OnE(X:int):void = set Order += array{5}

	E03_SubscribeOrder<public>():void =
		S1 := AsSub().Subscribe(OnA)
		S2 := AsSub().Subscribe(OnB)
		S3 := AsSub().Subscribe(OnC)
		S4 := AsSub().Subscribe(OnD)
		S5 := AsSub().Subscribe(OnE)
		Sub.Signal(1)
		Print("callback order 1: {Show(Order)}")
		set Order = array{}
		Sub.Signal(2)
		Print("callback order 2: {Show(Order)}")
		set Order = array{}
		S3.Cancel()
		Sub.Signal(3)
		Print("after cancelling C: {Show(Order)}")
		S3.Cancel()
		Print("second Cancel of the same subscription returned")

	AsSub()<transacts>:subscribable(int) = Sub

	Show(Values:[]int)<transacts>:string =
		var S:string = ""
		for (V : Values):
			set S += "{V} "
		S

	# --- task(t) ----------------------------------------------------------------------------------
	Worker()<suspends>:int =
		X := Ev.Await()
		X + 100

	F01_StartTask<public>():void =
		T := spawn{Worker()}
		set Held = option{T}
		Print("task started")

	F02_QueryTask<public>():void =
		if (T := Held?):
			Print("phases: Active {if (T.Active[]) then 1 else 0} Completed {if (T.Completed[]) then 1 else 0} Canceling {if (T.Canceling[]) then 1 else 0} Canceled {if (T.Canceled[]) then 1 else 0} Unsettled {if (T.Unsettled[]) then 1 else 0} Settled {if (T.Settled[]) then 1 else 0} Uninterrupted {if (T.Uninterrupted[]) then 1 else 0} Interrupted {if (T.Interrupted[]) then 1 else 0}")

	F03_Complete<public>():void =
		Ev.Signal(5)
		Print("signalled worker")

	F04_QueryAgain<public>():void =
		F02_QueryTask()

	AwaitHeld()<suspends>:void =
		if (T := Held?):
			R := T.Await()
			Print("Await on completed task answered {R}")

	F05_AwaitCompleted<public>():void =
		spawn{AwaitHeld()}

	Canceller()<suspends>:void =
		T := spawn{Worker()}
		T.Cancel()
		Print("after Cancel: Canceled {if (T.Canceled[]) then 1 else 0} Interrupted {if (T.Interrupted[]) then 1 else 0} Settled {if (T.Settled[]) then 1 else 0}")

	F06_Cancel<public>():void =
		spawn{Canceller()}

	# --- epic_internal reach ----------------------------------------------------------------------
	G01_EpicInternal<public>():void =
		Print("CanCallerAccessEpicInternal() = {if (CanCallerAccessEpicInternal()?) then "true" else "false"}")

	# --- localization -----------------------------------------------------------------------------
	Hello<localizes>:message = "Hello"
	Count<localizes>(N:int):message = "n={N}"
	Ratio<localizes>(F:float):message = "f={F}"
	Named<localizes>(S:string):message = "s={S}"

	H01_Localize<public>():void =
		Print("Localize(Hello) = [{Localize(Hello)}]")
		Print("Localize(Count(1234567)) = [{Localize(Count(1234567))}]")
		Print("Localize(Count(-5)) = [{Localize(Count(-5))}]")
		Print("Localize(Ratio(1234.5678)) = [{Localize(Ratio(1234.5678))}]")
		Print("Localize(Named(\"x\")) = [{Localize(Named("x"))}]")

	H02_JoinMessages<public>():void =
		M := Join(array{Hello, Named("y"), Count(3)}, Hello)
		Print("Localize(Join(...)) = [{Localize(M)}]")
		E := Join(array{}, Hello)
		Print("Localize(Join(empty)) = [{Localize(E)}]")

	# --- diagnostics ------------------------------------------------------------------------------
	I01_Diagnostic<public>()<transacts>:void =
		Print("ToDiagnostic(42) = [{ToDiagnostic(42).String}]")
		Print("ToDiagnostic(1.5) = [{ToDiagnostic(1.5).String}]")
		Print("ToDiagnostic(\"s\") = [{ToDiagnostic("s").String}]")
		Print("ToDiagnostic of an int array = [{ToDiagnostic(array{1, 2}).String}]")
		Print("ToDiagnostic(true) = [{ToDiagnostic(true).String}]")
		Print("ToDiagnostic of an option = [{ToDiagnostic(option{1}).String}]")

	# --- easing -----------------------------------------------------------------------------------
	J01_Easing<public>()<transacts>:void =
		Print("Linear(0.3) = {Easing.Linear(0.3)}, Ease(0.25) = {Easing.Ease(0.25)}, EaseIn(0.5) = {Easing.EaseIn(0.5)}, EaseOut(0.5) = {Easing.EaseOut(0.5)}, EaseInOut(0.25) = {Easing.EaseInOut(0.25)}")
		F := Easing.CubicBezier(0.42, 0.0, 0.58, 1.0)
		Print("CubicBezier(0.42,0,0.58,1)(0.25) = {F(0.25)}, out of range X gives identity: {Easing.CubicBezier(2.0, 0.0, 0.5, 1.0)(0.3)}")
