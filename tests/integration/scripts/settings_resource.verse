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
