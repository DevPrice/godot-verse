using { /Godot.org/Godot }

# What the debugger cases drive through Play (docs/editor-test-audit.md step 5): the editor layer's
# through Godot's own Debugger panel, and the debug-wire layer's over the remote-debug protocol. Both
# play res://debugger/debug_play.tscn and find every line they arm by its text, so a line added here
# moves no breakpoint -- but rewriting a statement they name rewrites a case. Nothing in the
# integration layer itself uses it.
debug_play := class(node2d):

	Health<public>:int = 7

	# A mirrored math struct reaches Stack Variables as a Vector2 only because the value can name
	# itself: a stopped frame carries no declaration to read its shape from.
	Where<public>:vector2 = vector2{X := 3.0, Y := 4.0}

	var Frames<public>:int = 0

	_Ready<override>():void =
		Print("debug_play ready")

	# Called from the third frame rather than from _Ready: the Verse debugger attaches at the
	# language's first frame, and a main scene's _Ready runs before it.
	#
	# `Start := 1` reports its line once and the two lines after it twice each -- once for the
	# construction or the call, once when its result lands -- which is what a step-over from either
	# must not stop on again, and what a breakpoint on `Again := Helper()` must not stop on twice.
	Walk<public>():void =
		Start := 1
		Spot := vector2{X := 1.0, Y := 2.0}
		Total := Helper()
		Doubled := Total * 2
		Deeper := Outer()
		Again := Helper()
		Print("debug_play walked: {Start} {Doubled} {Deeper} {Again} {Spot.X}")

	# The bare `Inner` emits no op, so it carries no location and a breakpoint on it never fires.
	Helper<public>()<transacts>:int =
		Inner := 21
		Inner

	Outer<public>()<transacts>:int =
		Stepped := Helper() + 1
		Stepped

	_Process<override>(Delta:float):void =
		set Frames += 1
		if (Frames = 3):
			Walk()
		var Sum:int = 0
		profile("debug_play_tag"):
			for (I := 0..9):
				set Sum += I
