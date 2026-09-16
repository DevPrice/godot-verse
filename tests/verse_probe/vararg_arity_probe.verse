using { /Godot.org/Godot }

# Can a vararg carry *value* arities beside its `[]variant` one?
#
#   bin/verse_probe.exe <engine>/Engine/Binaries/Win64/verse_host.dll <engine>/Engine \
#       tests/verse_probe/vararg_arity_probe.verse --class vararg_arity_probe
#
# phase-4b-design.md §8 proposes both shapes and stage 6 emitted only the array one, so
# `Call("test", VariantFromInt(1))` is "no overload matches (:[]char,:variant)" -- which is the
# first thing an author reaches for, because it is what GDScript's `call("test", 1)` looks like.
#
# The question is not whether two arities work; `vararg_probe.verse` settled that. It is whether a
# `variant` parameter and a `[]variant` parameter can be told apart in the same overload set, which
# is a different question and one the mirror has a warning about: phase-4b-design.md §15 records
# that `VariantFrom(:logic)` and `VariantFrom(:[]char)` were refused as ambiguous "for no reason any
# reading of overloads-on-parameter-type predicts". That was *module* scope; these are class methods.
#
# And the second question, if the first says yes: how far does it go? Three value arities is what
# covers a Godot `call`, an `emit_signal` and an `rpc` as an author writes them.

vararg_arity_probe := class(object):

    # The two stage 6 emits today.
    Fire<public>(Method:string):int = Method.Length
    Fire<public>(Method:string, Args:[]variant):int = Method.Length + Args.Length

    # The three §8 asks for. If any of these is ambiguous with the `[]variant` one, the whole
    # arrangement is refused at the declaration rather than at a call site.
    Fire<public>(Method:string, A:variant):int = 100
    Fire<public>(Method:string, A:variant, B:variant):int = 200
    Fire<public>(Method:string, A:variant, B:variant, C:variant):int = 300

    # **No fixed prefix, which is where this stops.** One loose arity is legal beside the array one
    # and two is not: a Verse function's parameters *are* its tuple, so `Only(:variant, :variant)`
    # takes `tuple(variant, variant)` -- which **is** `[]variant`, one type rather than two
    # overloads. A single `variant` is not a tuple and stays apart.
    #
    # Adding `Only<public>(Arg1:variant, Arg2:variant):int` below is "The function
    # (...)Only(:variant,:variant) is ambiguous with this definition", and it is the `GDScript.new`
    # finding a second time -- the empty tuple is the empty array, and so is every other arity of
    # one repeated type. Left as a comment rather than written, because one refused declaration
    # takes the whole file with it and the legal half is what has to stay runnable.
    #
    # Where it was actually found: the mirror. `GDScript.new` and the six reference varargs in
    # GodotApi.native.verse have no prefix, and four arities each gave 44 ambiguity errors -- from
    # the *runtime* compiler, after VNI had accepted the same files at build time.
    Only<public>(Args:[]variant):int = Args.Length
    Only<public>(Arg1:variant):int = 100

    # A fixed prefix of more than one, which is `rpc_id(peer, method, ...)` -- the shape that would
    # break if arity alone were doing the work rather than the parameter types.
    Aim<public>(Peer:int, Method:string):int = Peer
    Aim<public>(Peer:int, Method:string, Args:[]variant):int = Peer + Args.Length
    Aim<public>(Peer:int, Method:string, A:variant):int = Peer + 100

    # What the probe can call. Each number says which body won, so a call resolving to the wrong
    # overload reads as a wrong answer rather than as a pass.
    AskNoTail<public>():int = Fire("abc")                                     # 3
    AskArray<public>():int = Fire("abc", array{variant{}, variant{}})         # 5
    AskEmptyArray<public>():int = Fire("abc", array{})                        # 3 -- or ambiguous
    AskOne<public>():int = Fire("abc", variant{})                             # 100
    AskTwo<public>():int = Fire("abc", variant{}, variant{})                  # 200
    AskThree<public>():int = Fire("abc", variant{}, variant{}, variant{})     # 300
    AskAimArray<public>():int = Aim(7, "abc", array{variant{}})               # 8
    AskAimOne<public>():int = Aim(7, "abc", variant{})                        # 107
    AskOnlyArray<public>():int = Only(array{variant{}, variant{}})            # 2
    AskOnlyOne<public>():int = Only(variant{})                                # 100
