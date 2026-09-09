using { /Godot.org/Godot }

# The lifetime experiment. Verse has no null, so a reference to a node Godot has freed has to
# mean something. It means failure: the handle stays, and every accessor is <decides>, so a dead
# reference stops resolving instead of dangling.
lifetime := class(godot_node2d):

    var Frames : int = 0
    var Held : ?godot_node = false

    Ready<override>():void =
        if (Victim := GetChild[0]):
            set Held = option{Victim}
            Print("lifetime: holding a child node")

    Update<override>(Delta:float):void =
        set Frames = Frames + 1

        if (Frames = 5, Victim := Held?):
            Print("lifetime: freeing the held node")
            Victim.QueueFree()

        if (Frames >= 4, Frames <= 8, Victim := Held?):
            if (Name := Victim.GetName[]):
                Print("lifetime: frame {Frames}, still reachable as {Name}")
            else:
                Print("lifetime: frame {Frames}, the held reference no longer resolves")
