using { /Godot.org/Godot }
using { /Verse.org/Concurrency }

# docs/web-vm/spec/tasks.md's evidence: the order of every observable event in spawn, branch,
# sync, race, rush, defer, Cancel, task.Await, event.Signal and a raise, as VerseVM produces it.
#
#   bin/verse_probe.exe <engine>/Engine/Binaries/Win64/verse_host.dll <engine>/Engine \
#       tests/verse_probe/vm_tasks_probe.verse --class vm_tasks_probe
#
# The driver calls every zero-argument, non-suspending method in declaration order and ticks
# once between calls, so each group below is a sequence of steps a frame apart. Resumption is
# driven by event(int) rather than Sleep wherever order matters, because Signal resumes
# synchronously and the tick's wake order is natives.md's business, not this file's.

vm_tasks_probe := class(object):

	EvA:event(int) = event(int){}
	EvB1:event(int) = event(int){}
	EvB2:event(int) = event(int){}
	EvB3a:event(int) = event(int){}
	EvB3b:event(int) = event(int){}
	EvB3c:event(int) = event(int){}
	EvB4a:event(int) = event(int){}
	EvB4b:event(int) = event(int){}
	EvN1:event(int) = event(int){}
	EvN2:event(int) = event(int){}
	EvN3:event(int) = event(int){}
	EvC:event(int) = event(int){}
	EvC2:event(int) = event(int){}
	EvD1:event(int) = event(int){}
	EvD2:event(int) = event(int){}
	EvDHold:event(int) = event(int){}
	EvDE1:event(int) = event(int){}
	EvDE2:event(int) = event(int){}
	EvE1:event(int) = event(int){}
	EvE2:event(int) = event(int){}
	EvEC1:event(int) = event(int){}
	EvEC2:event(int) = event(int){}
	EvF:event(int) = event(int){}
	EvT:event(int) = event(int){}
	EvH:event(int) = event(int){}
	EvH2:event(int) = event(int){}
	EvQ:event(int) = event(int){}
	EvR:event(int) = event(int){}
	EvR2:event(int) = event(int){}

	var FTask:?task(void) = false
	var TTask:?task(int) = false
	var HTask:?task(void) = false
	var STask:?task(void) = false
	var QTask:?task(void) = false

	# Shared arms. Each prints when it starts, when it ends normally and from its defer, so a
	# cancelled arm is visible as a defer line with no end line before it.
	Arm(Name:string, Ev:event(int))<suspends>:int =
		defer:
			Print("  {Name} defer")
		Print("  {Name} start")
		V := Ev.Await()
		Print("  {Name} end")
		V

	Imm(Name:string, V:int)<suspends>:int =
		defer:
			Print("  {Name} defer")
		Print("  {Name} runs to end")
		V

	# --- A: spawn runs the body at once, up to its first suspension -----------------------

	ABody()<suspends>:void =
		Print("  A body start")
		EvA.Await()
		Print("  A body resumed")

	AImmBody()<suspends>:void =
		Print("  A2 body runs to end")

	A1_Spawn():void =
		Print("A1 before spawn")
		spawn{ABody()}
		Print("A1 after spawn")

	A2_SpawnImmediate():void =
		Print("A2 before spawn")
		spawn{AImmBody()}
		Print("A2 after spawn")

	A3_Signal():void =
		Print("A3 before signal")
		EvA.Signal(1)
		Print("A3 after signal")

	# --- B: race ---------------------------------------------------------------------------

	B1Driver()<suspends>:void =
		X := race:
			Imm("b1-arm1", 10)
			Arm("b1-arm2", EvB1)
		Print("  B1 race result {X}")

	B1_FirstArmImmediate():void =
		Print("B1 start")
		spawn{B1Driver()}
		Print("B1 after spawn")

	B2Driver()<suspends>:void =
		X := race:
			Arm("b2-arm1", EvB2)
			Imm("b2-arm2", 20)
		Print("  B2 race result {X}")

	B2_SecondArmImmediate():void =
		Print("B2 start")
		spawn{B2Driver()}
		Print("B2 after spawn")

	B3Driver()<suspends>:void =
		X := race:
			Arm("b3-arm1", EvB3a)
			Arm("b3-arm2", EvB3b)
			Arm("b3-arm3", EvB3c)
		Print("  B3 race result {X}")

	B3_Start():void =
		Print("B3 start")
		spawn{B3Driver()}
		Print("B3 after spawn")

	B3_SignalMiddle():void =
		Print("B3 signal arm2")
		EvB3b.Signal(32)
		Print("B3 signal returned")

	B4Fn()<suspends>:int =
		defer:
			Print("  B4 fn defer")
		race:
			block:
				defer:
					Print("  b4-arm1 defer")
				Print("  b4-arm1 start")
				EvB4a.Await()
				Print("  b4-arm1 returns 7 from the function")
				return 7
			block:
				defer:
					Print("  b4-arm2 defer")
				Print("  b4-arm2 start")
				EvB4b.Await()
				Print("  b4-arm2 end")
		Print("  B4 after race (only if arm2 won)")
		99

	B4Driver()<suspends>:void =
		R := B4Fn()
		Print("  B4 fn returned {R}")

	B4_ReturnFromArm():void =
		Print("B4 start")
		spawn{B4Driver()}
		Print("B4 after spawn")

	B4_Signal():void =
		Print("B4 signal arm1")
		EvB4a.Signal(0)
		Print("B4 signal returned")

	# --- N: race around sync ---------------------------------------------------------------

	NDriver()<suspends>:void =
		Print("  N begin")
		R := race:
			block:
				Print("  n-r1 begin")
				S := sync:
					Arm("n-s1", EvN1)
					Arm("n-s2", EvN2)
				Print("  n-r1 sync done {S(0)} {S(1)}")
				S(0) + S(1)
			block:
				Print("  n-r2 begin")
				Arm("n-r2", EvN3)
		Print("  N end {R}")

	N1_Start():void =
		Print("N1 start")
		spawn{NDriver()}
		Print("N1 after spawn")

	N2_SignalS1():void =
		Print("N2 signal s1")
		EvN1.Signal(1)
		Print("N2 signal returned")

	N3_SignalS2():void =
		Print("N3 signal s2")
		EvN2.Signal(2)
		Print("N3 signal returned")

	# --- C: sync ---------------------------------------------------------------------------

	CDriver()<suspends>:void =
		S := sync:
			Imm("c-arm1", 1)
			Arm("c-arm2", EvC)
			Imm("c-arm3", 3)
		Print("  C result {S(0)} {S(1)} {S(2)}")

	C1_Start():void =
		Print("C1 start")
		spawn{CDriver()}
		Print("C1 after spawn")

	C2_Signal():void =
		Print("C2 signal")
		EvC.Signal(2)
		Print("C2 signal returned")

	C3Driver()<suspends>:void =
		S := sync:
			Arm("c3-arm1", EvC2)
			Arm("c3-arm2", EvC2)
		Print("  C3 result {S(0)} {S(1)}")

	C3_TwoOnOneEvent():void =
		Print("C3 start")
		spawn{C3Driver()}
		Print("C3 after spawn")

	C4_Signal():void =
		Print("C4 signal")
		EvC2.Signal(4)
		Print("C4 signal returned")

	# --- D: rush ---------------------------------------------------------------------------

	DDriver()<suspends>:void =
		X := rush:
			Arm("d-arm1", EvD1)
			Arm("d-arm2", EvD2)
		Print("  D rush result {X}")
		EvDHold.Await()
		Print("  D driver end")

	D1_Start():void =
		Print("D1 start")
		spawn{DDriver()}
		Print("D1 after spawn")

	D2_SignalArm1():void =
		Print("D2 signal arm1")
		EvD1.Signal(41)
		Print("D2 signal returned")

	D3_SignalArm2():void =
		Print("D3 signal arm2")
		EvD2.Signal(42)
		Print("D3 signal returned")

	D4_Release():void =
		Print("D4 release driver")
		EvDHold.Signal(0)
		Print("D4 signal returned")

	DEDriver()<suspends>:void =
		X := rush:
			Imm("de-arm1", 1)
			Arm("de-arm2", EvDE1)
			Arm("de-arm3", EvDE2)
		Print("  DE rush result {X}")

	D5_RushThenEnd():void =
		Print("D5 start")
		spawn{DEDriver()}
		Print("D5 after spawn")

	# --- E: branch -------------------------------------------------------------------------

	EDriver()<suspends>:void =
		Print("  E begin")
		branch:
			defer:
				Print("  e-branch defer")
			Print("  e-branch start")
			EvE1.Await()
			Print("  e-branch end")
		Print("  E after branch")
		EvE2.Await()
		Print("  E driver end")

	E1_Start():void =
		Print("E1 start")
		spawn{EDriver()}
		Print("E1 after spawn")

	E2_SignalBranch():void =
		Print("E2 signal branch")
		EvE1.Signal(0)
		Print("E2 signal returned")

	E3_SignalDriver():void =
		Print("E3 signal driver")
		EvE2.Signal(0)
		Print("E3 signal returned")

	ECDriver()<suspends>:void =
		branch:
			defer:
				Print("  ec-branch defer")
			Print("  ec-branch start")
			EvEC1.Await()
			Print("  ec-branch end (never)")
		Print("  EC after branch")
		EvEC2.Await()
		Print("  EC driver end")

	E4_BranchOutlived():void =
		Print("E4 start")
		spawn{ECDriver()}
		Print("E4 after spawn")

	E5_EndDriverFirst():void =
		Print("E5 signal driver")
		EvEC2.Signal(0)
		Print("E5 signal returned")

	# --- G: defer on normal completion -----------------------------------------------------

	GBody()<suspends>:void =
		defer:
			Print("  g d1")
		block:
			defer:
				Print("  g inner-block defer")
			Print("  g in block")
		defer:
			Print("  g d2")
		Print("  g body end")

	G1_NormalDefers():void =
		Print("G1 start")
		spawn{GBody()}
		Print("G1 after spawn")

	# --- F: Cancel of a suspended task, defers across frames -------------------------------

	FInner()<suspends>:void =
		defer:
			Print("  f inner defer 1")
		defer:
			Print("  f inner defer 2")
		Print("  f inner waiting")
		EvF.Await()
		Print("  f inner end (never)")

	FOuter()<suspends>:void =
		defer:
			Print("  f outer defer")
		FInner()
		Print("  f outer after inner (never)")

	F1_Start():void =
		Print("F1 start")
		T := spawn{FOuter()}
		set FTask = option{T}
		Print("F1 after spawn")

	FCanceller()<suspends>:void =
		Print("  f cancel begin")
		if (T := FTask?):
			T.Cancel()
			Print("  f cancel returned")
			if (T.Canceled[]):
				Print("  f task reads Canceled")
			if (T.Settled[]):
				Print("  f task reads Settled")
			if (T.Completed[]):
				Print("  f task reads Completed (never)")

	F2_Cancel():void =
		Print("F2 start")
		spawn{FCanceller()}
		Print("F2 after spawn")

	FAwaiter()<suspends>:void =
		if (T := FTask?):
			Print("  f awaiting a canceled task")
			T.Await()
			Print("  f await of canceled task returned (never)")

	F3_AwaitCanceled():void =
		Print("F3 start")
		spawn{FAwaiter()}
		Print("F3 after spawn")

	F4_SignalAfterCancel():void =
		Print("F4 signal the canceled task's event")
		EvF.Signal(0)
		Print("F4 signal returned")

	# --- T: task.Await --------------------------------------------------------------------

	TBody()<suspends>:int =
		Print("  t body waiting")
		X := EvT.Await()
		Print("  t body end")
		X + 1

	TWaiter(Name:string)<suspends>:void =
		if (T := TTask?):
			Print("  t {Name} awaiting")
			R := T.Await()
			Print("  t {Name} got {R}")

	T1_Start():void =
		Print("T1 start")
		T := spawn{TBody()}
		set TTask = option{T}
		spawn{TWaiter("w1")}
		spawn{TWaiter("w2")}
		Print("T1 end")

	T2_Signal():void =
		Print("T2 signal")
		EvT.Signal(5)
		Print("T2 signal returned")

	T3_AwaitCompleted():void =
		Print("T3 start")
		spawn{TWaiter("w3")}
		Print("T3 end")

	TCancelDone()<suspends>:void =
		if (T := TTask?):
			T.Cancel()
			Print("  t cancel of a completed task returned")
			if (T.Completed[]):
				Print("  t still Completed")

	T4_CancelCompleted():void =
		Print("T4 start")
		spawn{TCancelDone()}
		Print("T4 end")

	# --- H: Cancel of a running task -------------------------------------------------------

	HParent()<suspends>:void =
		defer:
			Print("  h parent defer")
		EvH.Await()
		Print("  h parent resumed")
		branch:
			defer:
				Print("  h child defer")
			Print("  h child cancels parent")
			if (T := HTask?):
				T.Cancel()
			Print("  h child after cancel (never)")
		Print("  h parent continues after branch")
		EvH2.Await()
		Print("  h parent after second await (never)")

	H1_Start():void =
		Print("H1 start")
		T := spawn{HParent()}
		set HTask = option{T}
		Print("H1 after spawn")

	H2_Signal():void =
		Print("H2 signal")
		EvH.Signal(0)
		Print("H2 signal returned")

	# --- Q: cancel a task that has children, and a canceler that awaits it ----------------

	QParent()<suspends>:void =
		defer:
			Print("  q parent defer")
		sync:
			Arm("q-kid1", EvQ)
			Arm("q-kid2", EvQ)
			Arm("q-kid3", EvQ)
		Print("  q parent after sync (never)")

	Q1_Start():void =
		Print("Q1 start")
		T := spawn{QParent()}
		set QTask = option{T}
		Print("Q1 after spawn")

	QCanceller(Name:string)<suspends>:void =
		if (T := QTask?):
			Print("  q {Name} cancels")
			T.Cancel()
			Print("  q {Name} cancel returned")

	Q2_Cancel():void =
		Print("Q2 start")
		spawn{QCanceller("c1")}
		spawn{QCanceller("c2")}
		Print("Q2 end")

	# --- S: Sleep and Cancel ---------------------------------------------------------------

	SSleeper()<suspends>:void =
		defer:
			Print("  s sleeper defer")
		Print("  s sleeping")
		Sleep(0.0)
		Print("  s woke (never)")

	SCanceller()<suspends>:void =
		if (T := STask?):
			T.Cancel()
			Print("  s cancel returned")

	S1_SleepThenCancel():void =
		Print("S1 start")
		T := spawn{SSleeper()}
		set STask = option{T}
		spawn{SCanceller()}
		Print("S1 end")

	SNegative()<suspends>:void =
		Print("  s2 before Sleep(-1.0)")
		Sleep(-1.0)
		Print("  s2 after Sleep(-1.0)")

	S2_NegativeSleep():void =
		Print("S2 start")
		spawn{SNegative()}
		Print("S2 end")

	# --- R: a raise ------------------------------------------------------------------------

	RSleeper()<suspends>:void =
		defer:
			Print("  r sleeper defer")
		Print("  r sleeper waiting")
		EvR.Await()
		Print("  r sleeper resumed")

	R1_Start():void =
		Print("R1 start")
		spawn{RSleeper()}
		Print("R1 end")

	R2_Raise():void =
		Print("R2 about to raise")
		Err("probe raise")

	R3_Signal():void =
		Print("R3 signal")
		EvR.Signal(0)
		Print("R3 signal returned")

	RRaiser()<suspends>:void =
		defer:
			Print("  r raiser defer")
		EvR2.Await()
		Print("  r raiser resumed, raising")
		Err("raise in a resumed awaiter")

	R4_Start():void =
		Print("R4 start")
		spawn{RRaiser()}
		Print("R4 end")

	R5_Signal():void =
		Print("R5 signal")
		EvR2.Signal(0)
		Print("R5 signal returned")

	Z_Done():void =
		Print("Z done")
