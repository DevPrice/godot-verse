using { /Godot.org/Godot }
using { /Verse.org/Concurrency }

# Is the Verse book's "Subscribable Events" reachable from a script package, and would it be a
# better member type for `@export_signal` than a plain `event(t)`?
#
#   bin/verse_probe.exe <engine>/Engine/Binaries/Win64/verse_host.dll <engine>/Engine \
#       tests/verse_probe/subscribable_event_probe.verse --class subscribable_event_probe
#
# The book (verselang.github.io/book/14_concurrency) calls it `subscribable_event` and says outright
# that the feature "has not yet been released and is not currently available". What this engine drop
# has instead is `subscribable_event_intrnl`, declared `<epic_internal>` in Event.native.verse and
# carrying a comment of its own: "This type should be deleted and use event instead once we get
# event API changes complete."
#
# Epic's own code uses it through one pattern, which is the one copied below: the member is typed as
# the *interface* `listenable(t)` and only its initialiser names the concrete type. See
# agent_group.native.verse:77 and DamageableComponent.native.verse:12.

# Q1: does the unmangled name the book uses exist at all?
#
#     BookSpelling<public>:listenable(int) = subscribable_event(int){}
#
# Asked and answered as a comment rather than as code -- it is error 3506, "Unknown identifier
# `subscribable_event`". The book is describing an unreleased feature and this drop does not have it
# under that name.

subscribable_event_probe := class(object):

	# Q2: is the mangled, epic_internal one reachable from a *script* package? This is the question
	# that decides everything else. Script packages are EVerseScope::InternalUser (HostScript.cpp),
	# and that scope unlocks access to epic_internal definitions without granting authorship -- so
	# the expectation is yes, and the point of asking is to be sure rather than to infer.
	Held<public>:listenable(int) = subscribable_event_intrnl(int){}

	# Q3: does it satisfy `listenable(int)` as a value, which is the one thing a plain `event(t)`
	# cannot do -- `event` is `signalable` + `awaitable` with no `subscribable`.
	AsListenable<public>()<transacts>:listenable(int) = Held

	# Q4: and does its `Subscribe` resolve off the interface, with no bridge extension method in the
	# way?
	OnScored(Points:int):void = Print("scored {Points}")

	SubscribeLocally<public>()<transacts>:void =
		Cancel := Held.Subscribe(OnScored)
		Cancel.Cancel()

	# Q5: can it be signalled from a <transacts> body -- the effect every Godot callback arrives in?
	# Epic says no in agent_group.native.verse:90, in as many words: "Signalling is native because
	# `listenable` has no `Signal`, and `subscribable_event_intrnl.Signal` has the `no_rollback`
	# effect, which a <transacts> function cannot call." Two things follow if that holds here, and
	# both matter more than the type choice: the emit verb has to be native whichever event type is
	# used, and `listenable` alone cannot emit at all.
	#
	#     SignalFromTransacts<public>()<transacts>:void = Held.Signal(3)
	#
	# is left as a comment because `listenable` has no `Signal` to call -- the member's declared type
	# is the interface. The concrete-typed version is Q6.

	Concrete<public>:subscribable_event_intrnl(int) = subscribable_event_intrnl(int){}

	# Q6: the same call with the concrete type, where `Signal` does exist.
	SignalFromDefault<public>():void =
		Concrete.Signal(5)

	# Q7 is answered and the answer is a refusal, so it is recorded here rather than compiled:
	#
	#     SignalFromTransacts<public>()<transacts>:void = Concrete.Signal(6)
	#
	# is 3512 -- "calls a function (`(/Verse.org/Verse/subscribable_event_intrnl:)Signal`) that has
	# the 'no_rollback' effect". Worth running rather than reading, because Event.native.verse
	# declares the override as `Signal<native><override>(Val:t)<predicts>:void` and `<predicts>`
	# reads like it might narrow the base's default set. It does not.
	#
	# So the emit verb stays the bridge's whichever event type a member is declared with, which is
	# what Epic concluded in their own code: agent_group.native.verse:90 says "Signalling is native
	# because `listenable` has no `Signal`, and `subscribable_event_intrnl.Signal` has the
	# `no_rollback` effect, which a <transacts> function cannot call."

	Answer<public>():void =
		Print("subscribable_event_probe compiled")
