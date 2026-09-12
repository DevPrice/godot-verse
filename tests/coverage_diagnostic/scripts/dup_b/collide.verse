using { /Godot.org/Godot }

# The other half. `dup_b` carries no marker either, so both files are in the root module and the
# name is claimed twice.
collide := class(node2d):
    Which<public>()<transacts>:string = "b"
