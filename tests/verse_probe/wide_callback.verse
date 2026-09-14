using { /Godot.org/Godot }

# Phase 5 spike S-0, round three: what widening `godot_signal.Subscribe`'s callback would cost.
#
# Verse's own prior art is `subscribable<native>(t:type) := interface:
# Subscribe<public>(Callback(:t):void)<transacts>:cancelable` -- the callback parameter is
# specifier-less and only the method is <transacts>. GodotApi.native.verse declares
# `Subscribe(Callback(:t)<transacts>:void)` instead, on the reading that Verse's own "fixes its
# callback at a no_rollback domain that could not touch Godot". That reading has the lattice
# backwards: the default set *contains* transacts, so a wide callback can touch Godot and more.
#
# Round one of this file asked whether a <transacts> body could *invoke* a wide callback and the
# answer was no -- 3512, no_rollback. That is why Epic's `signalable.Signal`, which invokes the
# callbacks, carries the default set while `Subscribe`, which only stores one, is <transacts>.
# Our bridge never invokes a subscriber from Verse at all: `VhSignalSubscribe(Id, Callback:any)`
# hands it to the host and the host calls it through VFunction::Invoke, where Verse's effect
# checking is not in the way. So the question that actually decides the widening is storage, and
# that is what this asks.
#
#   bin/verse_probe.exe <engine>/Engine/Binaries/Win64/verse_host.dll <engine>/Engine \
#       tests/verse_probe/wide_callback.verse --class wide_callback

wide_callback := class(object):

	var Seen:int = 0

	Ev:event(int) = event(int){}

	# Stands in for `VhSignalSubscribe(Id:int, Callback:any)<transacts>:int`, which is how a
	# callback actually leaves Verse today.
	Store<public>(Callback:any)<transacts>:void = {}

	# Stands in for a widened godot_signal.Subscribe: the method <transacts>, the callback wide,
	# and the callback only handed onward rather than called.
	SubscribeWide<public>(Callback(:int):void)<transacts>:void =
		Store(Callback)

	# An existing handler, written the way every Dodge the Creeps handler is written today.
	NarrowHandler<public>(X:int)<transacts>:void =
		set Seen = X

	# A handler written the way a widened Subscribe would allow -- this is the one that matters,
	# because wall 3 needs a handler that can start a task.
	WideHandler<public>(X:int):void =
		set Seen = X
		spawn{Waiter()}

	Waiter()<suspends>:void =
		Ev.Await()
		Print("waiter resumed")

	# Q1: does today's handler still satisfy a widened parameter? If not, widening breaks every
	# script that already has one and the cost stops being one line. Effects are contravariant,
	# but contravariance in *this* position is the claim being tested.
	NarrowIntoWide<public>()<transacts>:void =
		SubscribeWide(NarrowHandler)

	# Q2: the point of the exercise -- a task-spawning handler accepted by a <transacts> subscribe.
	WideIntoWide<public>()<transacts>:void =
		SubscribeWide(WideHandler)

	# Q3: and the direction that should be refused, kept as a comment because it is. A wide handler
	# where a narrow one is asked for is what contravariance does not permit:
	#
	#     TakesNarrow<public>(Callback(:int)<transacts>:void)<transacts>:void = Callback(2)
	#     WideIntoNarrow<public>()<transacts>:void = TakesNarrow(WideHandler)

	# Q4: the end-to-end shape, run rather than read -- a wide handler called the way the host
	# calls one, spawning a task, which a later call resumes.
	FireHandler<public>():void =
		WideHandler(9)

	Fire<public>():void =
		Ev.Signal(1)

	Report<public>()<transacts>:int = Seen
