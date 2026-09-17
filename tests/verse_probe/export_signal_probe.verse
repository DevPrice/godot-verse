using { /Godot.org/Godot }

# `awaitable` alone needs this: `event` and `cancelable` are reachable through the Godot package's
# own imports, but /Verse.org/Concurrency is not. The compiler says so and names the using, which is
# the diagnostic a script author gets too.
using { /Verse.org/Concurrency }

# R-SIG-1's `@export_signal`, asked of the *analysis* half: which members reach the signal list,
# with what payload shape, and which are listed-and-refused.
#
#   bin/verse_probe.exe <engine>/Engine/Binaries/Win64/verse_host.dll <engine>/Engine \
#       tests/verse_probe/export_signal_probe.verse --class export_signal_probe
#
# The question this exists for is the one docs/signal-declaration.md 4 records as unmeasured:
# whether `SignalPayloadType` finds a populated type-variable substitution on an `event(t)` member.
# `signal(t)` is declared in /Godot.org/Godot and is read from source at the first build; `event(t)`
# is /Verse.org/Verse, which is read from a 32 KB digest. A null substitution does not fail -- it
# produces an empty shape, which registers the signal with *zero arguments* and emits nothing. So
# every event-typed member below carries a payload with a distinctive arity, and the probe's signal
# listing is what says whether it survived.

# A struct payload, which is the one spelling that carries argument *names* to the connect dialog.
strike_report := struct:
	Damage<public>:int = 0
	By<public>:string = ""

export_signal_probe := class(object):

	# --- the event spelling ---

	# Expect: 0 args.
	@export_signal
	Bare<public>:event() = event(){}

	# Expect: 1 arg, named for its type.
	@export_signal
	Scored<public>:event(int) = event(int){}

	# Expect: 2 args, Arg0 and Arg1 -- a tuple cannot name its elements.
	@export_signal
	Struck<public>:event(tuple(int, string)) = event(tuple(int, string)){}

	# Expect: 2 args, *named* Damage and By. This is the row that matters most: it proves both that
	# the substitution was read and that the struct branch of DescribePayload was reached through
	# an event rather than only through a signal.
	@export_signal
	Reported<public>:event(strike_report) = event(strike_report){}

	# --- what must NOT be listed ---

	# An event with no attribute is an ordinary Verse event, useful purely between tasks. Expect it
	# to be absent from the list entirely -- not listed and refused.
	Private<public>:event(int) = event(int){}

	# --- the signal spelling, which now needs the attribute too ---

	# Expect: 1 arg, reject 0.
	@export_signal
	Legacy<public>:signal(int) = signal(int){}

	# Expect: listed with VH_SIGNAL_NEEDS_ATTRIBUTE. A `signal(t)` has no purpose but Godot, so
	# silence here would be a Node panel that is empty for no stated reason.
	Forgotten<public>:signal(int) = signal(int){}

	# --- the runtime half's surface ---
	#
	# These only have to *compile* here: the probe's Godot is a stub whose every handle is dead, so
	# an emission reaches the host and stops. What is being asked is that the two extension methods
	# resolve on an `event(t)` from a script package, from a `<transacts>` body -- which is the
	# effect every Godot callback arrives in, and the one `event.Signal` is refused from.

	EmitBare<public>()<transacts>:void = Bare.Emit(())

	EmitScored<public>()<transacts>:void = Scored.Emit(11)

	EmitReported<public>()<transacts>:void = Reported.Emit(strike_report{Damage := 5, By := "spike"})

	OnScored(Points:int):void = Print("scored {Points}")

	# Subscribe answers a `cancelable`, which is Verse's own vocabulary rather than an invention.
	SubscribeScored<public>()<transacts>:void =
		Cancel := Scored.Subscribe(OnScored)
		Cancel.Cancel()

	# And the event is still an ordinary Verse event: this is the interop the whole change is for.
	AsAwaitable<public>()<transacts>:awaitable(int) = Scored

	Answer<public>():void =
		Print("export_signal_probe compiled")
