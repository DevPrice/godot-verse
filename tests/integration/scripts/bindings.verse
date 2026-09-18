using { /Godot.org/Godot }
using { /Godot.org/Bindings }

# R-INT-7: a GDScript class reached as a *declared type* rather than through `Callv`.
#
# `mob` is generated. Nothing on disk declares it and nothing in this project wrote it by hand: the
# editor read `Mob` out of Godot's own global class list, described it from `get_script_method_list`
# and handed the Verse to the host as a package of its own. `res://mob.gd` is ordinary GDScript that
# has never heard of Verse.
#
# What that buys over R-INT-2's `Target.Callv("hit", Args).AsInt[]`, which has worked since Phase 4,
# is everything the editor can say: completion, hover, an argument hint, and a wrong name refused at
# the call instead of an empty `variant` at runtime. The call underneath is the same one.

bindings := class(node2d):

	# A binding is a subclass of its mirrored base, so it is a `node2d` wherever one is wanted and
	# `mob[N]` is Verse's own downcast. Nothing in the bridge is asked for either.
	AskCast<public>(N:node2d)<transacts>:int =
		if (M := mob[N]) then M.Hit(21) else -1

	# The method's declared result type is the one GDScript annotated, which is the whole point: a
	# wrong expectation here is a compile error rather than an `AsInt[]` that declines at runtime.
	AskLabel<public>(N:node2d)<transacts>:string =
		if (M := mob[N]) then M.Label() else "?"

	# GDScript's `bool` answers a `logic`, not a `<decides>`: the mirror's 568 predicates are
	# spelled `<decides>` because Godot's own `is_const` and naming say they are tests, and a
	# script's method carries no such claim.
	AskHeavy<public>(N:node2d)<transacts>:logic =
		if (M := mob[N]) then M.Heavy() else false
