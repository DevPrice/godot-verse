using { /Godot.org/Godot }
using { /Verse.org/Concurrency }

# The two questions Stage 0 of the `@signal` plan still has open. `event_probe.verse` and
# `event_resume.verse` already settled the rest: `event(t)` is nameable and constructible from a
# script package, a `<suspends>` body awaits it and gets `t` back typed, `Ev.Signal` works from a
# specifier-less body and is 3512 from a `<transacts>` one, and a task suspended on it resumes on a
# later call.
#
#   bin/verse_probe.exe <engine>/Engine/Binaries/Win64/verse_host.dll <engine>/Engine \
#       tests/verse_probe/signal_listenable_probe.verse --class signal_listenable_probe
#
# Q1: is `listenable(t)` implementable from a user package at all? Two things could refuse it --
# `awaitable.Await` is `<suspends>` and wide, so the override has to be too; and `listenable` is
# declared in /Verse.org/Concurrency, whose reachability from here is what `cancelable` already
# suggests but does not prove for an *interface* being implemented rather than a class being named.
# If this compiles, `signal(t)` can carry the interfaces and every engine-signal accessor becomes a
# `listenable` with no change to the connection model.
listenable_holder<public>(t:type) := class(listenable(t)):

	Ev<public>:event(t) = event(t){}

	Await<override>()<suspends>:t = Ev.Await()

	# `subscribable.Subscribe(Callback(:t):void)<transacts>:cancelable`, matched exactly. The real
	# one answers a `connection` over VhSignalSubscribe; here anything implementing `cancelable`
	# proves the signature, so the probe uses the only one that needs no host.
	Subscribe<override>(Callback(:t):void)<transacts>:cancelable = probe_cancel{}

probe_cancel<public> := class(cancelable):
	Cancel<override>()<transacts>:void = {}

# Q2: does a parametric extension method on a *native parametric* class compile? This is the shape
# `Emit` would take -- the author's emit verb under `@signal`, which has to be `<transacts>` because
# every Godot callback runs in one and `event.Signal` is refused there (event_probe.verse Q2).
#
# If this is refused, the fallback is a module-level `Emit(Ev:event(t), Val:t)`, which costs the
# receiver spelling and nothing else.
#
# `where t:type` sits inside the receiver's parens, which is how /Verse.org/Verse spells its own --
# `(Input:[]t where t:type).Slice(...)` in Array.native.verse, and
# `(InSet:classifiable_subset_var(t) where t:type).Add(...)` for the parametric-class case. Written
# after the receiver it is 3502, "can't access a data definition's value from a preceding
# expression", reported at the receiver's type rather than at `t`.
#
# The body stands in for `VhEventEmit(Ev, Val)`: the native question -- whether an `event` crosses
# to the host through `any` -- is a host question and is not asked here. `VhSignalSubscribe(Id:int,
# Callback:any)` already passes a Verse object that way.
(Ev:event(t) where t:type).Emit<public>(Val:t)<transacts>:void =
	Print("Emit reached")

signal_listenable_probe := class(object):

	Held:listenable_holder(int) = listenable_holder(int){}

	Plain:event(int) = event(int){}

	# Q1a: does the interface-typed view of it satisfy `listenable(int)`? Declaring the class is one
	# thing; being *usable* as the interface is what the interop is for.
	AsListenable<public>()<transacts>:listenable(int) = Held

	# Q1b: and does awaiting through the interface work, which is what a library taking a
	# `listenable` would do.
	AwaitViaInterface<public>()<suspends>:int =
		L:listenable(int) = Held
		L.Await()

	# Q2a: the extension method on a bare event, spelled on the receiver.
	EmitViaExtension<public>()<transacts>:void =
		Plain.Emit(9)

	Answer<public>():void =
		Print("compiled")

# --- Stage 1 verification ----------------------------------------------------
#
# The above asks whether `listenable(t)` is implementable at all. This asks the thing Stage 1
# actually ships: that the *mirror's* `signal(t)` carries it, which the questions above cannot
# answer because `signal(t)`'s super is the native `vh_signal` and a native super is the one
# variable a user-package class does not have.

# A library function that knows nothing about Godot: it takes Verse's own interface. Nothing in
# this signature can be satisfied by a `signal(t)` unless the mirror declares the interface.
AwaitAnything<public>(L:listenable(int))<suspends>:int = L.Await()

stage1_probe := class(object):

	@export_signal
	Declared<public>:signal(int) = signal(int){}

	# Q3: does a script's own declared signal satisfy the interface, as a binding and as an
	# argument? This is what lets a script hand a signal to Verse code that has never heard of
	# this bridge.
	AsInterface<public>()<transacts>:listenable(int) = Declared

	ViaLibrary<public>()<suspends>:int = AwaitAnything(Declared)

	# Q4: and an *engine* accessor's result, which is the 503-accessor half of the win. A Timer is
	# not reachable here without a scene, so this only has to type-check -- the accessor answers
	# `signal(tuple())`, so satisfying `listenable(tuple())` is a question about the type and not
	# about the object.
	EngineAsInterface<public>(T:timer)<transacts>:listenable(tuple()) = T.Timeout()

	Answer<public>():void =
		Print("stage 1 compiled")
