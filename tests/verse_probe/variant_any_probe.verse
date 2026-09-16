using { /Godot.org/Godot }

# What `MakeVariant[Value]` reaches, and what it refuses.
#
#   bin/verse_probe.exe <engine>/Engine/Binaries/Win64/verse_host.dll <engine>/Engine \
#       tests/verse_probe/variant_any_probe.verse --class variant_any_probe
#
# One builder against 38 named ones, and the reason it can exist where an overloaded `AsVariant`
# cannot: `any` is a single parameter, so there is nothing to resolve and the rule that an overload
# set may hold at most one emptiable parameter never applies. The type dispatch moves out of the
# compiler and into the host -- `ReadSelfDescribingValue` in HostScript.cpp, which is the converter
# the *debugger* already needed, because a stopped frame carries no declaration either.
#
# `<decides>` because the dispatch can fail, and what fails is exactly what a value cannot say about
# itself. Answering `variant{}` instead would be indistinguishable from a deliberate empty one.
#
# **It is not called `Variant`, and that is not a style choice.** A module-level function in this
# package may not differ from a *type* in this package only by case: `Variant` beside the `variant`
# type makes the whole package fail to load, silently, in the runtime compiler only -- the host build
# succeeds. `Node` beside the mirrored `node` class does the same. Two *functions* differing only by
# case are fine (`VHFROMINT` beside `VhFromInt` compiles), and so is every one of these in an
# ordinary script package. See phase-4b-design.md.

variant_any_probe := class(object):

	# --- what it reaches -------------------------------------------------------------------------
	# Each asserts the *lane* rather than just that something came back: a discovering converter that
	# picked the wrong lane would still answer a variant.
	AskInt<public>():int = if (V := MakeVariant[7], N := V.AsInt[]) then N else -1              # 7

	AskFloat<public>():int =
		if (V := MakeVariant[2.5], F := V.AsFloat[], N := Floor[F * 2.0]) then N else -1        # 5

	AskLogic<public>():int = if (V := MakeVariant[true], L := V.AsBool[], L?) then 1 else 0     # 1

	AskString<public>():int =
		if (V := MakeVariant["abcd"], S := V.AsString[]) then S.Length else -1                  # 4

	# The case that proves the mechanism rather than the plumbing: a math struct is reached by
	# *naming its own class*, which keys the generated layout table. Nothing declared it here.
	AskVector2<public>():int =
		if (V := MakeVariant[vector2{X := 3.0, Y := 4.0}], W := V.AsVector2[], N := Floor[W.Y]) then N else -1   # 4

	AskColor<public>():int =
		if (V := MakeVariant[color{R := 1.0, G := 0.5, B := 0.25, A := 1.0}], C := V.AsColor[], N := Floor[C.R * 4.0]) then N else -1   # 4

	# A Godot object crosses as its handle. `Self` is one, so this is a live object rather than a
	# fabricated handle.
	AskObject<public>():int = if (V := MakeVariant[Self], O := V.AsObject[]) then 1 else 0      # 1

	# A receiver spelling is available and is not built: `(X:any).ToVariant<public>()<decides>` was
	# measured working as a one-line forward, but two names for one operation costs a module-level
	# name, and a module-level name makes every local of that spelling ambiguous rather than
	# shadowing. Left here as the record that it works, not as a gap.

	# --- what it refuses -------------------------------------------------------------------------
	# A failure here is the contract, not a gap: each of these is a value that cannot describe
	# itself, so the alternative to failing is guessing.

	# An empty array cannot say what it holds -- the same fact that makes two array-typed overloads
	# ambiguous, arriving at run time instead of at compile time.
	AskEmptyArray<public>():int = if (V := MakeVariant[array{}]) then 1 else 0                  # 0

	# And a full one still cannot say *which* of Godot's three integer packings was meant, which is
	# why `[]int` keeps a named builder.
	AskIntArray<public>():int = if (V := MakeVariant[array{1, 2, 3}]) then 1 else 0             # 0

	AskMap<public>():int = if (V := MakeVariant[map{1 => 2}]) then 1 else 0                     # 0
