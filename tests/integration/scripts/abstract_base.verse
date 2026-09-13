using { /Godot.org/Godot }

# R-NODE-5: a base a script derives from and Godot must not offer to instantiate.
@global_class
abstract_base := class<abstract>(node2d):

    Which<public>()<transacts>:string = "base"
