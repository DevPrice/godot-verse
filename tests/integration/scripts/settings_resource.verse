using { /Godot.org/Godot }

# R-EXP-6: a Verse class as a custom Godot Resource.
#
# The first consumer of "a script instance on an owner that is not a node", which is why it is
# after R-NODE-3 rather than before it. Everything below the script instance is keyed on the class
# and on the owner's instance id rather than on nodehood; what had never been exercised is any of
# it against an owner that is not a Node.
#
# `@global_class` so the New Resource dialog lists it -- the dialog reads the global class registry,
# and a Verse resource registers the way a Verse node class does, with no new attribute.

@global_class
settings_resource := class(resource):

    @export
    var Title<public>:string = "untitled"

    @export
    var Rounds<public>:int = 3

    @export
    var Speed<public>:float = 1.5

    # A method on a Resource, which is the half a placeholder cannot answer: in the editor a
    # non-@tool script is a placeholder exactly as GDScript's is, and at runtime it is not.
    Describe<public>()<transacts>:string = "{Title} x{Rounds}"

    Bump<public>()<transacts>:int =
        set Rounds = Rounds + 1
        Rounds

    # B19. `stowaway` below carries `@global_class` and registers **nothing**, because only the
    # class named after its file is ever offered to Godot -- so a slot filtered by `Stowaway` would
    # name a class ClassDB has never heard of, which is what the editor says out loud. The member is
    # exported anyway, filtered by `stowaway`'s nearest mirrored ancestor -- a `Resource` picker,
    # which is what GDScript draws for an inner class, for the same reason.
    @export
    var Stowaway<public>:?stowaway = false

    # A2. The picker A1 widened accepts any `Resource`, so what has to be narrow is the *write*, and
    # only Verse can put a legal value in this slot -- `stowaway` is not the class named after the
    # file, so no Godot object ever carries it as a script and nothing on that side can make one.
    # That is what gives the refusal something to preserve: Stow first, then offer the slot a wrong
    # value, then read back through Verse rather than through the ABI, which would agree with a bad
    # write.
    Stow<public>()<transacts>:void =
        set Stowaway = option{stowaway{Value := 5}}

    StowedValue<public>()<transacts>:int =
        if (S := Stowaway?) then S.Value else -1

    # The other refusal inside the same entry point, and the one host_smoke structurally cannot
    # reach: a *mirrored* member whose handle names an object of the wrong class. There the declared
    # class is real, so the check is `IsA` rather than "not mirrored at all" -- and host_smoke's
    # harness answers no class for any handle, which makes the declared class the fallback and every
    # handle match it. Only a Godot that knows what a handle is can fail this.
    #
    # `gradient` rather than a node: this is a Resource, and a `.tres` assignment is the path A2 is
    # about.
    @export
    var Palette<public>:?gradient = false

    PaletteIsSet<public>()<transacts>:logic =
        if (Palette?) then true else false

    # The fixture for R-EXP-2's second refusal, which had none at all: an option around something
    # that is not an object. Godot's inspector has an empty slot for a node and for a resource and
    # for nothing else, so there is no editor to draw and the member is refused at its declaration.
    #
    # Here rather than on a node script because an option around a *float* mints nothing -- the
    # other refusal, a bare `node2d`, needs a live node as its initializer and would perturb the
    # R-NODE-3 cases that count peers.
    @export
    var Maybe<public>:?float = false

# A second `@global_class` in one file, which is the shape that broke. The attribute is accepted by
# the compiler -- it is an ordinary Verse attribute and nothing about it is per-file -- and the
# consumer's scanner takes the file-stem class and no other, dropping the attributes above every
# other top-level class with it. So this registers no Godot class name at all.
@global_class
stowaway := class(resource):
    Value<public>:int = 0
