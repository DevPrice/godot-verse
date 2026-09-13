using { /Godot.org/Godot }

# The four Verse facts GodotMath.native.verse is built on, kept so each can be re-run rather than
# re-argued. Every one of them was written down wrong somewhere first.
#
#     bin/verse_probe.exe <engine>/Engine/Binaries/Win64/verse_host.dll <engine>/Engine \
#         tests/verse_probe/math_facts.verse --class math_facts
#
# Run it from `bin/`, where build_host.py leaves tbbmalloc.dll.

# 1. There is no `ToFloat`; `X * 1.0` is the int-to-float bridge, and it works on a *value* rather
#    than only on a literal. `phase-4-gaps.md` G13 said no such conversion existed, which is what
#    parked vector floor/ceil/round for a phase.
Bridge<public>(X:int)<transacts>:float = X * 1.0

# 2. `Floor` is `<decides>` and answers an `int`. So a float-valued floor needs both the bridge and a
#    fallback -- and the fallback is not tidiness: Verse's Floor declines on a non-finite input where
#    Godot's `floor(inf)` is `inf`, so answering the input is what makes the two agree.
TotalFloor<public>(X:float)<transacts>:float = if (V := Floor[X]) then V * 1.0 else X

# 3. Float division is **total**, answering Inf/-Inf/NaN as C does -- but integer division is
#    `Quotient`, which **floors** where C and Godot truncate toward zero.
FloatDiv<public>(A:float, B:float)<transacts>:float = A / B
FlooringQuotient<public>(A:int, B:int)<decides><computes>:int = Quotient[A, B]

# 4. Verse's float `=` is **reflexive for NaN**, unlike IEEE: `X = X` and `X <> X` both say "equal".
#    NaN is distinguishable only by being unordered -- it fails `<=` and `>=` against everything.
NanByEquality<public>(X:float)<decides><computes>:void = not (X = X)
NanByOrdering<public>(X:float)<decides><computes>:void =
    not (X <= 0.0)
    not (X >= 0.0)

math_facts := class(node2d):
    Report<public>()<transacts>:void =
        Print("1. int 7 as float = {Bridge(7)}")
        Print("2. floor(-1.2) = {TotalFloor(-1.2)}, floor(2.7) = {TotalFloor(2.7)}")
        Zero := 0.0
        Print("3. 1/0 = {FloatDiv(1.0, Zero)}, 0/0 = {FloatDiv(Zero, Zero)}")
        if (Q := FlooringQuotient[-3, 2]):
            Print("   Quotient[-3, 2] = {Q}, where Godot's -3 / 2 is -1")
        Nan := FloatDiv(Zero, Zero)
        if (NanByEquality[Nan]) then Print("4. equality detects NaN") else Print("4. equality does NOT detect NaN")
        if (NanByOrdering[Nan]) then Print("   ordering detects NaN") else Print("   ordering does NOT detect NaN")
