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

# A second `@global_class` in one file, which is the shape that broke. The attribute is accepted by
# the compiler -- it is an ordinary Verse attribute and nothing about it is per-file -- and the
# consumer's scanner takes the file-stem class and no other, dropping the attributes above every
# other top-level class with it. So this registers no Godot class name at all.
@global_class
stowaway := class(resource):
    Value<public>:int = 0
