using { /Godot.org/Godot }
using { /Godot.org/Bindings }

# Names a *member* of the binding, which is the half B30's own fix does not cover.
#
# Holding a script back keeps its class declared and drops its members, so a file that only named
# the type would compile either way and this would test nothing. `Doubled` exists only if the
# generation that fed this build was able to describe `cycle_probe.gd`, so the build fails with
# `Unknown member` when it was not -- which is what a provisional build's withheld verdict, and the
# corrective build after it, are for.
#
# Taken as a parameter rather than constructed: what is under test is whether the name resolves at
# compile time, and nothing here has to run for that to be answered.
caller := class(node2d):

	Check<public>(InProbe:cycle_probe)<transacts>:int =
		InProbe.Doubled(21)
