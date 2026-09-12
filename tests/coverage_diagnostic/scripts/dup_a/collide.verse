using { /Godot.org/Godot }

# Half of a deliberate collision. `dup_a` carries a marker named `my-stuff.vmodule`, which is not a
# Verse module name -- so the directory is *not* a module and this file stays in the root module,
# where `dup_b/collide.verse` already is. Two diagnostics come out of that: the marker's, and the
# duplicate name's, which is the one that has to teach the author that markers exist at all.
collide := class(node2d):
    Which<public>()<transacts>:string = "a"
