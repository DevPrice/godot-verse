using { /Godot.org/Godot }

Ready():void =
    Print("Verse is running inside Godot.")

Update(Delta:float):void =
    if (Node := GetNode("Mover")):
        if (P := GetVector2(Node, "position")):
            SetVector2(Node, "position", P(0) + Delta * 60.0, P(1))
