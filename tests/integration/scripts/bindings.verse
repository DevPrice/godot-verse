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

	# R-INT-9's remainder. A GDScript `var` is an ordinary Verse member on the binding, read and
	# written the way the mirror's own 3312 properties are -- its `<getter>`/`<setter>` reach Godot
	# through `Get`/`Set`, and its `= external{}` is legal in this Source package because a member
	# *with accessors* is exempt from the rule that keeps `external{}` inside digests.
	AskSpeed<public>(N:node2d)<transacts>:float =
		if (M := mob[N]) then M.Speed else -1.0

	# Writing and reading back are two calls, not one: a write to Godot defers to the commit, so a
	# read in the same computation still sees what was there before it (R-AUD-1's measured rule).
	AskSetSpeed<public>(N:node2d, To:float)<transacts>:void =
		if (M := mob[N]):
			set M.Speed = To

	# A string is the one shape that cannot be a member: `string` is `[]char`, and Verse asks a
	# container-typed member for indexed accessors -- `TagGetter(:accessor, :int):char` -- which
	# the mirror skips 403 properties rather than write. So it gets the pair Godot would have
	# given it, which is the only place in the package where a name is invented.
	AskTag<public>(N:node2d)<transacts>:string =
		if (M := mob[N]) then M.GetTag() else "?"

	# A constant and a static are not members of the class at all: Verse has no data on a type and
	# no `static`, so both live in the module named after the class.
	AskLimit<public>()<transacts>:int =
		MobStatics.Limit

	AskTitle<public>()<transacts>:string =
		MobStatics.Title

	# The static is dispatched through the *script resource*, which is how GDScript reaches one:
	# there is no ClassDB entry for a script class to call through.
	AskSpawnCost<public>(N:int)<transacts>:int =
		MobStatics.SpawnCost(N)

	# A GDScript enum is a real Verse enum, and `ToInt` is the public name for the int Godot
	# carries it as -- the same spelling the mirror gives its own 793.
	AskEnumValue<public>()<transacts>:int =
		ToInt(mob_state.Done)

	# Across the wire in both directions: the argument packs through `ToInt` and the result comes
	# back through the converter the enum was declared with.
	AskShift<public>(N:node2d)<transacts>:int =
		if (M := mob[N]) then ToInt(M.Shift(mob_state.Busy)) else -1

	AskMode<public>(N:node2d)<transacts>:int =
		if (M := mob[N]) then ToInt(M.Mode) else -1

	AskSetMode<public>(N:node2d)<transacts>:void =
		if (M := mob[N]):
			set M.Mode = mob_state.Done
