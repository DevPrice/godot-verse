using { /Godot.org/Godot }

# R-NODE-7 and R-NODE-8: every Godot virtual, and the one script-level hook that is not one.
#
# Since Phase 4 all 1413 of `extension_api.json`'s virtuals are generated onto the class that
# declares them, with a default body a script says `<override>` over. The name keeps Godot's own
# leading underscore -- `_Ready`, not `Ready` -- and that is measured rather than preferred: eight
# of them collide with a *signal* of the same PascalCase name on the classes an ordinary script
# derives from (`Node.ready`, `CanvasItem.draw`, `Control.gui_input`, `BaseButton.pressed`), and
# the underscore Godot already uses is what tells the two apart.
@global_class
virtuals := class(node2d):

    var Trail<public>:string = ""

    # Node's, and none of them was reachable before: only _ready, _process and _physics_process
    # were carried, hand-written on the native root.
    _EnterTree<override>():void =
        set Trail = Trail + "enter;"

    _ExitTree<override>():void =
        set Trail = Trail + "exit;"

    _Ready<override>():void =
        set Trail = Trail + "ready;"

    # Object's `_notification`, which is in no part of extension_api.json -- it is a hook Godot
    # offers to *scripts* rather than a method it registers in ClassDB -- so it is hand-declared on
    # the native root and reached through the ordinary call path.
    _Notification<override>(What:int):void =
        set Trail = Trail + "n{What};"

    # A value-returning virtual, which is what makes the default body matter: Godot asks and acts on
    # the answer, so an unoverridden one has to mean what Godot's own default means.
    _GetConfigurationWarnings<override>():[]string = array{"deliberate"}

    ReadTrail<public>()<transacts>:string = Trail
    ClearTrail<public>()<transacts>:void = set Trail = ""
