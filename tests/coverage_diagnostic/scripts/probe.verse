using { /Godot.org/Godot }

# Deliberately broken. Each line names a real Godot member the mirror does not carry under that
# name, one per reason R-SCN-2 permits, and what is under test is the sentence the editor attaches
# to each diagnostic -- not that the names fail, which is obvious, but that failing says where the
# member went.
probe := class(node2d):

    # superseded_by_property: Node2D.get_position is reachable as the property `Position`.
    Superseded()<transacts>:vector2 = GetPosition()

    # There is no `virtual_no_default` case to probe any more, and the absence is the record of why.
    # An unoverridden virtual has to answer *something*, and the two kinds that had nothing to
    # answer with both do now: an object return is an option, which defaults to `false`, and a
    # parametric container has the generated maker beside its converters. All 1413 are in the
    # mirror. The reason still exists in the generator and in the editor's explanation, so a future
    # Godot return type that has no default comes back through it rather than silently.

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

    # A compiler *warning* (glitch 2000), not an error: crosses the ABI as one and is logged by
    # the build as one. Kept last so the line the runner pins it to moves only with an addition.
    Unreachable()<transacts>:int =
        return 1
        2
