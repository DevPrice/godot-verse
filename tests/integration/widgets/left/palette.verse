using { /Godot.org/Godot }

# A `@global_class` Resource **inside a module**, which is the pair of facts that broke the
# inspector slot for one.
#
# ClassDB is one flat namespace and a module is deliberately not part of it, so this registers as
# `Palette` -- the file stem, PascalCased, with no module prefix. What the export descriptor for a
# member typed as this used to carry was the *bridge's* name for it, `left/palette`, which
# PascalCased to `Left/Palette`: a name nothing ever registered, and the inspector answering
# "Cannot get class".

@global_class
palette := class(resource):

    @export
    var Swatches<public>:int = 4
