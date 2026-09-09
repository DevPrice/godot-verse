using { /Godot.org/Godot }

spinner := module:

	Ready<public>():void =
		Print("Spinner is ready.")

	Update<public>(Delta:float):void =
		if:
			Node := VhGetNode["/root/Main/Mover/Sprite2D"]
			R := VhGetFloat[Node, "rotation"]
		then:
			VhSetFloat(Node, "rotation", R + Delta)
