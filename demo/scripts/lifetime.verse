using { /Godot.org/Godot }

# The lifetime experiment. Verse has no null, so a reference to a node Godot has freed has to
# mean something. It means failure: the handle stays, and every accessor is <decides>, so a dead
# reference stops resolving instead of dangling.
lifetime := class(godot_node2d):

    var Frames : int = 0
    # The held node's instance id. Kept as the raw handle rather than a godot_node so that this
    # script demonstrates a reference outliving its object rather than merely dropping it.
    var HeldHandle : int = 0

    Ready<override>():void =
        if (Victim := GetChild[0]):
            set HeldHandle = Victim.Handle
            Print("lifetime: holding a child node")

    Update<override>(Delta:float):void =
        set Frames = Frames + 1
        Held := godot_node{Handle := HeldHandle}

        if (Frames = 5):
            Print("lifetime: freeing the held node")
            Held.QueueFree()

        if (Frames >= 4, Frames <= 8):
            if (Name := Held.GetName[]):
                Print("lifetime: frame {Frames}, still reachable as {Name}")
            else:
                Print("lifetime: frame {Frames}, the held reference no longer resolves")
