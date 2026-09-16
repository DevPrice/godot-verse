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
# `GetIP[]` is deliberately not exercised here, and not because of this: asking Godot for the IP
# singleton at all segfaults the process at exit, on master and before this fix (by-hand-findings
# B21). NavigationServer2D takes the identical path and does not, so it is what covers it.
singletons := class(node2d):

    # "" when the accessor fails, which is what distinguishes the fix from an absent singleton:
    # the driver compares it against what Godot itself says the object's class is.
    NavigationServer2dClass<public>()<transacts>:string =
        if (Nav := GetNavigationServer2D[]) then Nav.GetClass() else ""

    # The control: a singleton whose registered object is of the class Godot registers it under,
    # which is the other 39 and was never affected.
    EngineClass<public>()<transacts>:string =
        if (Eng := GetEngine[]) then Eng.GetClass() else ""
