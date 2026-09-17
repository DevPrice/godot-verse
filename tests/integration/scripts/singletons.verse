using { /Godot.org/Godot }

# The generated singleton accessors, and the one thing about them that used to be silently wrong.
#
# Godot hands `Engine.get_singleton(N)` back as whatever class the object *is*, and for two of the
# 41 that is a class no extension_api.json carries: NavigationServer2D is a GodotNavigationServer2D
# and IP an IPWindows. The mirror has no row for either, so the handle resolved to nothing, the host
# built a bare `vh_object`, and the cast in the accessor declined -- which `<decides>` then reported
# as an ordinary absent singleton. The host now builds a singleton at the class its *name* says,
# which is mirrored by construction.
#
# `GetIP()` is deliberately not exercised here, and not because of this: asking Godot for the IP
# singleton at all segfaults the process at exit, on master and before this fix (by-hand-findings
# B21). NavigationServer2D takes the identical path and does not, so it is what covers it.
singletons := class(node2d):

    # The driver compares this against what Godot itself says the object's class is. Since the
    # accessor became total the same break would *raise* rather than answer "", which is louder
    # and reaches the driver as a call that produced nothing at all.
    NavigationServer2dClass<public>()<transacts>:string =
        GetNavigationServer2D().GetClass()

    # The control: a singleton whose registered object is of the class Godot registers it under,
    # which is the other 39 and was never affected.
    EngineClass<public>()<transacts>:string =
        GetEngine().GetClass()

    # The two accessors that keep `<decides>`, and what that buys. EditorInterface is
    # `"api_type": "editor"`, so a game has no such singleton -- exported, where it was never
    # registered, and run from the editor, where Godot refuses it outside `is_editor_hint()`. The
    # accessor reports the absence and this method goes on to answer, which is the whole difference
    # from GDScript: `EditorInterface` is an identifier only a TOOLS_ENABLED build registers, so an
    # export template refuses the entire script at load rather than the one line that named it.
    EditorInterfaceClass<public>()<transacts>:string =
        if (Editor := GetEditorInterfaceSingleton[]) then Editor.GetClass() else "absent"
