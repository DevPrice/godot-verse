using { /Godot.org/Godot }

# Two @global_class classes with one Godot name, in two different modules. Modules make the *Verse*
# names fine -- `left/widget` and `right/widget` are distinct -- but ClassDB is one flat namespace
# and the module is deliberately not in it, so the registered name collides and the author has to
# resolve it.
@global_class
widget := class(node2d):
    Which<public>()<transacts>:string = "left"
