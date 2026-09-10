using { /Godot.org/Godot }
using { /Verse.org/Simulation }

# Fixture for the export-list checks. Named after the file, like any class-shaped script: the
# host looks the class up at /user@localhost/exports.
exports := class(object):

    # Not a var, so it is exported read-only and writes to it must be refused.
    @editable
    @clamp_min("0.0")
    @clamp_max("500.0")
    @category("Movement")
    Speed<public>:float = 60.0

    @editable
    var Scale<public>:float = 1.5

    @editable
    var Enabled<public>:logic = true

    @editable
    var Label<public>:string = "hello"

    # No attribute, so it must not appear in the export list.
    Hidden<public>:int = 7

    # Reads and writes every exported member from Verse, which is the only thing that can tell a
    # correct write from a plausible one. A var is stored as a reference; a write that lands on
    # the slot instead of through the reference reads back as whatever was written, so the ABI
    # round-trip agrees with itself while the VM disagrees with both -- and only says so here,
    # when the interpreter dereferences what it expects to be a ref.
    Bump<public>():void =
        set Scale = Scale + Speed
        if (Enabled?):
            set Enabled = false
        else:
            set Enabled = true

    BumpLabel<public>():void =
        set Label = Label + "!"

# Exists only to give the symbol lookup a Godot API method to resolve against. Analysis never
# needs a real node behind it, and nothing instantiates this.
exports_probe := class(node2d):

    Probe<public>():void =
        if (IsInstanceValid()?):
            SetPosition(GetPosition())

    # Spike for generated property accessors: proves <getter>/<setter> reach a Godot get/set pair,
    # so `set Probe.Position = V` can replace SetPosition. The struct-typed var needs the second,
    # field-named overload of each -- that is what makes `set Probe.Position.X = 1.0` resolve.
    var Position<public><getter(PositionGetter)><setter(PositionSetter)>:vector2 = external {}

    PositionGetter<public>(Accessor:accessor)<transacts>:vector2 = GetPosition()

    PositionGetter<public>(Accessor:accessor, Field:string)<transacts>:float =
        P := GetPosition()
        if (Field = "X") then P.X else P.Y

    PositionSetter<public>(Accessor:accessor, Value:vector2)<transacts>:void = SetPosition(Value)

    PositionSetter<public>(Accessor:accessor, Field:string, Value:float)<transacts>:void =
        P := GetPosition()
        if (Field = "X"):
            SetPosition(vector2{X := Value, Y := P.Y})
        else:
            SetPosition(vector2{X := P.X, Y := Value})
