using { /Godot.org/Godot }

# `CallConst`, the `<reads>` twin of `object.Call` (spec R-INT-9).
#
#   bin/verse_probe.exe <engine>/Engine/Binaries/Win64/verse_host.dll <engine>/Engine \
#       tests/verse_probe/call_const_probe.verse --class call_const_probe
#
# There is no Godot behind the probe, so every call raises on a dead handle and no arm of this file
# answers a *value*. What it asks is whether the declarations exist and typecheck, which is the half
# a host build cannot answer on its own: VNI compiles `host/Verse` at build time against one package
# set, and the runtime compiler re-reads the same files against another. An overload set is exactly
# the kind of thing the second one refuses alone.
#
# **Why six arities and one loose form.** `Call` has that shape and these mirror it, but the rule
# underneath is not obvious: a function's parameters are its tuple, and a tuple of one repeated type
# *is* an array of it -- so `CallConst(Args:[]variant)` and `CallConst()` would be one overload
# without `Method:string` in front of both. Every arity here is distinguishable only because of that
# fixed first parameter.
#
# **What the `<reads>` really means.** Not that `Object::call` is const -- it is not -- but that the
# *method named* is, which nothing checks. `AskWideningIsRefused` is the half of that bargain the
# compiler does enforce: `<reads>` narrows, so the const twin may not be used to launder a
# `<transacts>` call back into a narrow context. Uncomment it and the file stops compiling.

call_const_probe := class(node):

	# Every arity, from a <reads> body. Reaching Godot at all is what a probe cannot do; being
	# *allowed to write this* is the whole question.
	AskZero<public>()<reads>:int = CallConst("tag").AsInt[] or -1

	AskOne<public>()<reads>:int = CallConst("tag", VariantInt(1)).AsInt[] or -1

	AskTwo<public>()<reads>:int = CallConst("tag", VariantInt(1), VariantInt(2)).AsInt[] or -1

	AskThree<public>()<reads>:int =
		CallConst("tag", VariantInt(1), VariantInt(2), VariantInt(3)).AsInt[] or -1

	AskFour<public>()<reads>:int =
		CallConst("tag", VariantInt(1), VariantInt(2), VariantInt(3), VariantInt(4)).AsInt[] or -1

	# The loose form, which is what a generated binding with a vararg reaches for.
	AskLoose<public>()<reads>:int =
		CallConst("tag", array{VariantInt(1), VariantInt(2)}).AsInt[] or -1

	# The `godot_array` spelling, which is what a script holding the result of `MakeArray()` has.
	# `MakeArray` is `<transacts>`, so the array is built by the caller and this one takes it.
	AskV<public>(Args:godot_array)<reads>:int = CallvConst("tag", Args).AsInt[] or -1

	# A `<reads>` helper calling another `<reads>` one, which is the point of the whole exercise:
	# before `CallConst` this could not be written at all, because `Call` is `<transacts>` and
	# `<reads>` narrows.
	AskChained<public>()<reads>:int = AskZero() + AskOne()

	# The other direction still works -- effects are contravariant, so a `<reads>` callee satisfies
	# a `<transacts>` caller.
	AskFromTransacts<public>()<transacts>:int = AskZero()

	# And the mutating verb is still available where it always was.
	AskMutating<public>()<transacts>:int = Call("tag").AsInt[] or -1

# Expected to be refused, kept as the record of what `<reads>` does *not* buy. `Call` is
# `<transacts>`, so a `<reads>` body may not reach it however the result is used:
#
#   error 3512: This invocation calls a function (`(/Godot.org/Godot/object:)Call`) that has the
#   'transacts' effect, which is not allowed by its context.
#
# call_const_reject := class(node):
#     AskWideningIsRefused<public>()<reads>:int = Call("tag").AsInt[] or -1
