using { /Godot.org/Godot }

# Does reading a `<getter(...)>` var whose getter is `<transacts>` count as a `<transacts>`
# invocation at the *read* site? The refusal fixture suggested not, which would be strange enough
# to be worth isolating.

reads_property := class(node2d):

	MirrorRead<public>()<reads>:vector2 = Position

	MirrorReadX<public>()<reads>:float = Position.X

