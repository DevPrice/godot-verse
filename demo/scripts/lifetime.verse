using { /Godot.org/Godot }

# The lifetime experiment. Verse has no null, so a reference to a node Godot has freed has to
# mean something. It means a runtime error: the handle outlives the object it names, so reaching
# through a stale one unwinds rather than quietly failing, and IsInstanceValid is what tells a
# live reference from a dead one before you do.
lifetime := class(node2d):

	var Frames : int = 0
	# The held node's instance id. Kept as the raw handle rather than a node so that this
	# script demonstrates a reference outliving its object rather than merely dropping it.
	var HeldHandle : int = 0

	Ready<override>():void =
		if (Victim := GetChild[0]):
			set HeldHandle = Victim.Handle
			Print("lifetime: holding a child node")

	Update<override>(Delta:float):void =
		set Frames = Frames + 1
		Held := node{Handle := HeldHandle}

		if (Frames = 5):
			Print("lifetime: freeing the held node")
			Held.QueueFree()

		if (Frames >= 4, Frames <= 8):
			if (Held.IsInstanceValid()?):
				Print("lifetime: frame {Frames}, still reachable as {Held.GetName()}")
			else:
				Print("lifetime: frame {Frames}, the held reference is no longer valid")
