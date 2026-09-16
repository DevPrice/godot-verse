using { /Godot.org/Godot }

# Two questions about the variant API's shape, and one rule that came out of the second.
#
#   bin/verse_probe.exe <engine>/Engine/Binaries/Win64/verse_host.dll <engine>/Engine \
#       tests/verse_probe/variant_api_probe.verse --class variant_api_probe
#
# **Q1: can a lane reader be spelled on the receiver** -- `V.AsInt[]` rather than `AsInt[V]`, the way
# Epic's own JSON API reads? It has to be an extension method, because `variant` is hand-written in
# Godot.native.verse, the readers are generated, and Verse cannot reopen a class. The part that is
# not obvious from the grammar is whether `<decides>` survives the desugaring: `V.AsInt[]` is a
# failure-call of `operator'.AsInt'(V, ())`, and a failure-call of a *sugared* call is a different
# question from a failure-call of a plain one. **Yes** -- AskReceiverRead below answers 7.
#
# **Q2: can the 38 `VariantFrom<GodotType>` builders be one overloaded `AsVariant(Value)`?**
# phase-4b-design.md 15 says no, recording that `VariantFrom(:logic)` was ambiguous with
# `VariantFrom(:[]char)` "for no reason any reading of overloads-on-parameter-type predicts", and
# concluding that module scope was the problem. Module scope is *not* the problem --
# GodotMath.native.verse overloads `Abs` across nine receiver types and `Length` across seven. So
# the pairs were run one at a time, and there is a rule:
#
#   **An overload set may hold at most one parameter from the emptiable family: `logic`, any
#   option, and any array** -- and `string` is `[]char`, so it is in that family too.
#
# Compiles: int+float, int+string, int+logic, logic+vector2.
# Refused: logic+string, logic+[]int, string+[]int, []int+[]float, logic+?int, ?int+[]int.
#
# Refused at the *definitions*, and refused identically as class methods and as extension methods on
# the receiver -- `(X:logic).ToVar()` beside `(X:string).ToVar()` is the same refusal, so there is no
# spelling that escapes it. The likely mechanism is that `false` is both a `logic` value and the
# empty option, and an option is a 0-or-1 array: one value inhabiting all three families is a call
# site that resolves none of them, which is the same shape of argument as `array{}` having no
# element type. **The mechanism is a conjecture. The ten measurements are not.**
#
# The refusals are comments rather than code because one refused definition takes the whole file
# with it, and because the probe cannot see them anyway: a package-level refusal comes back as
# `status 4, generation 0, 0 error(s)` with no diagnostic, exactly as a refused attribute does --
# and, worth knowing, exactly as a mistyped file path does.
#
# What the rule costs is Q2's answer: `AsVariant(42)` and `AsVariant("hi")` could coexist, but
# `AsVariant(true)` could not join them, and the most common case reading differently from its two
# neighbours is worse than a uniform name per lane. A second reason stands even if the compiler is
# fixed: 38 lanes over 32 Verse types, so six lanes -- StringName, NodePath, RID and two of the
# three integer packings -- have no argument type of their own to be selected by.

# --- the refusals, for re-running one at a time ---------------------------------------------------
#
#   MakeVar(Value:logic)<reads>:variant = VariantFromBool(Value)
#   MakeVar(Value:string)<reads>:variant = VariantFromString(Value)     # refused
#
#   MakeVar(Value:[]int)<reads>:variant = VariantFromPackedInt32Array(Value)
#   MakeVar(Value:[]float)<reads>:variant = VariantFromPackedFloat32Array(Value)    # refused
#
#   MakeVar(Value:?int)<reads>:variant = if (N := Value?) then VariantFromInt(N) else variant{}
#   MakeVar(Value:[]int)<reads>:variant = VariantFromPackedInt32Array(Value)        # refused
#
#   variants := class<computes>:                                        # class scope does not help
#       Of<public>(Value:logic)<reads>:variant = VariantFromBool(Value)
#       Of<public>(Value:string)<reads>:variant = VariantFromString(Value)          # refused

# --- Q1, and the overloads that are legal ---------------------------------------------------------

# The shape the mirror now generates, under its own name so this file does not redeclare one.
(V:variant).AsIntM()<decides><reads>:int = V.AsInt[]

# Legal: neither is emptiable.
MakeVar(Value:int)<reads>:variant = VariantFromInt(Value)
MakeVar(Value:float)<reads>:variant = VariantFromFloat(Value)
MakeVar(Value:vector2)<reads>:variant = VariantFromVector2(Value)

# Legal: exactly one emptiable parameter in the set. Adding `MakeVarS(Value:logic)` is the refusal.
MakeVarS(Value:int)<reads>:variant = VariantFromInt(Value)
MakeVarS(Value:string)<reads>:variant = VariantFromString(Value)

variant_api_probe := class(object):

	# Each answer says which body won, so an overload resolving to the wrong one reads as a wrong
	# number rather than as a pass.
	AskReceiverRead<public>():int =
		V := VariantFromInt(7)
		if (N := V.AsIntM[]) then N else -1                      # 7

	AskGeneratedReader<public>():int =
		V := VariantFromInt(8)
		if (N := V.AsInt[]) then N else -1                       # 8

	# The reader fails rather than raising when the tag is not the one asked for, which is the whole
	# contract and the reason it is <decides> rather than a cast.
	AskWrongLane<public>():int =
		V := VariantFromString("nope")
		if (N := V.AsInt[]) then N else -1                       # -1

	AskMakeInt<public>():int =
		if (N := MakeVar(11).AsInt[]) then N else -1             # 11

	AskMakeVector<public>():int =
		if (V := MakeVar(vector2{X := 3.0, Y := 4.0}).AsVector2[], F := Floor[V.X]) then F else -1   # 3

	AskMakeString<public>():int =
		if (S := MakeVarS("abcd").AsString[]) then S.Length else -1   # 4
