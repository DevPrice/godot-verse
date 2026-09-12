using { /Godot.org/Godot }

# The other half. Same class name, different module, and both attachable -- see the file beside it
# in `widgets/left/` for what that is proving.
widget := class(node2d):

    Which<public>()<transacts>:string = "right"
