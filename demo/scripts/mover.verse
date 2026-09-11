using { /Godot.org/Godot }

# Named after the file. A script that defines a class named after itself is attached to a node as
# that class: the host instantiates it against the node and Ready/Process are its methods, not
# free functions, so `Self` is the node and no GetNode lookup is needed.
#
# @global_class registers that name with Godot, so `Mover` appears in Create New Node and can be
# named as a type from GDScript -- the same thing C#'s [GlobalClass] does. Godot gets the
# PascalCase spelling; Verse keeps this one.
@global_class
mover := class(node2d):

	# No `var`, so the inspector's value is an initializer: it is applied while the object is still
	# being built, and by the time anything below can read it the value is fixed.
	@export
	Greeting<public>:string = "Verse is running inside Godot, as a class."

	# The range is on the type, so the compiler enforces it: 600.0 does not compile, here or
	# anywhere else this member is assigned. The inspector's slider is built from the same bounds.
	@export
	@export_group("Movement")
	var Speed<public>:type{_X:float where 0.0 <= _X, _X <= 500.0} = 60.0

	# A reference the scene fills in, which is why it is an option: nothing can force a value into
	# an inspector slot, and `sprite2d{}` would be a handle of 0 -- dead before the scene loaded.
	# The type is the whole of the hint, so the picker offers Sprite2D nodes and nothing else.
	@export
	@export_group("Sprite")
	var Target<public>:?sprite2d = false

	# A struct, which the inspector draws with an editor of its own -- a colour picker here, a pair
	# of spinboxes for a vector2. Epic's `editable` refused these outright for not being concrete,
	# which is the concrete reason this bridge declares its own attribute.
	@export
	var Tint<public>:color = color{R := 1.0, G := 1.0, B := 1.0, A := 1.0}

	Ready<override>():void =
		Print(Greeting)
		if (Sprite := Target?):
			set Sprite.FlipH = true
			set Sprite.Modulate = Tint

	Process<override>(Delta:float):void =
		set Position = vector2{X := Position.X + Delta * Speed, Y := Position.Y}
		Print("Mover x = {Position.X}")
