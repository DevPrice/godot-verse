using { /Godot.org/Godot }

# Phase 5 spike S-5, the half that decides whether `Await` needs a native at all. event_probe.verse
# established that /Verse.org/Verse's `event(t)` is nameable from the script package and that a
# parametric Verse class can hold one and answer `t` from it. This asks the two things that decide
# whether that is usable as `signal(t).Await()`:
#
#   - can a *narrowed* <suspends> body await an event, or does `awaitable.Await` carry no_rollback
#     the way `signalable.Signal` does? If it is wide, only a specifier-less body -- which is what
#     a mirrored virtual override is, and what a Subscribe handler is *not* -- can spawn a waiter.
#   - does a task suspended on an event survive the call that spawned it and resume on a later
#     call, which is R-ASYNC-1 asked without a Godot signal in the way.
#
#   bin/verse_probe.exe <engine>/Engine/Binaries/Win64/verse_host.dll <engine>/Engine \
#       tests/verse_probe/event_resume.verse --class event_resume
#
# The driver calls every zero-argument non-suspending method in declaration order and ticks between
# each, so Start, Fire and Report below are one frame apart.

event_resume := class(object):

	Ev:event(int) = event(int){}

	var Seen:int = 0

	# The wide waiter: <suspends> alone, which is the default effect set plus suspends.
	WaiterWide()<suspends>:void =
		X := Ev.Await()
		set Seen = X
		Print("wide waiter resumed")

	# Q1 is answered and the answer is a refusal, so the declaration is a comment rather than code:
	#
	#     WaiterNarrow()<suspends><transacts>:void = Ev.Await()
	#
	# is 3512 -- "calls a function (`(/Verse.org/Verse/event:)Await`) that has the 'no_rollback'
	# effect". `awaitable.Await` is as wide as `signalable.Signal`, so an event-awaiting body cannot
	# be narrowed, and only a specifier-less caller can spawn one. A Subscribe handler is fixed at
	# <transacts> and therefore cannot.

	StartWide<public>():void =
		spawn{WaiterWide()}
		Print("StartWide returned")

	# Q2 follows Q1: spawning a wide waiter from a <transacts> body -- wall 3's actual shape, since
	# a button handler reaches script code through Subscribe -- is refused for the same reason.
	#
	#     StartNarrowFromTransacts<public>()<transacts>:void = spawn{WaiterWide()}

	Fire<public>():void =
		Ev.Signal(42)
		Print("Fire returned")

	Report<public>()<transacts>:int = Seen
