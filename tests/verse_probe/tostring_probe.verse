using { /Godot.org/Godot }

# How a Verse class chooses the text Godot's `to_string()` shows -- R-NODE-10's first hook, asked of
# the compiler rather than designed. Five rounds; each answer is a line you can re-run.
#
# 1. A class MEMBER named `ToString` is refused at its own declaration:
#
#      error 3532: The function (/user@localhost/tostring_probe:)ToString() is ambiguous with
#          this definition: function (/Verse.org/Verse:)ToString(:[]char)
#
#    /Verse.org/Verse's ToString is reachable as an extension method, so a member of that name is
#    ambiguous with it -- the rule that already catches `Angle`, `Length` and `event`.
#
# 2. A module-level OVERLOAD `ToString(X:my_class)` is refused too, and by *our* declaration:
#
#      error 3532: ... is ambiguous with this definition:
#          function (/Godot.org/Godot:)ToString(:object) in package Engine/VerseHost
#
#    Every script class derives from `object`, so the mirror's own ToString(:object) -- which is
#    FREE_FUNCTION_REPLACEMENTS mapping Godot's Object.to_string -- covers the whole hierarchy and
#    Verse does not prefer the more specific overload.
#
# 3. The EXTENSION METHOD form compiles clean, and is therefore the spelling:
#
#      (X:my_class).ToString<public>()<transacts>:string = "..."
#
# 4. The two call syntaxes do NOT resolve to the same thing, which is the finding that matters:
#      `Self.ToString()` reaches the extension method       -> "probe with 7"
#      `"{Self}"`        reaches ToString(:object)          -> Godot's own to_string()
#    So interpolation does not see the override *directly*. It arrives there anyway once the bridge
#    implements to_string_func, by going out to Godot and back in.
#
# 5. What the host has to look up, read out of a deliberate duplicate declaration:
#
#      (/user@localhost:)operator'.ToString'(:tostring_probe, :tuple())
#
#    Module-level, not a class member -- which is why it is absent from vh_class_method_list and
#    why InstanceCall cannot reach it unaided. The receiver is parameter 0 and the call's own
#    arguments are a tuple in parameter 1.

tostring_probe<public> := class<unique>(node2d):
    var Count<public>:int = 7

    # Round 4's two call syntaxes, kept so the split can be re-run.
    ViaMethodCall<public>()<transacts>:string = Self.ToString()

    # Reaches Godot's to_string() rather than the extension method below. In the probe that raises,
    # because the probe's handle names no real Godot object; in a game it is the round trip.
    ViaInterpolation<public>()<transacts>:string = "{Self}"

    Run<public>()<transacts>:void =
        Print("ViaMethodCall() = {ViaMethodCall()}")


(X:tostring_probe).ToString<public>()<transacts>:string = "probe with {X.Count}"
