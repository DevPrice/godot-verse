using { /Godot.org/Godot }

# One half of the pair that says modules did what they were for.
#
# `widgets/left/` carries `left.vmodule`, so this file's definitions live in module `left` and its
# class is `left/widget` to the bridge. `widgets/right/widget.verse` declares a `widget` too, and
# before modules that was the collision the one flat project scope made unfixable -- two files in a
# project could not both be named `widget.verse` however far apart they sat.
#
# Nothing here imports anything: a module reads the root module by ordinary lexical scoping, which
# is why HelperAnswer below resolves with no `using` written.
widget := class(node2d):

    Which<public>()<transacts>:string = "left"

    # From helpers.verse, at the project root rather than in this module.
    RootConstant<public>()<transacts>:int = HelperAnswer

    # A reference to a `@global_class` Resource declared in this same module. The inspector filters
    # a slot by the name *Godot* knows the class as, which is the flat registered one -- and the
    # descriptor used to carry the module-qualified Verse name instead, so the slot named a class
    # ClassDB had never heard of. `by-hand-findings.md` B18 is where that was found.
    @export
    var Skin<public>:?palette = false
