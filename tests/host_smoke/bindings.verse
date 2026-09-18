using { /Godot.org/Godot }
using { /Godot.org/Bindings }

# R-INT-7/R-INT-8, with no Godot behind it. The bindings package is not on disk: the driver hands
# its source to `vh_set_bindings` as a string, exactly as the editor will hand over what it
# generated under `.godot/`. So what this file proves is that a *second source package added at
# runtime* participates in the build and is resolvable from a script -- which is the whole of what
# the host has to do for generated bindings.
#
# `rapier_body` and `rapier_joint` are the driver's; look there for their two rosters. Nothing on
# disk declares them, which is the point. Both rosters declare both classes, because every fixture
# in this project compiles as one program -- a file that only compiles under the second roster
# would take the whole suite down under the first.

bindings := class(object):

	# Constructing a binding is the assertion the driver cannot make from Verse: the peer minted
	# has to be the class the binding *binds* -- `RapierBody2D` -- and not the nearest mirrored
	# ancestor the walk would otherwise reach. Getting that wrong hands back a RefCounted where a
	# RapierBody2D was meant, silently, while a working scene looks entirely normal.
	#
	# The *value* is what says which roster is live: 42 under the first, 63 under the second, which
	# multiplies by three instead. A replaced package that was never really replaced still answers
	# 42, and a stale one that failed to retire is a redefinition rather than a wrong number.
	MakeBound<public>()<transacts>:int = rapier_body{}.Push(21)

	# A second class in the same package, so "the package resolved" is not one definition's luck.
	MakeSecond<public>()<transacts>:int = rapier_joint{}.Slack()

	# A binding is an ordinary Verse class, so it passes where its base does and Verse's own
	# downcast reaches it. Nothing in the bridge is asked for either.
	AskIsRefCounted<public>()<transacts>:logic =
		B := rapier_body{}
		if (R := ref_counted[B]) then true else false
