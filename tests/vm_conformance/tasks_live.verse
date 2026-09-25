using { /Godot.org/Godot }
using { /Verse.org/Concurrency }

# spec/tasks.md §5.7, §10, §11 and spec/ops.md §3: `await` and `batch` over a var, an array element
# and a map value; `set live` over a var and an element; event(t)'s snapshot, nested Signal and a
# cancel during Signal; a subscription; and Sleep. "concurrent": tools/run_vm_conformance.py skips
# this file under --sequential. cooked_probe never calls vh_tick, so a Sleep of zero or more
# suspends and never wakes here: only what happens up to the suspension, and a cancel of it, is
# observable.

tasks_live := class(object):

	var Counter:int = 0
	var Src:int = 1
	var Dst:int = 0
	var Arr:[]int = array{0, 0}
	var Elems:[]int = array{0, 0}
	var Scores:[string]int = map{"a" => 0}
	var CTask:?task(int) = false
	var Sub:?cancelable = false

	EvK:event(int) = event(int){}
	EvN:event(int) = event(int){}
	EvC:event(int) = event(int){}
	EvRace:event(int) = event(int){}
	SubEv:subscribable_event_intrnl(int) = subscribable_event_intrnl(int){}

	Arm(Name:string, Ev:event(int))<suspends>:int =
		defer:
			Print("  {Name} defer")
		Print("  {Name} start")
		V := Ev.Await()
		Print("  {Name} end")
		V

	# --- await over a var, and batch ------------------------------------------------------------

	Awaiter()<suspends>:void =
		Print("  aw start, Counter = {Counter}")
		await{Counter > 2}
		Print("  aw resumed, Counter = {Counter}")

	F1_AwaitStart<public>():void =
		Print("F1 start")
		spawn{Awaiter()}
		Print("F1 end")

	F2_Set1<public>():void =
		Print("F2 set 1")
		set Counter = 1
		Print("F2 end")

	F3_Set3<public>():void =
		Print("F3 set 3")
		set Counter = 3
		Print("F3 end")

	Awaiter2()<suspends>:void =
		Print("  aw2 start")
		await{Counter > 10}
		Print("  aw2 resumed, Counter = {Counter}")

	F4_BatchStart<public>():void =
		Print("F4 start")
		spawn{Awaiter2()}
		Print("F4 end")

	F5_Batch<public>():void =
		Print("F5 batch")
		batch:
			set Counter = 11
			Print("  f5 inside batch after set")
			set Counter = 12
		Print("F5 end")

	Awaiter3()<suspends>:void =
		Print("  aw3 start, Counter = {Counter}")
		await{Counter > 0}
		Print("  aw3 resumed, Counter = {Counter}")

	F6_AwaitTrueAtStart<public>():void =
		Print("F6 start")
		spawn{Awaiter3()}
		Print("F6 end")

	F7_SetSame<public>():void =
		Print("F7 set to the same value")
		set Counter = 12
		Print("F7 end")

	# --- await over an array element and a map value --------------------------------------------

	ElemAwaiter()<suspends>:void =
		await{Elems[0] > 2}
		if (E := Elems[0]) then Print("  elem awaiter resumed, Elems[0] = {E}")

	A1_AwaitElement<public>():void =
		spawn{ElemAwaiter()}
		Print("A1 spawned")

	A2_WriteOtherElement<public>():void =
		if (set Elems[1] = 9) {}
		Print("A2 wrote Elems[1]")

	A3_WriteElement<public>():void =
		if (set Elems[0] = 3) {}
		Print("A3 wrote Elems[0]")

	MapAwaiter()<suspends>:void =
		await{Scores["a"] > 5}
		if (V := Scores["a"]) then Print("  map awaiter resumed, a = {V}")

	M1_AwaitMapValue<public>():void =
		spawn{MapAwaiter()}
		Print("M1 spawned")

	M2_InsertOtherKey<public>():void =
		if (set Scores["b"] = 7) {}
		Print("M2 inserted b")

	M3_WriteKey<public>():void =
		if (set Scores["a"] = 9) {}
		Print("M3 wrote a")

	# --- set live over a var and over an element ------------------------------------------------

	L1_Bind<public>():void =
		set live Dst = Src
		Print("L1 bound: Dst = {Dst}")

	L2_WriteSource<public>():void =
		set Src = 5
		Print("L2 Src = 5: Dst = {Dst}")

	L3_OverwriteBound<public>():void =
		set Dst = 100
		Print("L3 Dst set to 100: Dst = {Dst}")

	L4_WriteSourceAgain<public>():void =
		set Src = 7
		Print("L4 Src = 7: Dst = {Dst}")

	L5_BindElement<public>():void =
		if (set live Arr[1] = Src) {}
		if (E := Arr[1]) then Print("L5 Arr[1] = {E}")

	L6_WriteSourceForElement<public>():void =
		set Src = 9
		if (E := Arr[1]) then Print("L6 Src = 9: Arr[1] = {E}")

	L7_OverwriteBoundElement<public>():void =
		if (set Arr[1] = 100) {}
		if (E := Arr[1]) then Print("L7 Arr[1] set to 100: Arr[1] = {E}")

	L8_WriteSourceAgainForElement<public>():void =
		set Src = 11
		if (E := Arr[1]) then Print("L8 Src = 11: Arr[1] = {E}")

	# --- event(t): the snapshot, a nested Signal, a cancel during Signal ------------------------

	Reawaiter(Name:string)<suspends>:void =
		loop:
			V := EvK.Await()
			Print("  {Name} got {V}")
			if (V >= 3):
				break

	D1_Start<public>():void =
		Print("D1 start")
		spawn{Reawaiter("k1")}
		spawn{Reawaiter("k2")}
		Print("D1 end")

	D2_SignalOnce<public>():void =
		Print("D2 signal 1")
		EvK.Signal(1)
		Print("D2 signal 1 returned")

	D3_SignalAgain<public>():void =
		Print("D3 signal 3")
		EvK.Signal(3)
		Print("D3 signal 3 returned")

	D4_SignalNobody<public>():void =
		Print("D4 signal 4")
		EvK.Signal(4)
		Print("D4 signal 4 returned")

	Nester()<suspends>:void =
		V := EvN.Await()
		Print("  n1 got {V}, signalling again")
		EvN.Signal(V + 10)
		Print("  n1 nested signal returned")

	Plain(Name:string)<suspends>:void =
		V := EvN.Await()
		Print("  {Name} got {V}")

	D5_Nested<public>():void =
		Print("D5 start")
		spawn{Nester()}
		spawn{Plain("n2")}
		spawn{Plain("n3")}
		Print("D5 signal")
		EvN.Signal(1)
		Print("D5 signal returned")

	D6_Late<public>():void =
		Print("D6 signal")
		EvN.Signal(2)
		Print("D6 signal returned")

	CancelsC2()<suspends>:void =
		V := EvC.Await()
		Print("  c1 got {V}, cancelling c2")
		if (T := CTask?):
			T.Cancel()
		Print("  c1 cancel returned")

	D7_CancelDuringSignal<public>():void =
		Print("D7 start")
		spawn{CancelsC2()}
		T := spawn{Arm("c2", EvC)}
		set CTask = option{T}
		spawn{Arm("c3", EvC)}
		Print("D7 signal")
		EvC.Signal(9)
		Print("D7 signal returned")

	# --- a subscription: after the awaiters, cancelled, and one made in a failed transaction ---

	OnSub(X:int):void = Print("  sub got {X}")
	OnLostSub(X:int):void = Print("  lost sub got {X} (never)")

	SubWaiter()<suspends>:void =
		V := SubEv.Await()
		Print("  sub awaiter got {V}")

	U1_Subscribe<public>():void =
		L:listenable(int) = SubEv
		set Sub = option{L.Subscribe(OnSub)}
		if (L.Subscribe(OnLostSub), Counter < 0) {}
		spawn{SubWaiter()}
		Print("U1 subscribed")

	U2_Signal<public>():void =
		Print("U2 signal")
		SubEv.Signal(1)
		Print("U2 signal returned")

	U3_CancelTwice<public>():void =
		if (S := Sub?):
			S.Cancel()
			S.Cancel()
		Print("U3 cancelled")

	U4_SignalAfterCancel<public>():void =
		Print("U4 signal")
		SubEv.Signal(2)
		Print("U4 signal returned")

	# --- Sleep ----------------------------------------------------------------------------------

	SleepZero()<suspends>:void =
		defer:
			Print("  s0 defer")
		Print("  s0 before Sleep(0.0)")
		Sleep(0.0)
		Print("  s0 after Sleep(0.0) (never: nothing ticks)")

	S1_SleepZeroSuspends<public>():void =
		Print("S1 start")
		spawn{SleepZero()}
		Print("S1 end")

	SleepLong()<suspends>:int =
		defer:
			Print("  s2 defer")
		Print("  s2 sleeping")
		Sleep(1000.0)
		Print("  s2 woke (never)")
		0

	S2Driver()<suspends>:void =
		T := spawn{SleepLong()}
		Print("  S2 cancel")
		T.Cancel()
		Print("  S2 cancel returned")

	S2_CancelSleeper<public>():void =
		Print("S2 start")
		spawn{S2Driver()}
		Print("S2 end")

	S3Driver()<suspends>:void =
		X := race:
			block:
				defer:
					Print("  s3 sleep arm defer")
				Sleep(1000.0)
				1
			Arm("s3 event arm", EvRace)
		Print("  S3 race result {X}")

	S3_RaceSleepAgainstEvent<public>():void =
		Print("S3 start")
		spawn{S3Driver()}
		Print("S3 end")

	S4_SignalRace<public>():void =
		Print("S4 signal")
		EvRace.Signal(2)
		Print("S4 signal returned")

	S5_SleepNegativeInRace<public>():void =
		Racer()<suspends>:void =
			X := race:
				block:
					Sleep(-1.0)
					1
				Arm("s5 event arm", EvRace)
			Print("  S5 race result {X}")
		spawn{Racer()}
		Print("S5 end")
