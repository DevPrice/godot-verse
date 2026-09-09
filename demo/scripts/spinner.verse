using { /Godot.org/Godot }

spinner := module:

	Ready<public>():void =
		Print("Spinner is ready.")

	Update<public>(Delta:float):void =
		if:
			Node := GetNode["/root/Main/Mover/Sprite2D"]
			R := GetFloat[Node, "rotation"]
		then:
			SetFloat(Node, "rotation", R + Delta)
