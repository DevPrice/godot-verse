using { /Godot.org/Godot }
using { /Verse.org/Concurrency }

# docs/web-vm/spec/tasks.md's second fixture: escapes out of sync and rush, a task spawned from a
# race arm, event(t)'s snapshot and cancellation during Signal, spawn in a failure context,
# `await` and `batch`, and the order subscribers are called in. Same driver contract as
# vm_tasks_probe.verse: declaration order, one tick between calls.
#
#   bin/verse_probe.exe <engine>/Engine/Binaries/Win64/verse_host.dll <engine>/Engine \
#       tests/verse_probe/vm_tasks_probe2.verse --class vm_tasks_probe2

vm_tasks_probe2 := class(object):

	EvS1:event(int) = event(int){}
	EvS2:event(int) = event(int){}
	EvR1:event(int) = event(int){}
	EvR2:event(int) = event(int){}
	EvX:event(int) = event(int){}
	EvXKid:event(int) = event(int){}
	EvK:event(int) = event(int){}
	EvN:event(int) = event(int){}
	EvC:event(int) = event(int){}
	EvL:event(int) = event(int){}

	var W2Task:?task(int) = false
	var Counter:int = 0
	var FailSpawned:int = 0

	Arm(Name:string, Ev:event(int))<suspends>:int =
		defer:
			Print("  {Name} defer")
		Print("  {Name} start")
		V := Ev.Await()
		Print("  {Name} end")
		V

	# --- return out of a sync arm ----------------------------------------------------------

	SyncReturnFn()<suspends>:int =
		defer:
			Print("  sr fn defer")
		sync:
			block:
				defer:
					Print("  sr-arm1 defer")
				Print("  sr-arm1 start")
				EvS1.Await()
				Print("  sr-arm1 returns 5")
				return 5
			Arm("sr-arm2", EvS2)
		Print("  sr after sync (never)")
		0

	SyncReturnDriver()<suspends>:void =
		R := SyncReturnFn()
		Print("  sr returned {R}")

	A1_SyncReturn():void =
		Print("A1 start")
		spawn{SyncReturnDriver()}
		Print("A1 after spawn")

	A2_Signal():void =
		Print("A2 signal arm1")
		EvS1.Signal(0)
		Print("A2 signal returned")

	# --- return out of a rush arm ----------------------------------------------------------

	RushReturnFn()<suspends>:int =
		defer:
			Print("  rr fn defer")
		rush:
			block:
				Print("  rr-arm1 start")
				EvR1.Await()
				Print("  rr-arm1 returns 6")
				return 6
			Arm("rr-arm2", EvR2)
		Print("  rr after rush (never)")
		0

	RushReturnDriver()<suspends>:void =
		R := RushReturnFn()
		Print("  rr returned {R}")
		EvL.Await()
		Print("  rr driver end")

	B1_RushReturn():void =
		Print("B1 start")
		spawn{RushReturnDriver()}
		Print("B1 after spawn")

	B2_Signal():void =
		Print("B2 signal arm1")
		EvR1.Signal(0)
		Print("B2 signal returned")

	B3_SignalLoser():void =
		Print("B3 signal arm2")
		EvR2.Signal(0)
		Print("B3 signal returned")

	# --- spawn inside a race arm survives the arm ------------------------------------------

	SpawnedKid()<suspends>:void =
		defer:
			Print("  x spawned kid defer")
		Print("  x spawned kid waiting")
		EvXKid.Await()
		Print("  x spawned kid end")

	XDriver()<suspends>:void =
		race:
			block:
				spawn{SpawnedKid()}
				EvX.Await()
				Print("  x arm1 end")
			Arm("x-arm2", EvX)
		Print("  x race done")

	C1_SpawnInArm():void =
		Print("C1 start")
		spawn{XDriver()}
		Print("C1 after spawn")

	C2_Signal():void =
		Print("C2 signal race")
		EvX.Signal(0)
		Print("C2 signal returned")

	C3_SignalKid():void =
		Print("C3 signal kid")
		EvXKid.Signal(0)
		Print("C3 signal returned")

	# --- event(t): awaits added during Signal wait for the next one -------------------------

	Reawaiter(Name:string)<suspends>:void =
		loop:
			V := EvK.Await()
			Print("  {Name} got {V}")
			if (V >= 3):
				break

	D1_Start():void =
		Print("D1 start")
		spawn{Reawaiter("k1")}
		spawn{Reawaiter("k2")}
		Print("D1 end")

	D2_SignalOnce():void =
		Print("D2 signal 1")
		EvK.Signal(1)
		Print("D2 signal 1 returned")

	# A nested Signal of the same event from inside an awaiter's resumption.
	Nester()<suspends>:void =
		V := EvN.Await()
		Print("  n1 got {V}, signalling again")
		EvN.Signal(V + 10)
		Print("  n1 nested signal returned")

	Plain(Name:string)<suspends>:void =
		V := EvN.Await()
		Print("  {Name} got {V}")

	D3_Nested():void =
		Print("D3 start")
		spawn{Nester()}
		spawn{Plain("n2")}
		spawn{Plain("n3")}
		Print("D3 signal")
		EvN.Signal(1)
		Print("D3 signal returned")

	D4_Late():void =
		Print("D4 signal")
		EvN.Signal(2)
		Print("D4 signal returned")

	# An awaiter cancelled by an earlier awaiter in the same Signal is not resumed.
	CancelsW2()<suspends>:void =
		V := EvC.Await()
		Print("  c1 got {V}, cancelling c2")
		if (T := W2Task?):
			T.Cancel()
		Print("  c1 cancel returned")

	D5_CancelDuringSignal():void =
		Print("D5 start")
		spawn{CancelsW2()}
		T := spawn{Arm("c2", EvC)}
		set W2Task = option{T}
		spawn{Plain("c3-never-on-EvN")}
		spawn{Arm("c3", EvC)}
		Print("D5 signal")
		EvC.Signal(9)
		Print("D5 signal returned")

	# --- spawn in a failure context --------------------------------------------------------

	NarrowBody()<suspends><transacts>:void =
		set FailSpawned += 1

	E1_SpawnInFailingIf():void =
		Print("E1 start")
		if (T := spawn{NarrowBody()}, FailSpawned > 100):
			Print("  e1 then (never)")
		else:
			Print("  e1 else, FailSpawned = {FailSpawned}")
		if (T := spawn{NarrowBody()}, FailSpawned >= 0):
			Print("  e1 succeeding if, FailSpawned = {FailSpawned}")
		Print("E1 end")

	# --- await and batch -------------------------------------------------------------------

	Awaiter()<suspends>:void =
		Print("  aw start, Counter = {Counter}")
		await{Counter > 2}
		Print("  aw resumed, Counter = {Counter}")

	F1_AwaitStart():void =
		Print("F1 start")
		spawn{Awaiter()}
		Print("F1 end")

	F2_Set1():void =
		Print("F2 set 1")
		set Counter = 1
		Print("F2 end")

	F3_Set3():void =
		Print("F3 set 3")
		set Counter = 3
		Print("F3 end")

	Awaiter2()<suspends>:void =
		Print("  aw2 start")
		await{Counter > 10}
		Print("  aw2 resumed, Counter = {Counter}")

	F4_BatchStart():void =
		Print("F4 start")
		spawn{Awaiter2()}
		Print("F4 end")

	F5_Batch():void =
		Print("F5 batch")
		batch:
			set Counter = 11
			Print("  f5 inside batch after set")
			set Counter = 12
		Print("F5 end")

	# An await whose condition already holds: does it still wait for a write?
	Awaiter3()<suspends>:void =
		Print("  aw3 start, Counter = {Counter}")
		await{Counter > 0}
		Print("  aw3 resumed, Counter = {Counter}")

	F6_AwaitTrueAtStart():void =
		Print("F6 start")
		spawn{Awaiter3()}
		Print("F6 end")

	F7_SetSame():void =
		Print("F7 set to the same value")
		set Counter = 12
		Print("F7 end")

	Z_Done():void =
		Print("Z done")
