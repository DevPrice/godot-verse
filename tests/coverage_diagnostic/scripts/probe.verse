using { /Godot.org/Godot }

# Deliberately broken. Each line names a real Godot member the mirror does not carry under that
# name, one per reason R-SCN-2 permits, and what is under test is the sentence the editor attaches
# to each diagnostic -- not that the names fail, which is obvious, but that failing says where the
# member went.
probe := class(node2d):

    # superseded_by_property: Node2D.get_position is reachable as the property `Position`.
    Superseded()<transacts>:vector2 = GetPosition()

    # virtual_no_default: since Phase 4 every one of Godot's 1413 virtuals is generated, so
    # `_EnterTree` is an ordinary member of `node` and only the ones whose return type has no
    # writable default are skipped. An unoverridden virtual has to answer *something*, and a
    # parametric container has no literal to be that.
    #
    # An *object* return used to be here and is not skipped any more: Godot says which returns may
    # be null, so the mirror declares those `?class`, and an option has a default where a class has
    # none (`by-hand-findings.md` B37).
    Virtual(S:audio_stream)<transacts>:void = S.GetParameterList()

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
