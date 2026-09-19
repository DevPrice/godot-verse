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

	# A Godot value type that is not one of the three the generator names. Thirteen of the sixteen
	# hovered as a local constant, which is the very thing naming `vector2` fixed for one of them.
	Cell<public>():vector2i = vector2i{X := 1, Y := 2}

	# Godot's RID, which is a struct of this bridge's own rather than one of the math types.
	Blank<public>():rid = rid{}

	# The wire type `_Get` and `_Set` are declared in terms of, and the type a script names most
	# often after the classes themselves.
	Nothing<public>():variant = variant{}

	# Verse's `string`, spelled the way the compiler prints it. `char` is a type of
	# /Verse.org/Verse's that the reserved-word list does not carry, so nothing here knows it.
	Letters<public>():[]char = "hover"

	# A parametric type, which the compiler resolves to the function that answers one -- so its
	# name reaches a hover as a function rather than as a class, and is the same Array either way.
	Children<public>():typed_array(node) = MakeNodeArray()

	# An extension method on a value type. It is a module-level definition of `operator'.Length'`,
	# so the compiler reports the file as its owner and the receiver appears only in its signature.
	Reach<public>(To:vector2):float = To.Length()

	# A scalar of GodotMath's, which Godot documents on @GlobalScope beside `randf_range`.
	Soften<public>(X:float):float = Smoothstep(0.0, 1.0, X)

	# The same name spelled both ways, which is what the receiver has to tell apart: Vector2.snapped
	# on this line and @GlobalScope.snapped on the next.
	SnapPoint<public>(At:vector2):vector2 = At.Snapped(vector2{X := 8.0, Y := 8.0})
	SnapScalar<public>(X:float):float = Snapped(X, 8.0)

	# Verse's own, which documents itself with a `@doc` attribute rather than with the comment
	# block every other family here uses. An attribute's text is in no line above the
	# declaration, so re-reading the source -- which is how every description on this side is
	# found -- can never produce it. The host reads it and hands it over (ABI 11.0).
	RootOf<public>(X:float):float = Sqrt(X)

	# One of Verse's own classes, named in a parameter rather than constructed. `event(t)` is
	# what `@export_signal` takes beside `signal(t)`, so a script meets it early.
	Holds<public>(E:event(int)):int = 0

	# An extension method on a Verse type Godot has no page for. `event(t).Emit` is a module-level
	# definition of the Godot package, so it hovered as a "Local Constant" whose type was the whole
	# function type -- until a documentation page was registered for it on demand under its receiver
	# (B40). The receiver is `event`, and the member is `Emit`.
	Fires<public>(E:event(int))<transacts>:void = E.Emit(1)

	# Parameters documented two ways, both of which reach the parameter's own hover because the host
	# reads the comment off the parameter's node rather than by source line (B41): a `#` line above
	# the parameter, and an inline `<# #>` block before it. The line-based reader could not, because
	# a parameter's source line is shared with the ones around it.
	Marks<public>(
		# the first parameter
		First:int,
		<# the second parameter #> Second:int):int = First + Second

	# A method of a *parametric* class. `signal(t)` is a function answering a type, so the walk
	# that records where a mirror definition was written stopped at it and every member of one
	# lost its location to the digest -- a type and an empty box.
	Waits<public>(Clock:timer)<suspends>:tuple() = Clock.Timeout().Await()

	# The prose itself, which reaches Godot as its own doc BBCode: lines join into a paragraph, a
	# blank line separates two, a `span` is code, a **word** is bold and Floor[X] is not a tag.
	#
	# The second paragraph, with the sample the mirror's own comments write after a blank line:
	#
	#     Result := Floor[X]
	#         Nested := 1
	Prose<public>(X:float):int = 0

	<# The other two comment forms, which the reader used to misread: this block read as `>`,
	   the closing line stripped to that and the walk stopped at the line above it. #>
	Blocked<public>():int = 0

	<#>
		An indented comment, whose body is whatever sits indented under the marker.
		The second line is the proof, because only the first survived before.
	Indented<public>():int = 0

	# The three types declared below this class, named here so each has a reference to hover as
	# well as its declaration.
	Helped<public>(H:hover_helper):int = H.Amount
	Reads<public>():hover_reading = hover_reading{}
	Pace<public>():hover_tempo = hover_tempo.Slow

# A second class in the file, which is ordinary Verse: a helper beside the class the script is
# named after. Nothing registers a doc for one, so it keeps the local result -- and the type
# beside it was a colon with nothing after it, because a type has no type to spell.
hover_helper := class:
	Amount<public>:int = 1

# A struct, which reaches a hover as the same VH_LOOKUP_CLASS a class does. The word can only
# come from the declaration, since nothing in the answer tells the two apart.
hover_reading := struct:
	Value<public>:float = 0.0

# An enum of the project's own, which is what the one hard-coded word used to cover alone.
hover_tempo := enum{Slow, Fast}
