using { /Godot.org/Godot }
using { /Verse.org/Concurrency }

# spec/tasks.md: spawn, race, sync, rush, branch, defer, cancellation (structurally, through an
# enclosing task's end, and directly, through Cancel), event(t), and the methods of a task(t)
# value -- the eight phase queries, Cancel and Await (the H group). "concurrent":
# tools/run_vm_conformance.py skips this file under --sequential. State is coordinated with
# event(int) fields (synchronous Signal) rather than Sleep, so no vh_tick pump is needed between
# calls -- cooked_probe does not tick, and it does not need to for any of this.

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

TasksPhases(Name:string, T:task(int)):void =
	if (T.Active[]) then Print("  {Name} Active") else Print("  {Name} not Active")
	if (T.Completed[]) then Print("  {Name} Completed") else Print("  {Name} not Completed")
	if (T.Canceling[]) then Print("  {Name} Canceling") else Print("  {Name} not Canceling")
	if (T.Canceled[]) then Print("  {Name} Canceled") else Print("  {Name} not Canceled")
	if (T.Unsettled[]) then Print("  {Name} Unsettled") else Print("  {Name} not Unsettled")
	if (T.Settled[]) then Print("  {Name} Settled") else Print("  {Name} not Settled")
	if (T.Uninterrupted[]) then Print("  {Name} Uninterrupted") else Print("  {Name} not Uninterrupted")
	if (T.Interrupted[]) then Print("  {Name} Interrupted") else Print("  {Name} not Interrupted")

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
	EvH1:event(int) = event(int){}
	EvH3:event(int) = event(int){}
	EvH5:event(int) = event(int){}

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

	# --- task(t) values: the phase queries, Cancel and Await --------------------------------

	H1_PhasesOfSuspended<public>():void =
		Print("H1 start")
		T := spawn{TasksArm("h1", EvH1)}
		TasksPhases("H1", T)
		Print("H1 end")

	H2_PhasesOfCompleted<public>():void =
		Print("H2 start")
		T := spawn{TasksImm("h2", 7)}
		TasksPhases("H2", T)
		Print("H2 end")

	H3Driver()<suspends>:void =
		T := spawn{TasksArm("h3", EvH3)}
		Print("  H3 cancel")
		T.Cancel()
		Print("  H3 cancel returned")
		TasksPhases("H3", T)

	H3_Cancel<public>():void =
		Print("H3 start")
		spawn{H3Driver()}
		Print("H3 after spawn")

	H4Driver()<suspends>:void =
		T := spawn{TasksImm("h4", 8)}
		V := T.Await()
		Print("  H4 awaited {V}")

	H4_AwaitCompleted<public>():void =
		Print("H4 start")
		spawn{H4Driver()}
		Print("H4 after spawn")

	H5Driver()<suspends>:void =
		T := spawn{TasksArm("h5", EvH5)}
		Print("  H5 awaiting")
		V := T.Await()
		Print("  H5 awaited {V}")
		TasksPhases("H5", T)

	H5_AwaitSuspended<public>():void =
		Print("H5 start")
		spawn{H5Driver()}
		Print("H5 after spawn")

	H6_SignalAwaited<public>():void =
		Print("H6 signal")
		EvH5.Signal(12)
		Print("H6 signal returned")
