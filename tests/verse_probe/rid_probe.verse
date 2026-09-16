using { /Godot.org/Godot }

# `rid` as a Verse struct, and the lane bug that made building it worth more than the type safety.
#
#   bin/verse_probe.exe <engine>/Engine/Binaries/Win64/verse_host.dll <engine>/Engine \
#       tests/verse_probe/rid_probe.verse --class rid_probe
#
# Godot's RID was mirrored as a bare `int`, which was wrong twice. It is not a number -- it indexes a
# server's table and nothing arithmetic about it means anything -- and, more urgently, **the lane
# did not round-trip**. A RID used to be packed into the variant's `Ref` lane, beside an object
# handle, on the reasoning that both are ids Godot already owns; but the generated reader was
# `VhToInt`, which reads `I0`, because RID's mirrored type *was* `int`. So RID was the one int-typed
# lane living somewhere else, `VariantRid(...).AsRid[]` answered 0, and so did every mirrored method
# that returns one. AskRoundTrip is that bug, as a test.
#
# The fix was to move the lane rather than to document it. `Ref` means "an id with identity" -- an
# object, or a reference the consumer minted -- and a RID has neither: nothing mints it and nothing
# releases it. In an ordinary integer lane the generated reader is right by default, so the bug
# cannot come back the way it arrived.
#
# `rid` is deliberately **not** one of the math types. A math type crosses as a component array under
# its own variant tag; a RID crosses as a scalar. Routing it through MATH_LAYOUT would change what
# VH_VARIANT_RID means on the wire -- an ABI major, for nothing Godot wants. What it does share is
# the *shape*: a generated `struct<concrete><computes>`, flat, so it can be a `var` property.

rid_probe := class(object):

	# The bug. 7 now; 0 before the lane was fixed.
	AskRoundTrip<public>():int = if (R := VariantRid(rid{Id := 7}).AsRid[]) then R.Id else -1        # 7

	# The reader still checks the tag, so an int-tagged variant is not a RID however much its
	# payload looks like one.
	AskWrongLane<public>():int = if (R := VariantInt(7).AsRid[]) then R.Id else -1        # -1

	# And the other direction: a RID is not an int, so the Int reader declines it too. This is the
	# type safety the struct buys -- as a bare `int` these two were the same value.
	AskNotAnInt<public>():int = if (N := VariantRid(rid{Id := 7}).AsInt[]) then N else -1            # -1

	# A default-constructed one, which is Godot's own "no resource".
	AskEmpty<public>():int = rid{}.Id                                                     # 0

	# `MakeVariant` reaches it, because a `rid` names its own class exactly as a `vector2` does --
	# which is the whole test the discovering converter applies. Only the *encoding* differs from a
	# math struct: a scalar rather than a component array, so it is its own arm in the host.
	AskMakeVariant<public>():int =
		if (V := MakeVariant[rid{Id := 7}], R := V.AsRid[]) then R.Id else -1              # 7

	# And it is a RID to Godot rather than an int, which is what the arm is for: picking the Int
	# lane here would round-trip through Verse perfectly and hand Godot the wrong type.
	AskMakeVariantIsRid<public>():int =
		if (V := MakeVariant[rid{Id := 7}], N := V.AsInt[]) then N else -1                 # -1
