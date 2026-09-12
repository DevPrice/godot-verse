using { /Godot.org/Godot }

# R-LANG-1: the class another script's class extends.
#
# Script-to-script inheritance had never been tested, and the mechanism it rests on is the one flat
# `/user@localhost` scope: this file's class is visible to every sibling with nothing written to
# import it, exactly as a library file's functions are. What makes it a *script* rather than a
# library is that it is named after its file and derives from a mirrored Godot class, so it can go on
# a node of its own as well as be derived from.
@global_class
base_entity := class(node2d):

    @export
    var Health<public>:int = 100

    # Overridden by derived_entity. `<abstract>` is not used here on purpose: this class is meant to
    # be attachable too, so it needs a body.
    Describe<public>()<transacts>:string = "entity"

    # Not overridden, so a derived instance calling this proves the base's own code runs.
    DoubledHealth<public>()<transacts>:int = Health * 2
