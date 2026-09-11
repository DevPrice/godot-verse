using { /Godot.org/Godot }

# The one flat scope the whole project shares means this name is taken for every other file too, so
# an enum meant for one script is named after it.
mover_direction := enum{Right, Left}

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

	# An array, which Godot draws as a list the author adds rows to. `[]float` has a packed container
	# of its own -- PackedFloat64Array -- as do `[]int`, `[]string` and `[]vector2`; `[]logic` does
	# not, and becomes an Array that names what it holds.
	@export
	var Speeds<public>:[]float = array{60.0, 120.0}

	# An enum, which the inspector draws as a dropdown of the enumerators. Godot stores the ordinal,
	# as GDScript and C# do -- including the trap that reordering the enumerators reinterprets every
	# scene already saved.
	@export
	var Direction<public>:mover_direction = mover_direction.Right

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
		if (First := Speeds[0]):
			Print("First speed is {First}")
		if (Sprite := Target?):
			set Sprite.FlipH = true
			set Sprite.Modulate = Tint

	Process<override>(Delta:float):void =
		var Step:float = Delta * Speed
		if (Direction = mover_direction.Left):
			set Step = -Step
		set Position = vector2{X := Position.X + Step, Y := Position.Y}
		Print("Mover x = {Position.X}")
