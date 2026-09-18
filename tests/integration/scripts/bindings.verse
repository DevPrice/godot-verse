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

	# An object in both directions, at both kinds of class: `mob` is the binding's own type, which no
	# mirror carries, and `node2d` is one the mirror does. Both are `<decides>` -- a class has no
	# value standing for "Godot answered nothing", which is why the mirror's own object-returning
	# methods are failable too (R-TYPE-4). A method the generator cannot type is left out of the
	# binding entirely, so naming these at all is what says it can type an object.
	# The arguments are wrapped because a binding's object parameters are optional: nothing Godot
	# reports about a GDScript method says whether it accepts null, and Godot's answer is that it
	# does. The mirror is the other way round -- it reads `required` out of the API dump, so
	# `AddChild(Child)` is still written plainly.
	AskMate<public>(N:node2d)<transacts>:int =
		if (M := mob[N], Same := M.Mate[option{M}]) then Same.Hit(21) else -1

	AskPlace<public>(N:node2d)<transacts>:string =
		if (M := mob[N], Where := M.Place[option{N}]) then Where.GetName() else "?"

	# And the point of the option: a GDScript method taking an object can be handed nothing.
	AskMateNothing<public>(N:node2d)<transacts>:logic =
		if (M := mob[N], M.Mate[false]) then false else true

	# A binding as a parameter of a *script's* own method, which reaches the host's argument wire
	# rather than the generated package: null is the empty option whatever package declares the
	# class (`by-hand-findings.md` B34).
	AskMaybeMobEmpty<public>(M:?mob)<transacts>:logic =
		if (M?) then false else true

	AskMaybeMobLabel<public>(M:?mob)<transacts>:string =
		if (Held := M?) then Held.Label() else "<none>"

	# What the completion case in test_cases.gd reads. The two shapes differ in what the *snapshot*
	# can say about them, which is what an author sees the instant the popup opens: an archetype
	# names its class in front of its own brace, and a local names nothing until the analysis for
	# that keystroke lands.
	AskFresh<public>()<transacts>:int =
		M := mob{}
		M.Hit(1)

	AskDirect<public>()<transacts>:int =
		mob{}.Hit(1)
