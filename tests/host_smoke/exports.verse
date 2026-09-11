using { /Godot.org/Godot }
using { /Verse.org/Simulation }

# Fixture for the export-list checks. Named after the file, like any script: the host looks the
# class up at /user@localhost/exports.
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

    # A range the compiler enforces rather than one the inspector merely draws: the bounds are on
    # the type, so assigning 600.0 anywhere is an error rather than a value the slider happens not
    # to offer. The hint comes from here, and no attribute says it twice.
    @editable
    var Ranged<public>:type{_X:float where 0.0 <= _X, _X <= 500.0} = 60.0

    @editable
    var Steps<public>:type{_X:int where 0 <= _X, _X <= 10} = 3

    # Constrained on one side only, which Godot's range hint has no spelling for: it wants two
    # numbers. Both directions, because the two are not the same code path.
    @editable
    var AtLeast<public>:type{_X:int where 0 <= _X} = 3

    @editable
    var AtMost<public>:type{_X:float where _X <= 1.0} = 0.5

    # A Godot reference the scene may leave unassigned, which is every Godot reference: nothing can
    # force a value into an inspector slot. The option is what lets the member hold that.
    @editable
    var Target<public>:?node2d = false

    # The same reference without one. It compiles -- `node2d{}` is a handle of 0 -- which is
    # exactly why it has to be refused rather than exported as a node that was never alive.
    @editable
    var Held<public>:node2d = node2d{}

    # An option is the shape of an empty inspector slot, and a number has no empty slot.
    @editable
    var Maybe<public>:?float = false

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
@global_class
exports_probe := class(node2d):

    # Exercises the generated property accessors: reading a struct-typed one, writing it whole
    # (a field cannot be written on its own -- see emit_property), and the scalar shape.
    Probe<public>():void =
        if (IsInstanceValid(Self)?):
            Shifted := Position.X + 1.0
            set Position = vector2{X := Shifted, Y := Position.Y}
            set Rotation = Rotation + 1.0

    # PhysicsProcess rather than Ready, which the module-shaped fixture already defines at the top
    # level -- the project shares one flat scope. The blank line below is load bearing: this has
    # to reach the lookup with no comment of its own, because an override with nothing to say is
    # what falls back to what it overrides, and a comment block runs up to the first line that is
    # not one.

    PhysicsProcess<override>(Delta:float):void =
        Probe()
