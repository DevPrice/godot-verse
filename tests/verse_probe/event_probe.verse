using { /Godot.org/Godot }

# Phase 5 spike S-5, the cheap half: is Verse's own /Verse.org/Concurrency reachable from the
# script package, and can a parametric Verse class hold an `event(t)` and answer `t` from it?
#
# If it is, `godot_signal(t).Await()` may need no parametric native at all -- the host resumes the
# event and Await is ordinary Verse. GodotApi.native.verse already names `cancelable`, which comes
# from that package, so the dependency is there; what is not known is whether `event` itself is
# nameable and whether `Signal` is callable from the <transacts> body a Godot callback runs in.
#
#   bin/verse_probe.exe <engine>/Engine/Binaries/Win64/verse_host.dll <engine>/Engine \
#       tests/verse_probe/event_probe.verse --class event_probe

# The shape godot_signal(t) would take if the answer is yes: ordinary parametric Verse over an
# event, with Await forwarding to it.
awaited_signal<public>(t:type) := class:
	Ev<public>:event(t) = event(t){}

	Await<public>()<suspends>:t = Ev.Await()

event_probe := class(object):

	Ints:awaited_signal(int) = awaited_signal(int){}

	# Q1: can a <suspends> body await it and get an `int` back, typed?
	AwaitInt<public>()<suspends>:int = Ints.Await()

	# Q2 was asked and the answer is a refusal, so it is recorded here rather than compiled:
	#
	#     SignalFromTransacts<public>()<transacts>:void = Ints.Ev.Signal(7)
	#
	# is 3512 -- "calls a function (`(/Verse.org/Verse/event:)Signal`) that has the 'no_rollback'
	# effect". Event.native.verse declares `Signal(Val:t):void` with no specifier, so it carries the
	# default set. That is why S-5 exists: `godot_signal.Signal` is <transacts> and cannot drive the
	# event itself, so the emission has to reach it from somewhere wide -- the host, or a
	# specifier-less method the host calls.

	# Q3: and from a specifier-less body, which is what a mirrored virtual override is.
	SignalFromDefault<public>():void =
		Ints.Ev.Signal(8)

	Answer<public>():void =
		SignalFromDefault()
