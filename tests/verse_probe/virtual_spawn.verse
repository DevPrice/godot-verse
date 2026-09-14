using { /Godot.org/Godot }

# Phase 5 spike S-0, round four: can a mirrored Godot virtual start a task?
#
# A virtual is generated with no effect specifier -- `_Ready<public>():void = {}`
# (GodotClasses.native.verse:28976) -- so an override carries the default effect set, which is the
# widest there is. That is the one context `spawn` of a wide <suspends> body is allowed from, so a
# virtual should be able to start a task where a <transacts> Subscribe handler cannot. This asks
# rather than assumes, because the whole of `Await`'s reach depends on it.
#
# async_reject.verse has the other half: writing <suspends> *on* the override is not an override at
# all -- it is 3532 "must have a distinct domain" plus 3523 "could not find a parent function to
# override", because the specifier makes it a different function.
#
#   bin/verse_probe.exe <engine>/Engine/Binaries/Win64/verse_host.dll <engine>/Engine \
#       tests/verse_probe/virtual_spawn.verse

virtual_spawn := class(node2d):

	Ev:event(int) = event(int){}

	Waiter()<suspends>:void =
		Ev.Await()
		Print("virtual_spawn waiter resumed")

	_Ready<override>():void =
		spawn{Waiter()}

	_Process<override>(Delta:float):void =
		spawn{Waiter()}

	# And the shape a script would actually write: a named sequence the virtual spawns, rather
	# than the spawn body itself.
	Sequence()<suspends>:void =
		Ev.Await()
		Print("first wait done")
		Ev.Await()
		Print("second wait done")

	_EnterTree<override>():void =
		spawn{Sequence()}
