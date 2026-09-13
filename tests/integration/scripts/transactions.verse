using { /Godot.org/Godot }

# Phase 4.5 spikes S-3 and S-4 (docs/phase-4.5-design.md 2), and afterwards the behavioural record
# behind the rule in `spec.md` next to R-AUD-1.
#
# The question is what a *failure* undoes. Three kinds of failure reach here, and the measured answer
# is that all three undo the same thing -- every Godot write the failing computation had deferred:
#
#   - a **nested failure context**, `if (X := F[])`, which Verse's own VM unwinds;
#   - a **`<decides>` method that declines at the top level** of a call from Godot. This one was
#     expected to behave differently and does not: `InstanceCall` reads it as `FOpResult::Fail` and
#     its own `AutoRTFM::Transact` commits normally, so the host's transaction is not what drops the
#     write. VerseVM wraps the invocation of a `<decides>` function in a failure context of its own,
#     and that is the transaction the deferral was registered against;
#   - a **raise**, which unwinds to the root failure context and aborts the host's transaction --
#     and additionally stops every script until the next `vh_tick`, which is why `test_main.gd`
#     runs this section a step per frame.
#
# Every assertion is in `test_main.gd`, because only Godot can say what the node's properties ended
# up as.

@global_class
transactions := class(node2d):

	# --- what `<reads>` buys (Phase 4.5 3) ----------------------------------------------------

	# The shape Stage 1 exists for: a helper that only *looks* at the scene, narrowed one step
	# below `<transacts>` so that it does not force `<transacts>` onto everything that calls it.
	# Before the const surface carried `<reads>` this did not compile, because reading Godot was
	# `<transacts>` and an explicit specifier narrows.
	#
	# Four kinds of read in one body, because they reach Godot by four different roads: a const
	# *method*, a *property* (whose getter is still `<transacts>` -- S-1 -- and is reachable anyway,
	# because the read site is not checked against the getter's effect), `IsInstanceValid`, and a
	# mirrored math type's own extension method.
	DescribeReads<public>()<reads>:float =
		if (IsInstanceValid[Self]):
			Position.Length() + GetChildCount() * 1.0
		else:
			-1.0

	# And the widening direction: a `<transacts>` method calling the `<reads>` one. If this were
	# refused, narrowing 3942 declarations would have broken every script.
	ReadFromTransacts<public>()<transacts>:float = DescribeReads()

	# A failure context inside a `<reads>` body, which is wall 8's own shape one step narrower than
	# wall 8 could be written.
	FirstChildName<public>()<reads>:string =
		if (Child := GetChild[0]) then Child.GetName() else "none"

	# --- what a deferred write is worth ------------------------------------------------------

	# A write inside a failure context that declines. `set Position` is `VhCallVoid`, which defers
	# to `AutoRTFM::OnCommit`.
	SetThenFailInner<public>()<transacts>:void =
		if (WriteAndDecline[]):
			set Rotation = 99.0

	WriteAndDecline<private>()<decides><transacts>:int =
		set Position = vector2{X := 11.0, Y := 11.0}
		1 > 2

	# The same write, declining at the top level, where the only Verse failure context is the one the
	# VM puts around a `<decides>` invocation.
	SetThenDecline<public>()<decides><transacts>:int =
		set Position = vector2{X := 22.0, Y := 22.0}
		1 > 2

	# The same write followed by a raise, which is the case the deferral was built for.
	SetThenRaise<public>()<transacts>:void =
		set Position = vector2{X := 33.0, Y := 33.0}
		Dead := node2d{}
		Dead.QueueFree()

	# --- what a read sees while a write is pending -------------------------------------------

	var ObservedX<public>:float = -1.0

	# A read after a deferred write in the same call. The write has not happened, so this must see
	# the old value -- which is the sharp edge the rule has to name.
	SetThenRead<public>()<transacts>:void =
		set Position = vector2{X := 44.0, Y := 44.0}
		set ObservedX = Position.X

	ReadObservedX<public>()<transacts>:float = ObservedX

	# Two deferred writes to one property: the second must win, so the queue is ordered rather
	# than a set.
	SetTwice<public>()<transacts>:void =
		set Position = vector2{X := 55.0, Y := 55.0}
		set Position = vector2{X := 66.0, Y := 66.0}

	# --- what a mutate-and-answer call does ---------------------------------------------------

	# The shape of all 1127 in `docs/nonatomic-methods.md`, in one method: `Object.connect` mutates
	# Godot *and* answers an Error, so it can be neither deferred nor ignored -- and unlike
	# `Subscribe` nothing compensates it, because the bridge forwards it rather than performing it
	# and so has no inverse to register. A raise after it must therefore leave the connection behind.
	ConnectThenRaise<public>()<transacts>:void =
		Connect("Scored", MakeCallable(int, OnScored))
		Dead := node2d{}
		Dead.QueueFree()

	# --- S-3: does a rollback compensation reach this call path? -------------------------------

	Hit<public>:godot_signal() = godot_signal(){}

	# The one `ConnectThenRaise` uses: `MakeCallable` wants a type argument, and an int payload is
	# the plainest one to spell.
	Scored<public>:godot_signal(int) = godot_signal(int){}

	var Hits<public>:int = 0

	OnHit<public>()<transacts>:void =
		set Hits = Hits + 1

	OnScored<public>(Points:int)<transacts>:void =
		set Hits = Hits + Points

	ReadHits<public>()<transacts>:int = Hits

	# The compensated shape: `VhSignalSubscribe` mutates Godot and answers, so the host registers an
	# `AutoRTFM::OnAbort<SameAsClosed>` that disconnects. These three ask whether it runs, once per
	# kind of failure -- and when they were first written the answer was no three times over, which
	# is how S-3 found that Phase 4's `Verse::Stm::OnRollback` had never run.
	SubscribeThenRaise<public>()<transacts>:void =
		Hit.Subscribe(OnHit)
		Dead := node2d{}
		Dead.QueueFree()

	SubscribeThenDecline<public>()<decides><transacts>:int =
		Hit.Subscribe(OnHit)
		1 > 2

	SubscribeThenFailInner<public>()<transacts>:void =
		if (SubscribeAndDecline[]):
			set Rotation = 99.0

	SubscribeAndDecline<private>()<decides><transacts>:int =
		Hit.Subscribe(OnHit)
		1 > 2

	# The control: no failure at all, so the connection stays.
	SubscribePlainly<public>()<transacts>:void =
		Hit.Subscribe(OnHit)

	EmitHit<public>()<transacts>:void = Hit.Signal(())
