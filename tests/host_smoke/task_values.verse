using { /Godot.org/Godot }
using { /Verse.org/Concurrency }

# The methods of a `task(t)` value, and a runtime error's frames, as an exported game sees them.
#
# host_smoke never loads this: tools/run_tests.py cooks it with the other fixtures and runs the
# class through the *runtime* host with bin/cooked_probe.exe, because both defects it guards were
# the runtime host's alone. Every method of a task value was a jump to address 0 there (T5.8), and
# a runtime error arrived with no frames (T5.7). The probe calls the public methods in declaration
# order and ticks nothing, so each task here is driven by an event signalled from a later call.

task_values := class(object):

	Gate:event(int) = event(int){}

	Held()<suspends>:int =
		Gate.Await()

	Quick()<suspends>:int =
		5

	PhaseWhileSuspended<public>():void =
		T := spawn{Held()}
		if (T.Active[]) then Print("task_values: active") else Print("task_values: not active")
		if (T.Completed[]) then Print("task_values: completed") else Print("task_values: not completed")

	PhaseWhenFinished<public>():void =
		T := spawn{Quick()}
		if (T.Completed[]) then Print("task_values: completed") else Print("task_values: not completed")
		if (T.Settled[]) then Print("task_values: settled") else Print("task_values: not settled")

	CancelDriver()<suspends>:void =
		T := spawn{Held()}
		T.Cancel()
		if (T.Canceled[]) then Print("task_values: canceled") else Print("task_values: not canceled")

	CancelSuspended<public>():void =
		spawn{CancelDriver()}

	AwaitFinishedDriver()<suspends>:void =
		T := spawn{Quick()}
		V := T.Await()
		Print("task_values: awaited {V}")

	AwaitFinished<public>():void =
		spawn{AwaitFinishedDriver()}

	AwaitHeldDriver()<suspends>:void =
		T := spawn{Held()}
		Print("task_values: awaiting")
		V := T.Await()
		Print("task_values: awaited held {V}")

	AwaitSuspended<public>():void =
		spawn{AwaitHeldDriver()}

	Release<public>():void =
		Gate.Signal(9)

	Raise<public>():void =
		Err("task_values raise")
