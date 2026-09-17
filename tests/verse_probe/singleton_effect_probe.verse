using { /Godot.org/Godot }

# What the singleton accessors cost a caller, and what it took to stop 39 of the 41 costing it.
#
# The question was ergonomics: `if (E := GetEngine[])` around every use of `Engine`, `Input` or `OS`
# is a tax GDScript does not pay, and for 39 of the 41 the failure it guards against cannot happen.
# 39 are registered during `Main::setup`, long before a scene loads; the two that can be absent in a
# game are `EditorInterface` and `GDScriptLanguageProtocol`, which are exactly the two whose class
# carries `"api_type": "editor"` in extension_api.json. Measured against 4.7 by walking
# `Engine.get_singleton_list()` in a headless run.
#
# **A native accessor is still refused**, which is why this was never a specifier to simply drop:
#
#     Verse compiler error V3564: `class engine used as a parameter/result in a native function
#     must also be native.
#
# So a total accessor has to handle the cast's failure itself, and the only thing that can stand in
# the `else` is `Err`, whose result type `false` is uninhabited:
#
#     GetEngine<public>()<reads>:engine =
#         if (S := engine[VhSingletonObject("Engine")]) then S else Err("...")
#
# Three things about that were measured rather than assumed.
#
#  1. `Err` carries the `diverges` effect. `<reads>` and `<computes>` both allow it. `<converges>`
#     does not -- *error 3512: This invocation calls a function that has the 'diverges' effect,
#     which is not allowed by its context* -- and `<converges>` is refused on any non-native
#     function anyway (*error 3565*), so no script body is too narrow to call a total accessor.
#
#  2. A raise is not a failure, and costs more. The call answers `VH_ERR_RUNTIME`; everything the
#     body deferred before the raise is rolled back with the transaction, so a `Print` on the line
#     above never reaches the log; and the raising instance's content scope is terminated, so that
#     node's suspended work dies and the next call into it gets a fresh scope. That is the trade
#     this file's `<decides>` used to buy: an `if` around an impossible condition, against one
#     instance's tasks if the impossible happens.
#
#  3. The mirror's own package takes it, which was the open question -- this probe compiles user
#     packages only, and it cannot write an accessor either, because `VhSingletonObject` is
#     internal to /Godot.org/Godot and naming one from here is *error 3593, invalid access of
#     internal function*. What answers it is running this file against the built mirror: the
#     callstack reads `(/Godot.org/Godot:)GetEngine  GodotClasses.native.verse`. Epic writes the
#     same thing in its own VNI packages -- `_ => Err("Unknown member name")` in
#     `RigidBody.native.verse` and `TransformComponent.native.verse` -- which is why it was worth
#     trying first.
#
#  4. It survives a cook. `Err` is a *module-level* native in /Verse.org/Verse, and a cooked
#     `VNativeProcedure` comes back with a null C++ thunk unless something rebinds it, which is a
#     jump to address 0 with no diagnostic. `RebindVniModuleNatives` covers it: `tests/cooked_probe`
#     over a cook of one class answers `VH_ERR_RUNTIME` and reports the message. The callstack is
#     the one thing an export does not get -- a runtime host has no compiler runtime to build one.
#
# One fact this file cannot carry: a `<computes>` body may not call an accessor at all, because
# `<computes>` is narrower than the `<reads>` every accessor is. The probe reports that refusal as
# `status 4, 0 error(s)`, so the fixture would compile nothing and say nothing.

singleton_effect_probe := class(node2d):

    # A total accessor from the narrowest body that can reach Godot at all, and no failure context.
    InReads<public>()<reads>:int =
        GetEngine().GetProcessFrames()

    # Chained, which is the ergonomic point: the accessor answers an `engine`, not an option over
    # one, so the call site is the single expression a GDScript author would write.
    Chained<public>()<reads>:string =
        GetEngine().GetClass()

    InTransacts<public>()<transacts>:int =
        GetEngine().GetProcessFrames()

    Plain<public>():int =
        GetEngine().GetProcessFrames()

    # The two that stay failable, and the half of the original measurement that still matters: a
    # failure context over one does not force `<transacts>` on the caller, because a
    # `<decides><reads>` callee has nothing to roll back. The tax is syntax, not
    # `dodge-the-creeps.md` wall 8.
    EditorInReads<public>()<reads>:int =
        if (E := GetEditorInterfaceSingleton[]) then 1 else 0

    # And it propagates the ordinary way, so a helper can hand the failure to its own caller
    # instead of answering a default.
    EditorName<public>()<decides><reads>:string =
        E := GetEditorInterfaceSingleton[]
        E.GetClass()
