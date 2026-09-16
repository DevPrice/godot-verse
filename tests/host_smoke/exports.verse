using { /Godot.org/Godot }

# An enum, which the inspector draws as a dropdown. Declared outside the class because a member's
# type has to exist before the member does, and the project shares one flat scope either way.
exports_mode := enum{Idle, Walking, Running}

# Fixture for the export-list checks. Named after the file, like any script: the host looks the
# class up at /user@localhost/exports.
exports := class(object):

    # Not a var, and exported anyway: the attribute has never required one. What `var` decides is
    # *when* a write is allowed, not whether the member is exported. An instance is unsealed from
    # vh_instantiate until the first call into it -- the window a scene's or a .tres's stored values
    # arrive in -- and any exported member may be written then; once sealed, only a `var` may. Both
    # halves are asserted in host_smoke.cpp: "a non-var member can be initialized before the
    # instance seals" and "writing a non-var member is refused once the instance is sealed".
    #
    # Nothing below this line may spell the attribute in prose: the lookup case finds the *first*
    # one in the file and expects it to be the declaration's own.
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

    # The same reference without one. It compiles, and the refusal is about the *type*: an
    # inspector slot for an object can always be left empty, and only an option can hold that.
    #
    # Since R-NODE-3 `node2d{}` is a live node rather than the handle of 0 it used to be, so the
    # old reason for this row -- "a node that was never alive" -- is gone and the rule it tested is
    # not. What it now also covers is the class-defaults instance: the host builds one per exporting
    # class per analysis to read these values off, and *that* one mints nothing, or every keystroke
    # would leak a Node2D (HostScript.cpp, FSuppressMintScope).
    @export
    var Held<public>:node2d = node2d{}

    # An option is the shape of an empty inspector slot, and a number has no empty slot.
    @export
    var Maybe<public>:?float = false

    # A reference to one of the project's own classes. The value a member like this holds is the
    # referenced node's own Verse object, handed over rather than rebuilt. `@global_class` is on
    # the class below and registers nothing, because it is not the one named after this file.
    @export
    var Friend<public>:?exports_probe = false

    # The same without the attribute, which changes nothing: neither class is the one named after
    # this file, and that is the half that decides whether Godot registers a name at all.
    @export
    var Stranger<public>:?exports_unregistered = false

    # A mirrored struct, which Epic's `editable` refused outright for not being concrete and which
    # Godot draws with an editor of its own.
    @export
    var Offset<public>:vector2 = vector2{X := 1.0, Y := 2.0}

    @export
    var Tint<public>:color = color{R := 0.25, G := 0.5, B := 0.75, A := 1.0}

    # Arrays, one per Godot container. A float, an int, a string and a vector2 each have a packed
    # array; `logic` does not, so that one is a plain Array with its element type spelled beside it.
    @export
    var Speeds<public>:[]float = array{1.0, 2.0}

    @export
    var Counts<public>:[]int = array{7, 8, 9}

    @export
    var Names<public>:[]string = array{"a", "bc"}

    @export
    var Flags<public>:[]logic = array{true, false}

    @export
    var Path<public>:[]vector2 = array{vector2{X := 1.0, Y := 2.0}}

    # An enum, which crosses as the ordinal Godot stores and reports its enumerators as the choices
    # the dropdown offers.
    @export
    var Mode<public>:exports_mode = exports_mode.Walking

    # What ReadTarget saw, so the ABI can read back the interpreter's own answer.
    @export
    var TargetSeen<public>:logic = false

    # The same, for a struct: the X the interpreter reads out of whatever was written.
    @export
    var OffsetSeen<public>:float = 0.0

    # And for the arrays: a number only a correctly built array can produce.
    @export
    var ArraysSeen<public>:float = 0.0

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

    # --- ABI v2 dispatch fixtures (R-NODE-6) -------------------------------------
    # One method per argument shape the wire carries, each returning something derived from what it
    # was given, so a single call checks the round trip in both directions.
    AddInts<public>(A:int, B:int):int = A + B
    ScaleFloat<public>(V:float, By:float):float = V * By
    Decorate<public>(Prefix:string, Suffix:string):string = "{Prefix}{Label}{Suffix}"
    Negate<public>(Flag:logic):logic = if (Flag?) then false else true
    CurrentScale<public>():float = Scale
    # <decides>: it runs and declines rather than being absent, which the ABI reports as
    # VH_ERR_FAILED and not VH_ERR_NOT_FOUND -- the two mean different things to a caller.
    NotBelow<public>(N:int, Least:int)<decides>:int =
        N >= Least
        N

    # A named parameter, which the compiler records on the function *type* and not on the
    # parameter's own definition: `Loudly` reaches vh_signature_at as an ordinary definition of type
    # `logic`, and the `?` is recoverable from the signature alone. Asserted there, because a hint
    # that spells this parameter positionally describes a call the compiler refuses.
    Shout<public>(Text:string, ?Loudly:logic = false):void =
        if (Loudly?):
            set Label = Text

    # The call host_smoke builds a completion buffer out of: it replaces the named argument with the
    # editor's placeholder, so a `?` followed by a name nothing declares -- which parses as an
    # option *type* and not as an argument at all -- is proved not to cost the call its signature.
    # That is what the editor asks for the instant a `?` is typed, and the whole named-argument
    # answer rests on it.
    #
    # The placeholder may not be spelled here, in this file, in prose or otherwise: the case that
    # erases the `:` off `Probe`'s `if` finds it with a plain search over the whole buffer and takes
    # the first hit.
    ShoutTwice<public>():void =
        Shout("twice", ?Loudly := true)

    # R-DIAG-2: raises, so the error carries this file and this line. `Raise` itself cannot be the
    # name -- it is already taken in this scope, and Verse reports the collision against whichever
    # definition it analyses second rather than against the new one.
    TouchTarget<public>():void =
        if (Node := Target?):
            Node.QueueFree()

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
            Node._Ready()
            set TargetSeen = true
        else:
            set TargetSeen = false

    # The same oracle for a struct. Reading a field goes through the object's own shape, so a struct
    # built with the wrong emergent type -- or with a field the shape does not carry -- fails here
    # rather than reading back as the numbers that were put in.
    ReadOffset<public>():void =
        set OffsetSeen = Offset.X + Tint.B

    # And for arrays. Indexing is what reads an element through the container's own storage kind, so
    # an array built as the wrong kind -- or holding a cell of the wrong type -- fails here; a `[]
    # vector2` additionally goes on to read a field off an element, which is two layers of
    # construction being checked at once.
    # One per container rather than one for all of them, so a failure says which.
    ReadSpeeds<public>():void =
        set ArraysSeen = -1.0
        if (F := Speeds[1]):
            set ArraysSeen = F

    ReadCounts<public>():void =
        set ArraysSeen = -1.0
        if (N := Counts[0]):
            set ArraysSeen = N * 1.0

    ReadNames<public>():void =
        set ArraysSeen = -1.0
        if (S := Names[1], S = "bc"):
            set ArraysSeen = 100.0

    ReadFlags<public>():void =
        set ArraysSeen = -1.0
        if (L := Flags[0], L?):
            set ArraysSeen = 1000.0

    ReadPath<public>():void =
        set ArraysSeen = -1.0
        if (P := Path[0]):
            set ArraysSeen = P.Y

    # An enumerator is a value of its own, not a number, so comparing one is what says the write put
    # the enumeration's own enumerator there rather than something shaped like an int.
    ReadMode<public>():void =
        set ArraysSeen = -1.0
        if (Mode = exports_mode.Running):
            set ArraysSeen = 2.0
        else if (Mode = exports_mode.Idle):
            set ArraysSeen = 0.0

    # Exists to give completion an anchor for a mirrored Godot enum named as a *type*, not a value
    # -- `node_process_mode.Always` is the spelling `SetProcessMode` takes, and it has to resolve
    # the same way `exports_mode.Walking` above does.
    ReadProcessMode<public>():void =
        if (node_process_mode.Always = node_process_mode.Always):
            set TargetSeen = true

# Exists only to give the symbol lookup a Godot API method to resolve against. Analysis never
# needs a real node behind it, and nothing instantiates this.
@global_class
exports_probe := class(node2d):

    # Exercises the generated property accessors: reading a struct-typed one, writing it whole
    # (a field cannot be written on its own -- see emit_property), and the scalar shape.
    Probe<public>():void =
        if (IsInstanceValid[Self]):
            Shifted := Position.X + 1.0
            set Position = vector2{X := Shifted, Y := Position.Y}
            set Rotation = Rotation + 1.0

    # _PhysicsProcess rather than _Ready, which the module-shaped fixture already defines at the top
    # level -- the project shares one flat scope. The blank line below is load bearing: this has
    # to reach the lookup with no comment of its own, because an override with nothing to say is
    # what falls back to what it overrides, and a comment block runs up to the first line that is
    # not one.

    _PhysicsProcess<override>(Delta:float):void =
        Probe()

# A class the project declares and does not register. Since B19 a reference to one is exported
# anyway, filtered by the nearest mirrored class it derives from -- so what this is here to say is
# that `@global_class` is not the half `exports_probe` is missing.
exports_unregistered := class(node2d) {}

