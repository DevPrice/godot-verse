using { /Godot.org/Godot }

Ready():void =
    Print("Verse is running inside Godot.")

Update(Delta:float):void =
    if:
        Node := GetNode["/root/Main/Mover"]
        P := GetVector2[Node, "position"]
    then:
        SetVector2(Node, "position", P(0) + Delta * 60.0, P(1))
        Print("Mover x = {P(0)}")
