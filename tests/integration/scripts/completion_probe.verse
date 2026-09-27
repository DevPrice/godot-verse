using { /Godot.org/Godot }

# The carets test_cases.gd asks probe_complete about. Every one sits in code that compiles, because
# the whole project is one build; the seam splices the caret marker in where the editor would, so a
# caret in the middle of `?ExactMatch` asks what `?E` asks. Each argument is a bare name so that the
# placeholder the caret becomes joins it into one identifier: a caret in front of a literal leaves a
# buffer that does not parse, and a buffer that does not parse has no call to draw a hint for.
completion_probe := class(node2d):

	Held:?node2d = false

	Pressed<public>(Action:string)<transacts>:logic =
		if (GetInputSingleton().IsActionPressed[Action, ?ExactMatch := true]) then true else false

	Reach<public>(Other:node2d, Name:string)<transacts>:int =
		if (Other.GetNode[Name]) then 1 else 0

	Unwrapped<public>()<transacts>:int =
		if (Held?) then 1 else 0
