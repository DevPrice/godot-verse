using { /Godot.org/Godot }

# Fixture for the export-list checks. Named after the file, like any script: the host looks the
# class up at /user@localhost/exports.
exports := class(object):

    # Not a var, so it is exported read-only and writes to it must be refused.
    @export
    @export_group("Movement")
    Speed<public>:float = 60.0

    @export
    var Scale<public>:float = 1.5

    @export
    var Enabled<public>:logic = true

    @export
    var Label<public>:string = "hello"

    # A range the compiler enforces rather than one the inspector merely draws: the bounds are on
    # the type, so assigning 600.0 anywhere is an error rather than a value the slider happens not
    # to offer. The hint comes from here, and no attribute says it twice.
    @export
    var Ranged<public>:type{_X:float where 0.0 <= _X, _X <= 500.0} = 60.0

    @export
    var Steps<public>:type{_X:int where 0 <= _X, _X <= 10} = 3

    # Constrained on one side only, which Godot's range hint has no spelling for: it wants two
    # numbers. Both directions, because the two are not the same code path.
    @export
    var AtLeast<public>:type{_X:int where 0 <= _X} = 3

    @export
    var AtMost<public>:type{_X:float where _X <= 1.0} = 0.5

    @export
    var Exclusive<public>:type{_X:float where _X < 500.0} = 1.0

    @export
    var ExclusiveInt<public>:type{_X:int where 0 < _X} = 1

    # A Godot reference the scene may leave unassigned, which is every Godot reference: nothing can
    # force a value into an inspector slot. The option is what lets the member hold that.
    @export
    var Target<public>:?node2d = false

    # The same reference without one. It compiles -- `node2d{}` is a handle of 0 -- which is
    # exactly why it has to be refused rather than exported as a node that was never alive.
    @export
    var Held<public>:node2d = node2d{}

    # An option is the shape of an empty inspector slot, and a number has no empty slot.
    @export
    var Maybe<public>:?float = false

    # A reference to one of the project's own classes, which Godot knows only because that class
    # registered itself. The value a member like this holds is the referenced node's own Verse
    # object, handed over rather than rebuilt.
    @export
    var Friend<public>:?exports_probe = false

    # The same without the registration. Verse is happy to let a member be typed as any class in
    # the project; the inspector has no way to filter a slot by a class Godot has never heard of.
    @export
    var Stranger<public>:?exports_unregistered = false

    # A mirrored struct, which Epic's `editable` refused outright for not being concrete and which
    # Godot draws with an editor of its own.
    @export
    var Offset<public>:vector2 = vector2{X := 1.0, Y := 2.0}

    @export
    var Tint<public>:color = color{R := 0.25, G := 0.5, B := 0.75, A := 1.0}

    # What ReadTarget saw, so the ABI can read back the interpreter's own answer.
    @export
    var TargetSeen<public>:logic = false

    # The same, for a struct: the X the interpreter reads out of whatever was written.
    @export
    var OffsetSeen<public>:float = 0.0

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

    # The assertion a read/write round-trip cannot make. A reference written in the wrong
    # representation reads back as whatever was put there, so only the interpreter can tell a
    # correct write from a plausible one -- and an option is two shapes rather than one, so the set
    # case and the empty case are separate answers. Calling a method on what comes out dispatches
    # through the object's own shape, which is what proves the payload is an object and not just
    # something shaped like a pointer.
    #
    # The answer goes into a member rather than being returned: the ABI calls void functions only.
    ReadTarget<public>():void =
        if (Node := Target?):
            Node.Ready()
            set TargetSeen = true
        else:
            set TargetSeen = false

    # The same oracle for a struct. Reading a field goes through the object's own shape, so a struct
    # built with the wrong emergent type -- or with a field the shape does not carry -- fails here
    # rather than reading back as the numbers that were put in.
    ReadOffset<public>():void =
        set OffsetSeen = Offset.X + Tint.B

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

# A class the project declares and does not register. It exists to be referred to: a reference to it
# has to be refused, because the inspector filters a slot by a Godot class name and this has none.
exports_unregistered := class(node2d) {}
