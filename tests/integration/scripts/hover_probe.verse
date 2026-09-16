using { /Godot.org/Godot }

# The fixture the hover cases in test_cases.gd read. Every member here is one shape the script
# editor's tooltip has to get right, and the prose in this comment is one of them: a hover over
# the words `node`, `script` or `label` in a sentence must draw nothing, because the mirror
# spells Godot's classes in lowercase and a comment is made of ordinary English.
#
# tools/probe_hover.py is the instrument this grew out of and is what to run when a tooltip looks
# wrong; these cases are the few shapes worth failing a build over.
hover_probe := class(node2d):

	# An `@export` member of the script's own class, which hovers as a property of it.
	@export
	var Health<public>:int = 3

	# A mirrored virtual, and the one place `:void` is written.
	_Ready<override>():void =
		set Visible = true

	# A parameter, which GDScript calls a Local Variable and which used to read as a constant.
	Grow<public>(Factor:float):float = Factor * 2.0

	# A constant reached through a `...Statics` module: Godot documents it as Vector2.ZERO.
	Corner<public>():vector2 = Vector2Statics.Zero

	# An @GlobalScope utility, reached through the module that holds the ones belonging to no
	# class. Godot documents it on @GlobalScope and that is where a click on it should go.
	Roll<public>():float = GodotStatics.RandfRange(0.0, 1.0)

	# A mirrored enum, which is Node.InternalMode with the word boundaries spelled away.
	Mode<public>():node_internal_mode = node_internal_mode.Disabled

	# A mirrored engine signal, which is a Verse function and a Godot *signal* -- the pair that
	# made every one of the 503 accessors hover as a method Godot does not have.
	Ticks<public>(Clock:timer):signal() = Clock.Timeout()
