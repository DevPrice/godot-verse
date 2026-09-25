using { /Godot.org/Godot }
using { /Verse.org/Concurrency }

# docs/web-vm/spec/tasks.md's refusals: what the compiler forbids about suspending, so the
# interpreter never has to define it. Nothing here is meant to compile; the text of each glitch is
# the result.
#
#   bin/verse_probe.exe <engine>/Engine/Binaries/Win64/verse_host.dll <engine>/Engine \
#       tests/verse_probe/vm_tasks_reject.verse

vm_tasks_reject := class(object):

	Ev:event(int) = event(int){}

	Waits()<suspends>:int = Ev.Await()

	# 1. A suspending call inside a failure context.
	SuspendInIf()<suspends>:void =
		if (Waits() = 1):
			Print("x")

	# 2. A suspending call in a defer body.
	SuspendInDefer()<suspends>:void =
		defer:
			Ev.Await()
		Print("x")

	# 3. A race inside a failure context.
	RaceInIf()<suspends>:void =
		if (race{Waits(); Waits()} = 1):
			Print("x")

	# 4. A suspending call in a <decides> function.
	SuspendDecides()<suspends><decides>:void =
		Waits() = 1

	# 5. spawn inside a failure context.
	SpawnInIf():void =
		if (T := spawn{Waits()}, 1 = 2):
			Print("x")

	# 6. branch outside a suspending context.
	BranchNotSuspends():void =
		branch:
			Waits()

	# 7. await on a var -- the epic_internal `await` macro -- in a wide body.
	var Counter:int = 0
	AwaitVar()<suspends>:void =
		await{Counter > 3}
		Print("x")

	# 8. batch.
	BatchVar():void =
		batch:
			set Counter = 1
