using { /Godot.org/Godot }

# The Verse class `cycle_probe.gd` names. `@global_class` is what registers `Widget` with Godot, and
# registering it is what lets a GDScript annotate a member with it -- which is the first half of the
# loop this project exists to reproduce.
@global_class
widget := class(node2d):

	Spin<public>()<transacts>:void =
		set Rotation = Rotation + 0.1
