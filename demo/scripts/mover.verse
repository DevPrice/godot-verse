using { /Godot.org/Godot }
using { /Godot.org/Bindings }

# The one flat scope the whole project shares means this name is taken for every other file too, so
# an enum meant for one script is named after it.
mover_direction := enum{Right, Left}

# Named after the file
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

	@export
	var Speeds<public>:[]float = array{60.0, 120.0}

	@export
	var Direction<public>:mover_direction = mover_direction.Right

	@export
	@export_group("Sprite")
	var Target<public>:?sprite2d = false

	@export
	var Tint<public>:color = color{R := 1.0, G := 1.0, B := 1.0, A := 1.0}

	_Ready<override>():void =
		Print(Greeting)
		if (First := Speeds[0]):
			Print("First speed is {First}")
		if (Sprite := Target?):
			set Sprite.FlipH = true
			set Sprite.Modulate = Tint

	_Process<override>(Delta:float):void =
		var Step:float = Delta * Speed
		if (Direction = mover_direction.Left):
			set Step = -Step
		set Position = vector2{X := Position.X + Step, Y := Position.Y}
		Print("Mover x = {Position.X}")
