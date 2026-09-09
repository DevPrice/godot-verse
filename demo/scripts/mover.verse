using { /Godot.org/Godot }

# Named after the file. A script that defines a class named after itself is attached to a node as
# that class: the host instantiates it against the node and Ready/Update are its methods, not
# free functions, so `Self` is the node and no GetNode lookup is needed.
mover := class(godot_node2d):

	Speed<public>:float = 60.0

	Ready<override>():void =
		Print("Verse is running inside Godot, as a class.")

	Update<override>(Delta:float):void =
		if (P := GetPosition[]):
			SetPosition(vector2{X := P.X + Delta * Speed, Y := P.Y})
			Print("Mover x = {P.X}")
