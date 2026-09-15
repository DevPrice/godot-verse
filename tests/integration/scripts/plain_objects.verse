using { /Godot.org/Godot }

# R-NODE-3: a Verse class instantiated without being attached to a node.
#
# What host_smoke asserts about this is arithmetic -- one peer per archetype, none per scripted
# node, released when Verse drops it -- because the only thing behind it there is a counter. What
# only a real Godot can say is that the object is a real Godot object: that Godot considers it
# valid, that it reports the class the mirror walked up to, and that handing it to Godot and taking
# it back gives the *same* Verse object rather than a fresh wrapper with the same handle.

# A RefCounted, which is the lifetime that dies with its last holder -- Godot's middle case and the
# one a Verse author gets for free.
counter := class(ref_counted):
    var Count<public>:int = 0

    Bump<public>()<transacts>:int =
        set Count = Count + 1
        Count

plain_objects := class(node2d):

    var Held<public>:?counter = false

    # No node anywhere in this: the archetype is the whole feature.
    MakeAndUse<public>()<transacts>:int =
        C := counter{}
        C.Bump()
        C.Bump()
        C.Bump()

    # The object itself, so Godot can be asked what it thinks of it. Held as well, or the last
    # reference to it would be the one crossing out and the object would die on the way.
    MakeAndKeep<public>()<transacts>:object =
        C := counter{Count := 41}
        set Held = option{C}
        C

    # Identity. `counter[O]` can only succeed if the object arriving from Godot is the one this
    # script minted: a fresh mirror wrapper of the same handle would be a `ref_counted`, and no
    # downcast to a script's own class succeeds against one.
    BumpThrough<public>(O:object)<transacts>:int =
        if (C := counter[O]) then C.Bump() else -1

    ReadHeld<public>()<transacts>:int =
        if (C := Held?) then C.Count else -1

    DropHeld<public>()<transacts>:void =
        set Held = false

    # The other Godot lifetime: a node, which the tree owns once it is parented. Nothing about this
    # is special to Verse -- it is `Node2D.new()` followed by `add_child`, spelled the way Verse
    # spells an archetype.
    MakeChild<public>(Name:string)<transacts>:void =
        N := node2d{}
        N.SetName(Name)
        AddChild(N, false, node_internal_mode.Disabled)
