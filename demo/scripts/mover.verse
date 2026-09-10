using { /Godot.org/Godot }
using { /Verse.org/Simulation }

# Named after the file. A script that defines a class named after itself is attached to a node as
# that class: the host instantiates it against the node and Ready/Update are its methods, not
# free functions, so `Self` is the node and no GetNode lookup is needed.
#
# @global_class registers that name with Godot, so `Mover` appears in Create New Node and can be
# named as a type from GDScript -- the same thing C#'s [GlobalClass] does. Godot gets the
# PascalCase spelling; Verse keeps this one.
@global_class
mover := class(node2d):

	@editable
	@clamp_min("0.0")
	@clamp_max("500.0")
	@category("Movement")
	var Speed<public>:float = 60.0

	# No `var`, so the inspector's value is an initializer: it is applied while the object is still
	# being built, and by the time anything below can read it the value is fixed.
	@editable
	Greeting<public>:string = "Verse is running inside Godot, as a class."

	Ready<override>():void =
		Print(Greeting)

	Update<override>(Delta:float):void =
		set Position = vector2{X := Position.X + Delta * Speed, Y := Position.Y}
		Print("Mover x = {Position.X}")
