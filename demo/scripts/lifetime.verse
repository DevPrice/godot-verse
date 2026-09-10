using { /Godot.org/Godot }

# The lifetime experiment. Verse has no null, so a reference to a node Godot has freed has to
# mean something. It means a runtime error: the Verse object outlives the Godot one it names, so
# reaching through a stale reference unwinds rather than quietly failing, and IsInstanceValid is
# what tells a live reference from a dead one before you do.
lifetime := class(node2d):

	var Frames : int = 0
	# The held node, kept past the frame that found it. Holding the reference is the point: the
	# Verse object stays perfectly alive while the Godot object under it goes away.
	var Held : ?node = false

	Ready<override>():void =
		if (Victim := GetChild[0]):
			set Held = option{Victim}
			Print("lifetime: holding a child node")

	Process<override>(Delta:float):void =
		set Frames = Frames + 1

		if (Victim := Held?):
			if (Frames = 5):
				Print("lifetime: freeing the held node")
				Victim.QueueFree()

			if (Frames >= 4, Frames <= 8):
				if (IsInstanceValid(Victim)?):
					Print("lifetime: frame {Frames}, still reachable as {Victim.GetName()}")
				else:
					Print("lifetime: frame {Frames}, the held reference is no longer valid")
