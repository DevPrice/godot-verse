using { /Godot.org/Godot }

# Phase 4b stage 6's two language questions, asked of the compiler rather than assumed from S-3.
#
#   bin/verse_probe.exe <engine>/Engine/Binaries/Win64/verse_host.dll <engine>/Engine \
#       tests/verse_probe/vararg_probe.verse --class vararg_probe
#
# Godot has 33 vararg entry points and the mirror carries none of them. What a generated one has to
# be is a method with a *fixed* prefix -- `rpc(method, ...)`, `call_group(group, method, ...)` --
# and a variable tail, which on this wire is a `[]variant`. Two things have to be true for that:
#
#   1. a class may carry **both** arities under one name, so `EmitSignal("hit")` and
#      `EmitSignal("hit", array{...})` are the same method. S-3 said a class's methods overload;
#      it did not ask about two arities of the same name, and phase-4b-design.md 15 records that a
#      spike answers the question it was given.
#   2. the prefix and the tail can be **joined**. The generated body has to hand `VhCallValue` one
#      array, and the fixed arguments are packed one at a time while the tail arrives whole.
#
# Both are asked below in ordinary Verse. The native primitives are module-scoped to the mirror, so
# a probe script cannot call `VhCallValue` itself -- what this file can settle is the shape, and the
# integration layer is where the generated article is called for real.

vararg_probe := class(object):

    # Q1: two arities under one name, which is what a generated vararg pair is.
    Fire<public>(Method:string):int = Method.Length
    Fire<public>(Method:string, Args:[]variant):int = Method.Length + Args.Length

    # Q2a: `+` as array concatenation.
    JoinWithPlus<public>(Head:variant, Tail:[]variant)<transacts>:[]variant = array{Head} + Tail

    # Q2b: the named form, in case `+` is not it. `Concatenate` is in the mirror's own list of
    # names Verse claims, which is a hint that it exists and no proof of its shape.
    JoinWithConcatenate<public>(Head:variant, Tail:[]variant)<transacts>:[]variant =
        Concatenate(array{Head}, Tail)

    # Q2c: the fallback that needs no operator at all -- a comprehension over both. Worth knowing
    # because a generated body that cannot use an operator can always use this.
    JoinWithFor<public>(Head:variant, Tail:[]variant)<transacts>:[]variant =
        var Out:[]variant = array{Head}
        for (V : Tail):
            set Out = Out + array{V}
        Out

    # What the probe can call, so the answers are readable without a Godot. No specifier, because
    # `Fire` has none either and so carries `no_rollback` -- a `<transacts>` caller is glitch 3512.
    # 3 + (3 + 2) = 8 says both arities resolved to the right body.
    AskArity<public>():int = Fire("abc") + Fire("abc", array{variant{}, variant{}})

    AskJoinPlus<public>()<transacts>:int = JoinWithPlus(variant{}, array{variant{}, variant{}}).Length
    AskJoinConcatenate<public>()<transacts>:int = JoinWithConcatenate(variant{}, array{variant{}}).Length
    AskJoinFor<public>()<transacts>:int = JoinWithFor(variant{}, array{variant{}, variant{}}).Length
