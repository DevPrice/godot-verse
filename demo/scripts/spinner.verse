using { /Godot.org/Godot }

# The node this is attached to is `Self`, so a script reaches its own node's properties directly
# rather than looking one up by scene path.
spinner := class(sprite2d):

	Ready<override>():void =
		Print("Spinner is ready.")

	Process<override>(Delta:float):void =
		set Rotation = Rotation + Delta
	
	PhysicsProcess<override>(Delta:float):void = {}
