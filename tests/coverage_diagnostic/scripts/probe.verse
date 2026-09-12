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

    # property_renamed: VisualShaderNodeFloatParameter.max cannot be a `var` named Max --
    # /Verse.org/Verse's Max is a function, and data has no signature to be told apart by -- so it is
    # `Maximum`. Four properties across the whole API are in this position.
    RenamedProperty(P:visual_shader_node_float_parameter)<transacts>:float = P.Max

    # ...and the *other* spelling an author might try, which is Godot's own getter. It is superseded
    # by the property, so it points at the same place.
    RenamedPropertyGetter(P:visual_shader_node_float_parameter)<transacts>:float = P.GetMax()

    # superseded_by_free_function: Object.to_string is Verse's own ToString, which is also what
    # string interpolation desugars to.
    FreeFunction(O:object)<transacts>:string = O.ToString()

    # R-SCN-5's other half: the enum is the type now, so the integer that used to work does not.
    ProcessModeFromInt()<transacts>:void = set ProcessMode = 2
