using { /Godot.org/Godot }

# Deliberately broken. Each line names a real Godot member the mirror does not carry under that
# name, one per reason R-SCN-2 permits, and what is under test is the sentence the editor attaches
# to each diagnostic -- not that the names fail, which is obvious, but that failing says where the
# member went.
probe := class(node2d):

    # superseded_by_property: Node2D.get_position is reachable as the property `Position`.
    Superseded()<transacts>:vector2 = GetPosition()

    # virtual: Node._enter_tree is Godot's to call, and only three virtuals are carried (R-NODE-7).
    Virtual()<transacts>:void = EnterTree()

    # property_ambiguous_name: Animation.length cannot be a `var` named Length -- Verse's own Length
    # is in scope where it would be declared -- so Godot's own accessors carry it instead.
    AmbiguousProperty(A:animation)<transacts>:float = A.Length
