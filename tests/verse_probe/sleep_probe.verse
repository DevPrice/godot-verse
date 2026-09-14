using { /Godot.org/Godot }

# Phase 5, the two things the probe can answer that no Godot is needed for: does the native
# `<suspends>` Sleep actually suspend and resume from the pump, and does a `defer` in a suspending
# body run when the task is cancelled rather than resumed?
#
# The second is what `godot_signal.Await` rests on -- its `defer` is the only thing that takes the
# Godot connection away when a `race` loses, a node is freed, or a rebuild cancels everything -- and
# nothing about it is obvious from the language reference.
#
#   bin/verse_probe.exe <engine>/Engine/Binaries/Win64/verse_host.dll <engine>/Engine \
#       tests/verse_probe/sleep_probe.verse --class sleep_probe
#
# The driver calls every zero-argument non-suspending method in declaration order and ticks between
# each, so Start, Report and the rest are one frame apart.

sleep_probe := class(object):

	var Woke:int = 0
	var Deferred:int = 0
	var Raced:int = 0

	# The plain case. `Sleep(0.0)` means "resume at the next pump", which is the cheapest way to
	# yield a frame.
	Waker()<suspends>:void =
		Sleep(0.0)
		set Woke = Woke + 1
		Print("Sleep(0.0) resumed")

	StartSleep<public>():void =
		spawn{Waker()}
		Print("StartSleep returned")

	ReportWoke<public>()<transacts>:int = Woke

	# Does `defer` run when a task is cancelled? `race` cancels the loser, and the loser here sleeps
	# for an hour, so anything it prints after the race is a defer that ran on cancellation.
	Loser()<suspends>:void =
		defer:
			set Deferred = Deferred + 1
			Print("loser's defer ran")
		Sleep(3600.0)
		Print("loser finished, which should not happen")

	Winner()<suspends>:void =
		Sleep(0.0)
		Print("winner finished")

	Racer()<suspends>:void =
		race:
			Loser()
			Winner()
		set Raced = 1
		Print("race returned")

	StartRace<public>():void =
		spawn{Racer()}
		Print("StartRace returned")

	ReportDeferred<public>()<transacts>:int = Deferred
	ReportRaced<public>()<transacts>:int = Raced

	# A handler that still says `<transacts>` has to keep satisfying the widened Subscribe callback
	# (effects are contravariant in a parameter). Compile-only: there is no Godot here to emit, and
	# the signal is unbound, so nothing is called.
	Narrow(X:int)<transacts>:void =
		Print("narrow handler")

	# And a wide one, which is what the widening was for: only a wide handler may spawn.
	Wide(X:int):void =
		spawn{Waker()}

	Connect<public>()<transacts>:void =
		Sig := godot_signal(int){}
		Sig.Subscribe(Narrow)
		Sig.Subscribe(Wide)
		Print("both handlers satisfied Subscribe")
