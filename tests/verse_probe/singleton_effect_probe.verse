using { /Godot.org/Godot }

# What the `<decides>` on the 41 singleton accessors costs a caller, and whether it can be dropped.
#
# The question was ergonomics: `if (E := GetEngine[])` around every use of `Engine`, `Input` or `OS`
# is a tax GDScript does not pay, and for 39 of the 41 the failure it guards against cannot happen.
# 39 are registered during `Main::setup`, long before a scene loads; the two that can be absent in a
# game are `EditorInterface` and `GDScriptLanguageProtocol`, which are exactly the two whose class
# carries `"api_type": "editor"` in extension_api.json. Measured against 4.7 by walking
# `Engine.get_singleton_list()` in a headless run.
#
# **It cannot be dropped.** A non-failable accessor has to produce an `engine` without a failure
# context, and only three things can: a cast (failable), an archetype instantiation (`<transacts>`,
# and it mints a *new* Godot object), or a native declared to return the class. The third is what
# the spike asked, and VNI refuses it:
#
#     Verse compiler error V3564: `class engine used as a parameter/result in a native function
#     must also be native.
#
# Making the mirrored classes native means a C++ shadow per class and the whole hierarchy with them,
# so the `<decides>` stays. What it does *not* cost is the effect cascade, which is the reason it
# was worth measuring rather than assuming -- see below.
#
# One fact this file cannot carry: a `<computes>` body may not call an accessor at all, because
# `<computes>` is narrower than the `<reads>` every accessor is. The probe reports that refusal as
# `status 4, 0 error(s)`, so the fixture would compile nothing and say nothing.

singleton_effect_probe := class(node2d):

    # The one that decides the argument. A failure context does *not* force `<transacts>` here: a
    # `<decides><reads>` callee has nothing to roll back, so the narrowest body that can reach Godot
    # at all can still use the accessors. The tax is syntax, not `dodge-the-creeps.md` wall 8.
    InReads<public>()<reads>:int =
        if (E := GetEngine[]) then 1 else 0

    # And it propagates the ordinary way, so a helper can hand the failure to its own caller
    # instead of answering a default.
    Frames<public>()<decides><reads>:int =
        E := GetEngine[]
        E.GetProcessFrames()

    InTransacts<public>()<transacts>:int =
        if (E := GetEngine[]) then 1 else 0

    Plain<public>():int =
        if (E := GetEngine[]) then 1 else 0
