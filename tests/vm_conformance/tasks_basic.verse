using { /Godot.org/Godot }
using { /Verse.org/Concurrency }

# spec/tasks.md: spawn, race, sync, rush, branch, defer, cancellation (structural, through an
# enclosing task's end -- see below), event(t). "concurrent": tools/run_vm_conformance.py skips
# this file under --sequential. State is coordinated with event(int) fields (synchronous Signal)
# rather than Sleep, so no vh_tick pump is needed between calls -- cooked_probe does not tick, and
# it does not need to for any of this.
#
# No method of a task(t) VALUE is called here (no .Await(), .Cancel(), no phase query): every one
# crashes verse_host_runtime.dll (SIGSEGV) at this engine build, though every one runs fine
# against verse_host.dll (the editor host) through tests/verse_probe. Confirmed down to the
# smallest repro: `T := spawn{Body()}` then `T.Active[]` alone, in the same call, already
# segfaults the runtime host. Reported to the lead; excluded here because a fixture that crashes
# the reference cannot be recorded at all, the same reason values.md/tasks.md's own crash cases
# are excluded. Cancellation is still exercised structurally instead: a `branch`'s body is a
# child of its enclosing task, so the enclosing task's own end cancels it with no method call on
# any task value (tasks.md §6.3, the E group below).

TasksArm(Name:string, Ev:event(int))<suspends>:int =
	defer:
		Print("  {Name} defer")
	Print("  {Name} start")
	V := Ev.Await()
	Print("  {Name} end")
	V

TasksImm(Name:string, V:int)<suspends>:int =
	defer:
		Print("  {Name} defer")
	Print("  {Name} runs to end")
	V

tasks_basic := class(object):

	EvA:event(int) = event(int){}
	EvB1:event(int) = event(int){}
	EvB2:event(int) = event(int){}
	EvC1:event(int) = event(int){}
	EvC2:event(int) = event(int){}
	EvC3:event(int) = event(int){}
	EvD1:event(int) = event(int){}
	EvD2:event(int) = event(int){}
	EvE1:event(int) = event(int){}
	EvF:event(int) = event(int){}
	EvG:event(int) = event(int){}

	# --- spawn: runs the body at once, up to its first suspension ---------------------------

	ABody()<suspends>:void =
		Print("  A body start")
		EvA.Await()
		Print("  A body resumed")

	A1_Spawn<public>():void =
		Print("A1 before spawn")
		spawn{ABody()}
		Print("A1 after spawn")

	A2_Signal<public>():void =
		Print("A2 before signal")
		EvA.Signal(1)
		Print("A2 after signal")

	# --- race: first-finished wins, later arms never start if the winner never suspended ----

	B1Driver()<suspends>:void =
		X := race:
			TasksImm("b1-arm1", 10)
			TasksArm("b1-arm2", EvB1)
		Print("  B1 race result {X}")

	B1_FirstArmImmediateWins<public>():void =
		Print("B1 start")
		spawn{B1Driver()}
		Print("B1 after spawn")

	B2Driver()<suspends>:void =
		X := race:
			TasksArm("b2-arm1", EvB2)
			TasksArm("b2-arm2", EvB1)
		Print("  B2 race result {X}")

	B2_Start<public>():void =
		Print("B2 start")
		spawn{B2Driver()}
		Print("B2 after spawn")

	B2_SignalArm1<public>():void =
		Print("B2 signal arm1")
		EvB2.Signal(20)
		Print("B2 signal returned")

	# --- sync: every arm starts and finishes; the value is the array of arm values ----------

	C1Driver()<suspends>:void =
		X := sync:
			TasksImm("c-arm1", 1)
			TasksArm("c-arm2", EvC1)
			TasksImm("c-arm3", 3)
		Print("  C1 sync result {X(0)} {X(1)} {X(2)}")

	C1_Start<public>():void =
		Print("C1 start")
		spawn{C1Driver()}
		Print("C1 after spawn")

	C1_Signal<public>():void =
		Print("C1 signal")
		EvC1.Signal(2)
		Print("C1 signal returned")

	# --- rush: the first to finish gives the value; the rest are children of the caller -----

	D1Driver()<suspends>:void =
		X := rush:
			TasksArm("d-arm1", EvD1)
			TasksArm("d-arm2", EvD2)
		Print("  D1 rush result {X}")
		EvE1.Await()
		Print("  D1 driver end")

	D1_Start<public>():void =
		Print("D1 start")
		spawn{D1Driver()}
		Print("D1 after spawn")

	D1_SignalArm1<public>():void =
		Print("D1 signal arm1")
		EvD1.Signal(41)
		Print("D1 signal returned")

	D1_SignalArm2<public>():void =
		Print("D1 signal arm2")
		EvD2.Signal(42)
		Print("D1 signal returned")

	D1_ReleaseDriver<public>():void =
		Print("D1 release driver")
		EvE1.Signal(0)
		Print("D1 release returned")

	# --- branch: the body is a child of the enclosing task; the enclosing task's end cancels it

	EDriver()<suspends>:void =
		defer:
			Print("  E driver defer")
		branch:
			EBranchBody()
		Print("  E after branch")
		EvF.Await()
		Print("  E driver end")

	EBranchBody()<suspends>:void =
		defer:
			Print("  e-branch defer")
		Print("  e-branch start")
		EvG.Await()
		Print("  e-branch end")

	E1_Start<public>():void =
		Print("E1 start")
		spawn{EDriver()}
		Print("E1 after spawn")

	E2_SignalDriver<public>():void =
		Print("E2 signal driver")
		EvF.Signal(0)
		Print("E2 signal returned")

	# --- Sleep(negative) does not suspend ----------------------------------------------------

	SleepNegativeDoesNotSuspend<public>():void =
		SleepBody()<suspends>:void =
			Print("  before Sleep(-1.0)")
			Sleep(-1.0)
			Print("  after Sleep(-1.0)")
		spawn{SleepBody()}
