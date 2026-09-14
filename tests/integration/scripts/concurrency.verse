using { /Godot.org/Godot }

# R-ASYNC-1/2/4/5 and R-SIG-5: a task that suspends on a Godot signal and resumes when Godot emits.
#
# How a script starts one is the whole of what an author has to be taught, and it is two rules.
# **`spawn` is how**, because an awaiting body cannot be narrowed -- `event.Await` carries
# `no_rollback`, so a `<suspends><transacts>` body that awaits is a compile error -- and the caller
# has to be wide too. A mirrored virtual override is wide (the mirror generates them with no
# effect specifier), and since Phase 5 so is a signal handler. **`<suspends>` on the override is
# not**: `_Ready<override>()<suspends>:void` is glitch 3532, because the specifier makes it a
# different function and there is then no parent to override.
#
# The other half is that a `<reads>` body may not `spawn` at all -- the one place Phase 4.5's
# narrowing and this phase's concurrency collide.

@global_class
concurrency := class(node2d):

	Fired<public>:godot_signal() = godot_signal(){}
	Scored<public>:godot_signal(int) = godot_signal(int){}
	Struck<public>:godot_signal(tuple(int, string)) = godot_signal(tuple(int, string)){}

	var Seen<public>:int = 0
	var SeenBy<public>:string = ""
	var Stage<public>:int = 0

	# --- awaiting a signal this script declares -------------------------------

	WaitForFired()<suspends>:void =
		set Stage = 1
		Fired.Await()
		set Stage = 2
		Print("concurrency: resumed after Fired")

	StartWaitForFired<public>():void =
		spawn{WaitForFired()}

	# The payload comes back typed, which is what holding an `event(t)` buys over answering a list
	# of values: `t` is whatever the declaration said.
	WaitForScored()<suspends>:void =
		Points := Scored.Await()
		set Seen = Points

	StartWaitForScored<public>():void =
		spawn{WaitForScored()}

	# A tuple payload arrives as a tuple, which is what a two-argument emission decomposes from.
	WaitForStruck()<suspends>:void =
		Payload := Struck.Await()
		set Seen = Payload(0)
		set SeenBy = Payload(1)

	StartWaitForStruck<public>():void =
		spawn{WaitForStruck()}

	EmitFired<public>()<transacts>:void = Fired.Signal(())
	EmitScored<public>(Points:int)<transacts>:void = Scored.Signal(Points)
	EmitStruck<public>(Damage:int, By:string)<transacts>:void = Struck.Signal((Damage, By))

	ReadStage<public>()<transacts>:int = Stage
	ReadSeen<public>()<transacts>:int = Seen
	ReadSeenBy<public>()<transacts>:string = SeenBy
	Reset<public>()<transacts>:void =
		set Seen = 0
		set SeenBy = ""
		set Stage = 0

	# --- awaiting one of Godot's own ------------------------------------------
	#
	# The same method, because every one of the 489 mirrored engine-signal accessors answers a
	# `godot_signal(t)` too. Nothing per-signal was written for this.

	WaitForTimer(T:timer)<suspends>:void =
		T.Timeout().Await()
		set Stage = 10

	StartWaitForTimer<public>(T:timer):void =
		spawn{WaitForTimer(T)}

	# --- a signal nothing in the mirror knows about ---------------------------
	#
	# One a GDScript script declared, or one made with `add_user_signal`. There is no declaration
	# to read a payload type off, so the arguments arrive as the Godot Array they would have been
	# anyway and the script unpacks them with the accessors a `godot_array` already has.

	WaitForForeign(Owner:object, Name:string)<suspends>:void =
		Args := MakeSignal(Owner, Name).Await()
		if (Points := Args.GetInt[0]):
			set Seen = Points
		set Stage = 20

	StartWaitForForeign<public>(Owner:object, Name:string):void =
		spawn{WaitForForeign(Owner, Name)}

	OnForeign(Args:godot_array)<transacts>:void =
		if (Points := Args.GetInt[0]):
			set Seen = Seen + Points

	# The rollback-safe way to receive a foreign signal, and the reason to reach for it over
	# `Object.Connect`: a failure after this leaves no connection behind.
	SubscribeForeign<public>(Owner:object, Name:string)<transacts>:void =
		MakeSignal(Owner, Name).Subscribe(OnForeign)

	# The same, and then a failure, so the test can see the connection undone again. The failure is
	# a `<decides>` callee that declines, which is the cheapest of the three shapes Phase 4.5
	# measured and the one that needs no raise.
	SubscribeForeignThenFail<public>(Owner:object, Name:string)<transacts>:void =
		if (SubscribeAndDecline[Owner, Name]):
			set Rotation = 99.0

	SubscribeAndDecline<private>(Owner:object, Name:string)<decides><transacts>:int =
		MakeSignal(Owner, Name).Subscribe(OnForeign)
		1 > 2

	# --- what a race leaves behind --------------------------------------------
	#
	# The loser is cancelled, and its `defer` is what takes its Godot connection away. If it did
	# not run, `get_signal_connection_list` would still show the loser's.

	RaceTwo()<suspends>:void =
		race:
			Fired.Await()
			Scored.Await()
		set Stage = 30

	StartRace<public>():void =
		spawn{RaceTwo()}

	# --- Sleep ----------------------------------------------------------------
	#
	# Real time, on the host's own clock, because it has to work where there is no scene tree.
	# Game timing should await a Timer instead; this is here so the test can say the difference is
	# deliberate rather than accidental.

	Nap()<suspends>:void =
		Sleep(0.0)
		set Stage = 40

	StartNap<public>():void =
		spawn{Nap()}

	# --- a handler that starts a task -----------------------------------------
	#
	# What the widening in D20 was for. A `Subscribe` handler carries no effect specifier now, so
	# it is wide, so it may `spawn` -- which is the shape a game-over sequence is written in.

	OnScoredStartsTask(Points:int):void =
		spawn{WaitForFired()}

	SubscribeStartingTask<public>()<transacts>:void =
		Scored.Subscribe(OnScoredStartsTask)

	# --- what a raise costs (R-ASYNC-4, D2) -----------------------------------
	#
	# A task suspended on this instance dies with the raise; another instance's does not. That is
	# the whole point of a scope per instance, and the case host_smoke and this file both make.

	Raise<public>()<transacts>:void =
		# Reaching through a handle Godot has freed is a runtime error rather than a failure, which
		# is the cheapest raise a script can spell on purpose.
		Dead := node2d{}
		Dead.QueueFree()
