using { /Verse.org/Native }

# Godot's math types, as ordinary Verse.
#
# OQ-11 asked how a value-type method crosses, given that every mirrored call rides
# `VhCallValue(Handle, ...)` and a `vector2` has no handle. The answer is that it does not cross at
# all: Verse has type-based extension methods and definable operators, so `velocity.Normalized()`
# and `Position + Velocity * Delta` are Verse code over Verse data, with no ABI and no round trip.
# Godot's own C# does exactly this -- `Vector2.Length()` is `Mathf.Sqrt((X * X) + (Y * Y))` in
# managed code, and `Mathf.cs` redefines the constants rather than asking the engine.
#
# One fact reframes what "exact parity" could mean here: Verse's `float` is 64-bit and Godot's
# `real_t` is 32-bit in a standard build, so a vector2's components are truncated on every crossing
# whatever this file does. Dispatching to Godot would not buy bit-exactness. What it would buy is
# *edge-case semantics*, which is a much narrower prize -- and is why each body below matches
# Godot's own edge case rather than only its formula: `Normalized()` of a zero vector is zero, not
# NaN, because that is what `core/math/vector2.h` does.
#
# **Effects.** `<computes>` where the body is arithmetic, `<reads>` where it is not: Verse's own
# `Sqrt`, `Sin`, `Cos` and `Lerp` are `<reads>` rather than `<computes>`, and an explicit specifier
# *replaces* the default set rather than adding to it, so a `<computes>` function may not call one.
# Both are narrower than `<transacts>`, so either is callable from a Godot callback and from inside
# a failure context.
#
# **All sixteen types are covered, and what is still missing is on the record.** Godot has 367
# methods and 261 operators across them, and C# solved that by writing every one. This writes the
# operators and the methods scene code reaches, plus the `@GlobalScope` scalar math Verse has no name
# for -- which is also R-SCN-3's answer for 92 of Godot's 114 utilities.
#
# What is deliberately *not* written, rather than merely absent: the **general inverse** of a `basis`
# or a `transform3d` (a rotation's inverse is its transpose, which is written and is the case scene
# code has), the Euler and axis-angle **conversions**, and the `projection` constructors -- a
# projection is built by the engine and consumed, not assembled by hand.
#
# `tools/gen_verse_api.py` reads *this file* to find out what is written and records everything else
# as a `math_not_written` skip, which is what makes the absence say so in the editor instead of
# being a name that is simply not there (R-SCN-2, docs/phase-4-design.md 8.3). Reading the file
# rather than maintaining a list is the point: **add a method below and its skip disappears on the
# next generation**, with nothing else to update. 410 of the 628 are skips today, from 585.
#
# The parsing is line-based, so a definition has to start at column zero to be seen -- which every
# one below does, because a Verse module-level definition has nowhere else to start.

# --- scalars -----------------------------------------------------------------
#
# Godot's `@GlobalScope` scalar math, for the names Verse has no counterpart for. Verse's own answer
# everything it *has* an answer to -- `Abs`, `Sqrt`, `Min`, `Max`, `Clamp`, `Lerp`, `Pow`, `Exp`,
# `Ln`, `Mod`, the trig family, and `Sgn`, which is spelled `Sgn` and not `Sign` -- and under
# R-AUD-2 those win. These are what is left, written here rather than dispatched for OQ-11's reason:
# they are arithmetic, and a round trip through the wire buys nothing.
#
# Three Verse facts shape them, and all three were measured with `tests/verse_probe` rather than
# assumed, because the first two had been written down wrong:
#
#   - **`Floor`, `Ceil` and `Round` are `<decides>` and answer an `int`.** Godot's answer a float.
#   - **`X * 1.0` is the int-to-float bridge.** Verse has no `ToFloat`; mixed arithmetic is the
#     conversion, and it works on a value and not only on a literal.
#   - **Float division is total**, answering `Inf`, `-Inf` and `NaN` exactly as Godot's does.
#
# So a float-valued floor is `if (V := Floor[X]) then V * 1.0 else X`, and the `else` is not tidiness:
# Verse's Floor declines on a non-finite input where Godot's `floor(inf)` is `inf`, so answering the
# input is what makes the two agree. That is why these are total and an author never brackets them.

# Verse's own `Pi` is not visible from this package, so the constants are declared here -- which is
# what this file's header already says Godot's C# does: `Mathf.cs` redefines them rather than asking
# the engine. Measured rather than assumed: a bare `Pi` compiles under VNI at build time and is an
# unknown identifier when the *runtime* compiler re-reads this file, which is the compile that counts.
MathPi<public>:float = 3.141592653589793
MathTau<public>:float = 6.283185307179586

# Godot's `floorf`, `ceilf` and `roundf` -- the float-returning halves. Verse's own are the `floori`
# family: an int, and failable.
FloorF<public>(Value:float)<transacts>:float = if (V := Floor[Value]) then V * 1.0 else Value
CeilF<public>(Value:float)<transacts>:float = if (V := Ceil[Value]) then V * 1.0 else Value
RoundF<public>(Value:float)<transacts>:float = if (V := Round[Value]) then V * 1.0 else Value

# `snapped(v, step)` is `floor(v / step + 0.5) * step`, and a zero step is the identity rather than a
# division by zero -- core/math/math_funcs.cpp. The guard is Godot's own, not added here.
Snapped<public>(Value:float, Step:float)<transacts>:float =
    if (Step <> 0.0, Ratio := Value / Step) then FloorF(Ratio + 0.5) * Step else Value

# `is_equal_approx`, which is what makes float comparison usable at all. The tolerance scales with
# magnitude the way Godot's does: a fixed epsilon is worthless at 1e6. `<decides>` rather than
# `logic` so it composes into a condition, which is the same call `IsInstanceValid` makes.
IsEqualApprox<public>(A:float, B:float)<decides><transacts>:void =
    A = B or Abs(A - B) <= Max(0.00001, 0.00001 * Abs(A))

IsZeroApprox<public>(Value:float)<decides><transacts>:void = Abs(Value) < 0.00001

# `inverse_lerp` and `remap`. Total, and deliberately so: a zero range answers `Inf` or `NaN` here
# exactly as it does in Godot, because Verse's float division answers what C's does.
InverseLerp<public>(From:float, To:float, Value:float)<transacts>:float =
    (Value - From) / (To - From)

Remap<public>(Value:float, InFrom:float, InTo:float, OutFrom:float, OutTo:float)<transacts>:float =
    Lerp(OutFrom, OutTo, InverseLerp(InFrom, InTo, Value))

# `move_toward`: step toward a value without overshooting it, which is the whole point of it.
MoveToward<public>(From:float, To:float, Delta:float)<transacts>:float =
    if (Abs(To - From) <= Delta) then To else From + Sgn(To - From) * Delta

# `smoothstep`, clamped then cubed rather than a bare Hermite: flat outside the range is what makes
# it usable as an easing curve, and a zero-width range is a step rather than a NaN.
Smoothstep<public>(From:float, To:float, Value:float)<transacts>:float =
    if (From = To):
        if (Value < From) then 0.0 else 1.0
    else:
        T := Clamp(InverseLerp(From, To, Value), 0.0, 1.0)
        T * T * (3.0 - 2.0 * T)

# `wrapf`, which keeps a value in [Minimum, Maximum) and handles a negative one -- unlike a bare
# modulo, which is the hand-rolled version everyone writes and the bug everyone then has.
WrapF<public>(Value:float, Minimum:float, Maximum:float)<transacts>:float =
    Range := Maximum - Minimum
    if (IsZeroApprox[Range]):
        Minimum
    else:
        Offset := (Value - Minimum) / Range
        Minimum + (Value - Minimum) - Range * FloorF(Offset)

# `pingpong`: bounce rather than wrap, which is what an idle animation wants.
PingPong<public>(Value:float, Length:float)<transacts>:float =
    if (IsZeroApprox[Length]):
        0.0
    else:
        Wrapped := WrapF(Value, 0.0, Length * 2.0)
        if (Wrapped < Length) then Wrapped else Length * 2.0 - Wrapped

# The angle family. `angle_difference` is the shortest way round, and it is what keeps `lerp_angle`
# from taking the long arc -- the bug every hand-rolled angle lerp has.
AngleDifference<public>(From:float, To:float)<transacts>:float = WrapF(To - From, -MathPi, MathPi)

LerpAngle<public>(From:float, To:float, Weight:float)<transacts>:float =
    From + AngleDifference(From, To) * Weight

RotateToward<public>(From:float, To:float, Delta:float)<transacts>:float =
    Difference := AngleDifference(From, To)
    if (Abs(Difference) <= Delta) then To else From + Sgn(Difference) * Delta

# Integer division that truncates **toward zero**, which is what C does and therefore what every
# Godot integer vector's `/` does. Verse's `Quotient` floors instead -- `Quotient[-3, 2]` is -2 where
# Godot's `-3 / 2` is -1 -- so using it directly would have been off by one for exactly the negative
# operands nobody tests. Measured, after the probe printed -2.
TruncatedQuotient<public>(A:int, B:int)<decides><computes>:int =
    Magnitude := Quotient[Abs(A), Abs(B)]
    if (A * B < 0) then -Magnitude else Magnitude

# Only here because forgetting them is the classic Godot bug: every rotation property is radians and
# every inspector field is degrees.
DegToRad<public>(Degrees:float)<transacts>:float = Degrees * MathPi / 180.0
RadToDeg<public>(Radians:float)<transacts>:float = Radians * 180.0 / MathPi

# --- vector2 -----------------------------------------------------------------

operator'+'<public>(L:vector2, R:vector2)<computes>:vector2 = vector2{X := L.X + R.X, Y := L.Y + R.Y}
operator'-'<public>(L:vector2, R:vector2)<computes>:vector2 = vector2{X := L.X - R.X, Y := L.Y - R.Y}
operator'*'<public>(L:vector2, S:float)<computes>:vector2 = vector2{X := L.X * S, Y := L.Y * S}
operator'*'<public>(S:float, R:vector2)<computes>:vector2 = vector2{X := R.X * S, Y := R.Y * S}
prefix'-'<public>(V:vector2)<computes>:vector2 = vector2{X := -V.X, Y := -V.Y}

# Componentwise, which is what Godot's `Vector2 * Vector2` means -- not a dot product.
operator'*'<public>(L:vector2, R:vector2)<computes>:vector2 = vector2{X := L.X * R.X, Y := L.Y * R.Y}

# `<decides>` by choice rather than by necessity, and the distinction matters because the comment
# here used to get it wrong. Verse's float division is **total**: it answers `Inf`, `-Inf` and `NaN`
# exactly as Godot's does, measured. What the specifier buys is the second half of the old argument,
# which stands on its own -- the caller who knows the divisor is non-zero writes the brackets, and
# the one who does not was going to get `inf` and not notice.
operator'/'<public>(L:vector2, S:float)<decides><computes>:vector2 =
    vector2{X := L.X / S, Y := L.Y / S}

(V:vector2).LengthSquared<public>()<computes>:float = V.X * V.X + V.Y * V.Y
(V:vector2).Length<public>()<reads>:float = Sqrt(V.LengthSquared())

# Godot's `normalized()` answers the **zero vector** for a zero-length input rather than NaN
# (core/math/vector2.cpp), and this is total for that reason: a caller that wants to know whether
# there was a direction tests the length, which is the question they actually had.
(V:vector2).Normalized<public>()<reads>:vector2 =
    L := V.Length()
    if (L > 0.0, Scaled := V / L) then Scaled else vector2{}

(V:vector2).Dot<public>(Other:vector2)<computes>:float = V.X * Other.X + V.Y * Other.Y

# Godot's 2D `cross`, which is the z of the 3D one: a scalar, not a vector.
(V:vector2).Cross<public>(Other:vector2)<computes>:float = V.X * Other.Y - V.Y * Other.X

(V:vector2).DistanceTo<public>(Other:vector2)<reads>:float = (Other - V).Length()
(V:vector2).DistanceSquaredTo<public>(Other:vector2)<computes>:float = (Other - V).LengthSquared()

(V:vector2).Lerp<public>(To:vector2, Weight:float)<reads>:vector2 =
    vector2{X := Lerp(V.X, To.X, Weight), Y := Lerp(V.Y, To.Y, Weight)}

(V:vector2).Clamp<public>(Low:vector2, High:vector2)<computes>:vector2 =
    vector2{X := Clamp(V.X, Low.X, High.X), Y := Clamp(V.Y, Low.Y, High.Y)}

(V:vector2).Abs<public>()<computes>:vector2 = vector2{X := Abs(V.X), Y := Abs(V.Y)}
(V:vector2).Sign<public>()<computes>:vector2 = vector2{X := Sgn(V.X), Y := Sgn(V.Y)}

# `floor()`, `ceil()` and `round()` were recorded as unwritable, on the grounds that Verse has no
# int-to-float conversion to bridge its int-returning Floor with Godot's float-returning one. That
# was wrong: `X * 1.0` is the conversion, and the scalar helpers above carry it.
(V:vector2).Floor<public>()<transacts>:vector2 = vector2{X := FloorF(V.X), Y := FloorF(V.Y)}
(V:vector2).Ceil<public>()<transacts>:vector2 = vector2{X := CeilF(V.X), Y := CeilF(V.Y)}
(V:vector2).Round<public>()<transacts>:vector2 = vector2{X := RoundF(V.X), Y := RoundF(V.Y)}

(V:vector2).Snapped<public>(Step:vector2)<transacts>:vector2 =
    vector2{X := Snapped(V.X, Step.X), Y := Snapped(V.Y, Step.Y)}

# Godot's `min`/`max` on vectors are componentwise, which is not what a reader assumes -- these are
# the bounding-box pair, not "the smaller vector".
(V:vector2).Min<public>(Other:vector2)<computes>:vector2 =
    vector2{X := Min(V.X, Other.X), Y := Min(V.Y, Other.Y)}
(V:vector2).Max<public>(Other:vector2)<computes>:vector2 =
    vector2{X := Max(V.X, Other.X), Y := Max(V.Y, Other.Y)}

# `direction_to` is `(other - v).normalized()`, and `limit_length` shortens without lengthening --
# both are what movement code writes instead of normalising by hand and forgetting the zero case.
(V:vector2).DirectionTo<public>(Other:vector2)<reads>:vector2 = (Other - V).Normalized()

(V:vector2).LimitLength<public>(Limit:float)<reads>:vector2 =
    L := V.Length()
    if (L > 0.0, Limit < L, Scaled := V / L) then Scaled * Limit else V

(V:vector2).MoveToward<public>(To:vector2, Delta:float)<reads>:vector2 =
    Difference := To - V
    L := Difference.Length()
    if (L <= Delta, Scaled := Difference / L) then To else V + Difference.Normalized() * Delta

(V:vector2).IsZeroApprox<public>()<decides><transacts>:void =
    IsZeroApprox[V.X]
    IsZeroApprox[V.Y]

(V:vector2).IsEqualApprox<public>(Other:vector2)<decides><transacts>:void =
    IsEqualApprox[V.X, Other.X]
    IsEqualApprox[V.Y, Other.Y]

# `aspect()` is width over height, which is what a viewport rect is asked for.
(V:vector2).Aspect<public>()<transacts>:float = V.X / V.Y

# The reflection family, which is what a bouncing projectile writes. `Normal` is assumed normalised,
# as Godot assumes it -- `bounce` is `-reflect`, and `slide` drops the normal component.
(V:vector2).Slide<public>(Normal:vector2)<computes>:vector2 = V - Normal * V.Dot(Normal)
(V:vector2).Bounce<public>(Normal:vector2)<computes>:vector2 = -(V.Slide(Normal) * 2.0 - V)
(V:vector2).Reflect<public>(Normal:vector2)<computes>:vector2 = V.Slide(Normal) * 2.0 - V
(V:vector2).Project<public>(Onto:vector2)<decides><computes>:vector2 =
    Onto * (V.Dot(Onto) / Onto.LengthSquared())

# `orthogonal()` is the 90-degree rotation, which is cheaper and exact where `Rotated(Pi/2)` is not.
(V:vector2).Orthogonal<public>()<computes>:vector2 = vector2{X := V.Y, Y := -V.X}

(V:vector2).Rotated<public>(Angle:float)<reads>:vector2 =
    C := Cos(Angle)
    S := Sin(Angle)
    vector2{X := V.X * C - V.Y * S, Y := V.X * S + V.Y * C}

# Godot's `angle()` is `atan2(y, x)`, and Verse spells atan2 as the two-argument ArcTan.
(V:vector2).Angle<public>()<reads>:float = ArcTan(V.Y, V.X)
(V:vector2).AngleTo<public>(Other:vector2)<reads>:float = ArcTan(V.Cross(Other), V.Dot(Other))
(V:vector2).AngleToPoint<public>(Other:vector2)<reads>:float = (Other - V).Angle()

# Godot's `Vector2.from_angle`, which is how a direction is made from a heading.
Vector2FromAngle<public>(Angle:float)<reads>:vector2 = vector2{X := Cos(Angle), Y := Sin(Angle)}

# --- vector2i ----------------------------------------------------------------

operator'+'<public>(L:vector2i, R:vector2i)<computes>:vector2i = vector2i{X := L.X + R.X, Y := L.Y + R.Y}
operator'-'<public>(L:vector2i, R:vector2i)<computes>:vector2i = vector2i{X := L.X - R.X, Y := L.Y - R.Y}
operator'*'<public>(L:vector2i, S:int)<computes>:vector2i = vector2i{X := L.X * S, Y := L.Y * S}
prefix'-'<public>(V:vector2i)<computes>:vector2i = vector2i{X := -V.X, Y := -V.Y}

# Componentwise, as Godot's is. Integer division is `Quotient`, not `/`: Verse's `/` on two ints
# answers a `rational` and is failable, where Godot's Vector2i `/` truncates toward zero.
operator'*'<public>(L:vector2i, R:vector2i)<computes>:vector2i = vector2i{X := L.X * R.X, Y := L.Y * R.Y}
operator'/'<public>(L:vector2i, S:int)<decides><computes>:vector2i =
    vector2i{X := TruncatedQuotient[L.X, S], Y := TruncatedQuotient[L.Y, S]}

(V:vector2i).LengthSquared<public>()<computes>:int = V.X * V.X + V.Y * V.Y
# A length is a float even for an integer vector, as Godot's is -- hence the `* 1.0` bridge.
(V:vector2i).Length<public>()<reads>:float = Sqrt(V.LengthSquared() * 1.0)

(V:vector2i).Abs<public>()<computes>:vector2i = vector2i{X := Abs(V.X), Y := Abs(V.Y)}
(V:vector2i).Sign<public>()<computes>:vector2i = vector2i{X := Sgn(V.X), Y := Sgn(V.Y)}
(V:vector2i).Clamp<public>(Low:vector2i, High:vector2i)<computes>:vector2i =
    vector2i{X := Clamp(V.X, Low.X, High.X), Y := Clamp(V.Y, Low.Y, High.Y)}
(V:vector2i).Min<public>(Other:vector2i)<computes>:vector2i =
    vector2i{X := Min(V.X, Other.X), Y := Min(V.Y, Other.Y)}
(V:vector2i).Max<public>(Other:vector2i)<computes>:vector2i =
    vector2i{X := Max(V.X, Other.X), Y := Max(V.Y, Other.Y)}
(V:vector2i).DistanceSquaredTo<public>(Other:vector2i)<computes>:int = (Other - V).LengthSquared()
(V:vector2i).DistanceTo<public>(Other:vector2i)<reads>:float = (Other - V).Length()
(V:vector2i).Aspect<public>()<computes>:float = V.X * 1.0 / (V.Y * 1.0)

# --- vector3 -----------------------------------------------------------------

operator'+'<public>(L:vector3, R:vector3)<computes>:vector3 =
    vector3{X := L.X + R.X, Y := L.Y + R.Y, Z := L.Z + R.Z}
operator'-'<public>(L:vector3, R:vector3)<computes>:vector3 =
    vector3{X := L.X - R.X, Y := L.Y - R.Y, Z := L.Z - R.Z}
operator'*'<public>(L:vector3, S:float)<computes>:vector3 =
    vector3{X := L.X * S, Y := L.Y * S, Z := L.Z * S}
operator'*'<public>(S:float, R:vector3)<computes>:vector3 =
    vector3{X := R.X * S, Y := R.Y * S, Z := R.Z * S}
prefix'-'<public>(V:vector3)<computes>:vector3 = vector3{X := -V.X, Y := -V.Y, Z := -V.Z}

operator'/'<public>(L:vector3, S:float)<decides><computes>:vector3 =
    vector3{X := L.X / S, Y := L.Y / S, Z := L.Z / S}

(V:vector3).LengthSquared<public>()<computes>:float = V.X * V.X + V.Y * V.Y + V.Z * V.Z
(V:vector3).Length<public>()<reads>:float = Sqrt(V.LengthSquared())

(V:vector3).Normalized<public>()<reads>:vector3 =
    L := V.Length()
    if (L > 0.0, Scaled := V / L) then Scaled else vector3{}

(V:vector3).Dot<public>(Other:vector3)<computes>:float = V.X * Other.X + V.Y * Other.Y + V.Z * Other.Z

(V:vector3).Cross<public>(Other:vector3)<computes>:vector3 =
    vector3{
        X := V.Y * Other.Z - V.Z * Other.Y,
        Y := V.Z * Other.X - V.X * Other.Z,
        Z := V.X * Other.Y - V.Y * Other.X}

(V:vector3).DistanceTo<public>(Other:vector3)<reads>:float = (Other - V).Length()
(V:vector3).DistanceSquaredTo<public>(Other:vector3)<computes>:float = (Other - V).LengthSquared()

(V:vector3).Lerp<public>(To:vector3, Weight:float)<reads>:vector3 =
    vector3{X := Lerp(V.X, To.X, Weight), Y := Lerp(V.Y, To.Y, Weight), Z := Lerp(V.Z, To.Z, Weight)}

(V:vector3).Abs<public>()<computes>:vector3 = vector3{X := Abs(V.X), Y := Abs(V.Y), Z := Abs(V.Z)}

(V:vector3).Sign<public>()<computes>:vector3 = vector3{X := Sgn(V.X), Y := Sgn(V.Y), Z := Sgn(V.Z)}

(V:vector3).Clamp<public>(Low:vector3, High:vector3)<computes>:vector3 =
    vector3{
        X := Clamp(V.X, Low.X, High.X),
        Y := Clamp(V.Y, Low.Y, High.Y),
        Z := Clamp(V.Z, Low.Z, High.Z)}

(V:vector3).Floor<public>()<transacts>:vector3 =
    vector3{X := FloorF(V.X), Y := FloorF(V.Y), Z := FloorF(V.Z)}
(V:vector3).Ceil<public>()<transacts>:vector3 =
    vector3{X := CeilF(V.X), Y := CeilF(V.Y), Z := CeilF(V.Z)}
(V:vector3).Round<public>()<transacts>:vector3 =
    vector3{X := RoundF(V.X), Y := RoundF(V.Y), Z := RoundF(V.Z)}

(V:vector3).Snapped<public>(Step:vector3)<transacts>:vector3 =
    vector3{X := Snapped(V.X, Step.X), Y := Snapped(V.Y, Step.Y), Z := Snapped(V.Z, Step.Z)}

(V:vector3).Min<public>(Other:vector3)<computes>:vector3 =
    vector3{X := Min(V.X, Other.X), Y := Min(V.Y, Other.Y), Z := Min(V.Z, Other.Z)}
(V:vector3).Max<public>(Other:vector3)<computes>:vector3 =
    vector3{X := Max(V.X, Other.X), Y := Max(V.Y, Other.Y), Z := Max(V.Z, Other.Z)}

(V:vector3).DirectionTo<public>(Other:vector3)<reads>:vector3 = (Other - V).Normalized()

(V:vector3).LimitLength<public>(Limit:float)<reads>:vector3 =
    L := V.Length()
    if (L > 0.0, Limit < L, Scaled := V / L) then Scaled * Limit else V

(V:vector3).MoveToward<public>(To:vector3, Delta:float)<reads>:vector3 =
    Difference := To - V
    if (Difference.Length() <= Delta) then To else V + Difference.Normalized() * Delta

(V:vector3).IsZeroApprox<public>()<decides><transacts>:void =
    IsZeroApprox[V.X]
    IsZeroApprox[V.Y]
    IsZeroApprox[V.Z]

(V:vector3).IsEqualApprox<public>(Other:vector3)<decides><transacts>:void =
    IsEqualApprox[V.X, Other.X]
    IsEqualApprox[V.Y, Other.Y]
    IsEqualApprox[V.Z, Other.Z]

# The same reflection family vector2 carries, and the same assumption: `Normal` is normalised, as
# Godot assumes it. `AngleTo` uses the cross product's length rather than a bare acos of the dot,
# which is what keeps it accurate for nearly-parallel vectors -- core/math/vector3.cpp.
(V:vector3).Slide<public>(Normal:vector3)<computes>:vector3 = V - Normal * V.Dot(Normal)
(V:vector3).Bounce<public>(Normal:vector3)<computes>:vector3 = -(V.Slide(Normal) * 2.0 - V)
(V:vector3).Reflect<public>(Normal:vector3)<computes>:vector3 = V.Slide(Normal) * 2.0 - V
(V:vector3).Project<public>(Onto:vector3)<decides><computes>:vector3 =
    Onto * (V.Dot(Onto) / Onto.LengthSquared())
(V:vector3).AngleTo<public>(Other:vector3)<reads>:float =
    ArcTan(V.Cross(Other).Length(), V.Dot(Other))

# --- vector3i ----------------------------------------------------------------

operator'+'<public>(L:vector3i, R:vector3i)<computes>:vector3i =
    vector3i{X := L.X + R.X, Y := L.Y + R.Y, Z := L.Z + R.Z}
operator'-'<public>(L:vector3i, R:vector3i)<computes>:vector3i =
    vector3i{X := L.X - R.X, Y := L.Y - R.Y, Z := L.Z - R.Z}
operator'*'<public>(L:vector3i, S:int)<computes>:vector3i =
    vector3i{X := L.X * S, Y := L.Y * S, Z := L.Z * S}
operator'*'<public>(L:vector3i, R:vector3i)<computes>:vector3i =
    vector3i{X := L.X * R.X, Y := L.Y * R.Y, Z := L.Z * R.Z}
operator'/'<public>(L:vector3i, S:int)<decides><computes>:vector3i =
    vector3i{
        X := TruncatedQuotient[L.X, S],
        Y := TruncatedQuotient[L.Y, S],
        Z := TruncatedQuotient[L.Z, S]}
prefix'-'<public>(V:vector3i)<computes>:vector3i = vector3i{X := -V.X, Y := -V.Y, Z := -V.Z}

(V:vector3i).LengthSquared<public>()<computes>:int = V.X * V.X + V.Y * V.Y + V.Z * V.Z
(V:vector3i).Length<public>()<reads>:float = Sqrt(V.LengthSquared() * 1.0)
(V:vector3i).Abs<public>()<computes>:vector3i = vector3i{X := Abs(V.X), Y := Abs(V.Y), Z := Abs(V.Z)}
(V:vector3i).Sign<public>()<computes>:vector3i = vector3i{X := Sgn(V.X), Y := Sgn(V.Y), Z := Sgn(V.Z)}
(V:vector3i).Clamp<public>(Low:vector3i, High:vector3i)<computes>:vector3i =
    vector3i{
        X := Clamp(V.X, Low.X, High.X),
        Y := Clamp(V.Y, Low.Y, High.Y),
        Z := Clamp(V.Z, Low.Z, High.Z)}
(V:vector3i).Min<public>(Other:vector3i)<computes>:vector3i =
    vector3i{X := Min(V.X, Other.X), Y := Min(V.Y, Other.Y), Z := Min(V.Z, Other.Z)}
(V:vector3i).Max<public>(Other:vector3i)<computes>:vector3i =
    vector3i{X := Max(V.X, Other.X), Y := Max(V.Y, Other.Y), Z := Max(V.Z, Other.Z)}
(V:vector3i).DistanceSquaredTo<public>(Other:vector3i)<computes>:int = (Other - V).LengthSquared()
(V:vector3i).DistanceTo<public>(Other:vector3i)<reads>:float = (Other - V).Length()

# --- vector4 -----------------------------------------------------------------

operator'+'<public>(L:vector4, R:vector4)<computes>:vector4 =
    vector4{X := L.X + R.X, Y := L.Y + R.Y, Z := L.Z + R.Z, W := L.W + R.W}
operator'-'<public>(L:vector4, R:vector4)<computes>:vector4 =
    vector4{X := L.X - R.X, Y := L.Y - R.Y, Z := L.Z - R.Z, W := L.W - R.W}
operator'*'<public>(L:vector4, S:float)<computes>:vector4 =
    vector4{X := L.X * S, Y := L.Y * S, Z := L.Z * S, W := L.W * S}
operator'*'<public>(S:float, R:vector4)<computes>:vector4 =
    vector4{X := R.X * S, Y := R.Y * S, Z := R.Z * S, W := R.W * S}
operator'*'<public>(L:vector4, R:vector4)<computes>:vector4 =
    vector4{X := L.X * R.X, Y := L.Y * R.Y, Z := L.Z * R.Z, W := L.W * R.W}
operator'/'<public>(L:vector4, S:float)<decides><computes>:vector4 =
    vector4{X := L.X / S, Y := L.Y / S, Z := L.Z / S, W := L.W / S}
prefix'-'<public>(V:vector4)<computes>:vector4 = vector4{X := -V.X, Y := -V.Y, Z := -V.Z, W := -V.W}

(V:vector4).LengthSquared<public>()<computes>:float = V.X * V.X + V.Y * V.Y + V.Z * V.Z + V.W * V.W
(V:vector4).Length<public>()<reads>:float = Sqrt(V.LengthSquared())
(V:vector4).Normalized<public>()<reads>:vector4 =
    L := V.Length()
    if (L > 0.0, Scaled := V / L) then Scaled else vector4{}
(V:vector4).Dot<public>(Other:vector4)<computes>:float =
    V.X * Other.X + V.Y * Other.Y + V.Z * Other.Z + V.W * Other.W
(V:vector4).Abs<public>()<computes>:vector4 =
    vector4{X := Abs(V.X), Y := Abs(V.Y), Z := Abs(V.Z), W := Abs(V.W)}
(V:vector4).Sign<public>()<computes>:vector4 =
    vector4{X := Sgn(V.X), Y := Sgn(V.Y), Z := Sgn(V.Z), W := Sgn(V.W)}
(V:vector4).Floor<public>()<transacts>:vector4 =
    vector4{X := FloorF(V.X), Y := FloorF(V.Y), Z := FloorF(V.Z), W := FloorF(V.W)}
(V:vector4).Ceil<public>()<transacts>:vector4 =
    vector4{X := CeilF(V.X), Y := CeilF(V.Y), Z := CeilF(V.Z), W := CeilF(V.W)}
(V:vector4).Round<public>()<transacts>:vector4 =
    vector4{X := RoundF(V.X), Y := RoundF(V.Y), Z := RoundF(V.Z), W := RoundF(V.W)}
(V:vector4).Snapped<public>(Step:vector4)<transacts>:vector4 =
    vector4{
        X := Snapped(V.X, Step.X),
        Y := Snapped(V.Y, Step.Y),
        Z := Snapped(V.Z, Step.Z),
        W := Snapped(V.W, Step.W)}
(V:vector4).Clamp<public>(Low:vector4, High:vector4)<computes>:vector4 =
    vector4{
        X := Clamp(V.X, Low.X, High.X),
        Y := Clamp(V.Y, Low.Y, High.Y),
        Z := Clamp(V.Z, Low.Z, High.Z),
        W := Clamp(V.W, Low.W, High.W)}
(V:vector4).Min<public>(Other:vector4)<computes>:vector4 =
    vector4{
        X := Min(V.X, Other.X), Y := Min(V.Y, Other.Y),
        Z := Min(V.Z, Other.Z), W := Min(V.W, Other.W)}
(V:vector4).Max<public>(Other:vector4)<computes>:vector4 =
    vector4{
        X := Max(V.X, Other.X), Y := Max(V.Y, Other.Y),
        Z := Max(V.Z, Other.Z), W := Max(V.W, Other.W)}
(V:vector4).Lerp<public>(To:vector4, Weight:float)<reads>:vector4 =
    vector4{
        X := Lerp(V.X, To.X, Weight), Y := Lerp(V.Y, To.Y, Weight),
        Z := Lerp(V.Z, To.Z, Weight), W := Lerp(V.W, To.W, Weight)}
(V:vector4).DistanceTo<public>(Other:vector4)<reads>:float = (Other - V).Length()
(V:vector4).DistanceSquaredTo<public>(Other:vector4)<computes>:float = (Other - V).LengthSquared()

# --- vector4i ----------------------------------------------------------------

operator'+'<public>(L:vector4i, R:vector4i)<computes>:vector4i =
    vector4i{X := L.X + R.X, Y := L.Y + R.Y, Z := L.Z + R.Z, W := L.W + R.W}
operator'-'<public>(L:vector4i, R:vector4i)<computes>:vector4i =
    vector4i{X := L.X - R.X, Y := L.Y - R.Y, Z := L.Z - R.Z, W := L.W - R.W}
operator'*'<public>(L:vector4i, S:int)<computes>:vector4i =
    vector4i{X := L.X * S, Y := L.Y * S, Z := L.Z * S, W := L.W * S}
operator'/'<public>(L:vector4i, S:int)<decides><computes>:vector4i =
    vector4i{
        X := TruncatedQuotient[L.X, S],
        Y := TruncatedQuotient[L.Y, S],
        Z := TruncatedQuotient[L.Z, S],
        W := TruncatedQuotient[L.W, S]}
prefix'-'<public>(V:vector4i)<computes>:vector4i =
    vector4i{X := -V.X, Y := -V.Y, Z := -V.Z, W := -V.W}

(V:vector4i).LengthSquared<public>()<computes>:int = V.X * V.X + V.Y * V.Y + V.Z * V.Z + V.W * V.W
(V:vector4i).Length<public>()<reads>:float = Sqrt(V.LengthSquared() * 1.0)
(V:vector4i).Abs<public>()<computes>:vector4i =
    vector4i{X := Abs(V.X), Y := Abs(V.Y), Z := Abs(V.Z), W := Abs(V.W)}
(V:vector4i).Sign<public>()<computes>:vector4i =
    vector4i{X := Sgn(V.X), Y := Sgn(V.Y), Z := Sgn(V.Z), W := Sgn(V.W)}
(V:vector4i).Clamp<public>(Low:vector4i, High:vector4i)<computes>:vector4i =
    vector4i{
        X := Clamp(V.X, Low.X, High.X), Y := Clamp(V.Y, Low.Y, High.Y),
        Z := Clamp(V.Z, Low.Z, High.Z), W := Clamp(V.W, Low.W, High.W)}
(V:vector4i).Min<public>(Other:vector4i)<computes>:vector4i =
    vector4i{
        X := Min(V.X, Other.X), Y := Min(V.Y, Other.Y),
        Z := Min(V.Z, Other.Z), W := Min(V.W, Other.W)}
(V:vector4i).Max<public>(Other:vector4i)<computes>:vector4i =
    vector4i{
        X := Max(V.X, Other.X), Y := Max(V.Y, Other.Y),
        Z := Max(V.Z, Other.Z), W := Max(V.W, Other.W)}

# --- color -------------------------------------------------------------------

# Godot's Color arithmetic, which a tween or a flash effect writes. Alpha takes part, as it does in
# Godot: `modulate * 0.5` halves the alpha too.
operator'+'<public>(L:color, R:color)<computes>:color =
    color{R := L.R + R.R, G := L.G + R.G, B := L.B + R.B, A := L.A + R.A}
operator'-'<public>(L:color, R:color)<computes>:color =
    color{R := L.R - R.R, G := L.G - R.G, B := L.B - R.B, A := L.A - R.A}
operator'*'<public>(L:color, S:float)<computes>:color =
    color{R := L.R * S, G := L.G * S, B := L.B * S, A := L.A * S}

(C:color).Lerp<public>(To:color, Weight:float)<reads>:color =
    color{
        R := Lerp(C.R, To.R, Weight),
        G := Lerp(C.G, To.G, Weight),
        B := Lerp(C.B, To.B, Weight),
        A := Lerp(C.A, To.A, Weight)}

operator'*'<public>(S:float, R:color)<computes>:color =
    color{R := R.R * S, G := R.G * S, B := R.B * S, A := R.A * S}

# Componentwise, which is what a shader-style tint is.
operator'*'<public>(L:color, R:color)<computes>:color =
    color{R := L.R * R.R, G := L.G * R.G, B := L.B * R.B, A := L.A * R.A}

# `darkened` scales toward black and `lightened` toward white, and neither touches alpha --
# core/math/color.h. That asymmetry with `*`, which *does* scale alpha, is Godot's and not a slip.
(C:color).Darkened<public>(Amount:float)<computes>:color =
    color{R := C.R * (1.0 - Amount), G := C.G * (1.0 - Amount), B := C.B * (1.0 - Amount), A := C.A}

(C:color).Lightened<public>(Amount:float)<computes>:color =
    color{
        R := C.R + (1.0 - C.R) * Amount,
        G := C.G + (1.0 - C.G) * Amount,
        B := C.B + (1.0 - C.B) * Amount,
        A := C.A}

# Alpha survives inversion too, as Godot's `invert()` leaves it alone.
(C:color).Inverted<public>()<computes>:color =
    color{R := 1.0 - C.R, G := 1.0 - C.G, B := 1.0 - C.B, A := C.A}

# Rec. 709 coefficients, which is what Godot uses to decide whether a colour reads as light or dark.
(C:color).GetLuminance<public>()<computes>:float = 0.2126 * C.R + 0.7152 * C.G + 0.0722 * C.B

(C:color).Clamp<public>(Low:color, High:color)<computes>:color =
    color{
        R := Clamp(C.R, Low.R, High.R),
        G := Clamp(C.G, Low.G, High.G),
        B := Clamp(C.B, Low.B, High.B),
        A := Clamp(C.A, Low.A, High.A)}

(C:color).IsEqualApprox<public>(Other:color)<decides><transacts>:void =
    IsEqualApprox[C.R, Other.R]
    IsEqualApprox[C.G, Other.G]
    IsEqualApprox[C.B, Other.B]
    IsEqualApprox[C.A, Other.A]

# --- rect2 -------------------------------------------------------------------
#
# A rect is a position and a size, and Godot's methods assume a *positive* size: `has_point` on a
# rect with a negative extent answers false rather than doing something clever, which is why `Abs`
# exists and why scene code calls it first. These follow that rather than normalising silently.

(R:rect2).GetEnd<public>()<computes>:vector2 = R.Position + R.Size
(R:rect2).GetCenter<public>()<computes>:vector2 = R.Position + R.Size * 0.5
(R:rect2).GetArea<public>()<computes>:float = R.Size.X * R.Size.Y

(R:rect2).HasPoint<public>(Point:vector2)<decides><computes>:void =
    Point.X >= R.Position.X
    Point.Y >= R.Position.Y
    Point.X < R.Position.X + R.Size.X
    Point.Y < R.Position.Y + R.Size.Y

(R:rect2).HasArea<public>()<decides><computes>:void =
    R.Size.X > 0.0
    R.Size.Y > 0.0

(R:rect2).Intersects<public>(Other:rect2)<decides><computes>:void =
    R.Position.X < Other.Position.X + Other.Size.X
    R.Position.X + R.Size.X > Other.Position.X
    R.Position.Y < Other.Position.Y + Other.Size.Y
    R.Position.Y + R.Size.Y > Other.Position.Y

# Godot's `abs()`: the same rectangle described with a positive size, which is what makes a
# drag-selection rect usable however it was dragged.
(R:rect2).Abs<public>()<computes>:rect2 =
    rect2{
        Position := vector2{
            X := R.Position.X + Min(R.Size.X, 0.0),
            Y := R.Position.Y + Min(R.Size.Y, 0.0)},
        Size := R.Size.Abs()}

(R:rect2).Grow<public>(Amount:float)<computes>:rect2 =
    rect2{
        Position := vector2{X := R.Position.X - Amount, Y := R.Position.Y - Amount},
        Size := vector2{X := R.Size.X + Amount * 2.0, Y := R.Size.Y + Amount * 2.0}}

# The bounding box of both, which is what a scene's extent is accumulated with.
(R:rect2).Merge<public>(Other:rect2)<computes>:rect2 =
    Low := R.Position.Min(Other.Position)
    High := R.GetEnd().Max(Other.GetEnd())
    rect2{Position := Low, Size := High - Low}

(R:rect2).Expand<public>(Point:vector2)<computes>:rect2 =
    Low := R.Position.Min(Point)
    High := R.GetEnd().Max(Point)
    rect2{Position := Low, Size := High - Low}

# --- rect2i ------------------------------------------------------------------

(R:rect2i).GetEnd<public>()<computes>:vector2i = R.Position + R.Size
(R:rect2i).GetCenter<public>()<decides><computes>:vector2i =
    vector2i{
        X := R.Position.X + TruncatedQuotient[R.Size.X, 2],
        Y := R.Position.Y + TruncatedQuotient[R.Size.Y, 2]}
(R:rect2i).GetArea<public>()<computes>:int = R.Size.X * R.Size.Y

(R:rect2i).HasPoint<public>(Point:vector2i)<decides><computes>:void =
    Point.X >= R.Position.X
    Point.Y >= R.Position.Y
    Point.X < R.Position.X + R.Size.X
    Point.Y < R.Position.Y + R.Size.Y

(R:rect2i).HasArea<public>()<decides><computes>:void =
    R.Size.X > 0
    R.Size.Y > 0

(R:rect2i).Intersects<public>(Other:rect2i)<decides><computes>:void =
    R.Position.X < Other.Position.X + Other.Size.X
    R.Position.X + R.Size.X > Other.Position.X
    R.Position.Y < Other.Position.Y + Other.Size.Y
    R.Position.Y + R.Size.Y > Other.Position.Y

(R:rect2i).Abs<public>()<computes>:rect2i =
    rect2i{
        Position := vector2i{
            X := R.Position.X + Min(R.Size.X, 0),
            Y := R.Position.Y + Min(R.Size.Y, 0)},
        Size := R.Size.Abs()}

(R:rect2i).Merge<public>(Other:rect2i)<computes>:rect2i =
    Low := R.Position.Min(Other.Position)
    High := R.GetEnd().Max(Other.GetEnd())
    rect2i{Position := Low, Size := High - Low}

# --- the @GlobalScope scalars Verse has no name for --------------------------
#
# The second half of R-SCN-3's utility question (G11). Verse's stdlib answers most of Godot's 114,
# and R-AUD-2 gives those Verse's spelling. These are the ones it has no answer to *and* which are
# arithmetic rather than engine behaviour -- so they are written here, for OQ-11's reason, rather
# than dispatched. The ones that genuinely are engine behaviour are in GodotApi.native.verse.

# The *inverse* hyperbolics. Verse already has `Sinh`, `Cosh` and `Tanh` -- which the compiler said
# when these were first written as duplicates -- so those keep Verse's spelling under R-AUD-2 and
# only the inverses are written, as the textbook identities over `Ln` and `Sqrt`.
Asinh<public>(X:float)<reads>:float = Ln(X + Sqrt(X * X + 1.0))
# `acosh` is real only for X >= 1 and `atanh` only for |X| < 1, so both decline outside their domain
# rather than answering NaN -- which is the one place these deliberately differ from Godot, and it
# differs the way `operator'/'` already does.
Acosh<public>(X:float)<decides><reads>:float =
    X >= 1.0
    Ln(X + Sqrt(X * X - 1.0))
Atanh<public>(X:float)<decides><reads>:float =
    Abs(X) < 1.0
    Ln((1.0 + X) / (1.0 - X)) * 0.5

# Decibels. The constants are 20/ln(10) and its inverse, which is what Godot's own are.
LinearToDb<public>(Linear:float)<decides><reads>:float =
    Linear > 0.0
    Ln(Linear) * 8.685889638065035
DbToLinear<public>(Db:float)<reads>:float = Exp(Db * 0.11512925464970229)

# Godot's `ease`, which is the curve an inspector's "ease" field means. The branches are its own
# (core/math/math_funcs.cpp): a positive curve is in-or-out depending on whether it is below one, a
# negative one is in-out, and zero is zero rather than an identity -- which surprises people, so it
# is written rather than smoothed over.
Ease<public>(X:float, Curve:float)<reads>:float =
    Clamped := Clamp(X, 0.0, 1.0)
    if (Curve > 0.0):
        if (Curve < 1.0) then 1.0 - Pow(1.0 - Clamped, 1.0 / Curve) else Pow(Clamped, Curve)
    else if (Curve < 0.0):
        if (Clamped < 0.5):
            Pow(Clamped * 2.0, -Curve) * 0.5
        else:
            (1.0 - Pow(1.0 - (Clamped - 0.5) * 2.0, -Curve)) * 0.5 + 0.5
    else:
        0.0

# Catmull-Rom through four points, which is what an animation curve samples between keys.
#
# **Written as bindings rather than one long sum, and that is not a style choice.** Verse does not
# continue an expression onto a line beginning with an operator -- it takes the first line and drops
# the rest, *silently*, with no diagnostic. The first draft of this function was
# `0.5 * ((From * 2.0)` followed by three `+ ...` lines and it answered 0.0 for every input. Keep
# every arithmetic expression in this file on one line or bound a term at a time.
CubicInterpolate<public>(From:float, To:float, Pre:float, Post:float, Weight:float)<computes>:float =
    W2 := Weight * Weight
    W3 := W2 * Weight
    Linear := (-Pre + To) * Weight
    Square := (2.0 * Pre - 5.0 * From + 4.0 * To - Post) * W2
    Cube := (-Pre + 3.0 * From - 3.0 * To + Post) * W3
    0.5 * (From * 2.0 + Linear + Square + Cube)

# A cubic Bezier and its derivative -- the tangent, which is what orients something moving along
# the curve.
BezierInterpolate<public>(Start:float, Control1:float, Control2:float, End:float, T:float)<computes>:float =
    Omt := 1.0 - T
    Omt2 := Omt * Omt
    Start * (Omt2 * Omt) + Control1 * Omt2 * T * 3.0 + Control2 * Omt * (T * T) * 3.0 + End * (T * T * T)

BezierDerivative<public>(Start:float, Control1:float, Control2:float, End:float, T:float)<computes>:float =
    Omt := 1.0 - T
    First := (Control1 - Start) * 3.0 * (Omt * Omt)
    Second := (Control2 - Control1) * 6.0 * Omt * T
    Third := (End - Control2) * 3.0 * (T * T)
    First + Second + Third

# `is_nan` cannot use the usual trick. **Verse's float `=` is reflexive even for NaN** -- both
# `X = X` and `X <> X` answer "equal", unlike IEEE and unlike C -- so `not (X = X)` never succeeds
# and a first draft of this quietly reported that nothing was ever NaN. What does distinguish NaN is
# that it is unordered: it fails `<=` and `>=` against everything, itself included.
# `is_inf` rests on the magnitude exceeding every finite double.
IsNan<public>(X:float)<decides><computes>:void =
    not (X <= 0.0)
    not (X >= 0.0)
IsInf<public>(X:float)<decides><computes>:void =
    not IsNan[X]
    Abs(X) > 1.7976931348623157e308
IsFinite<public>(X:float)<decides><computes>:void =
    not IsNan[X]
    not IsInf[X]

# --- plane -------------------------------------------------------------------
#
# A plane is a unit normal and a distance along it from the origin, so `D` is signed and the sign
# convention is Godot's: a point is "over" the plane when `Normal.Dot(P) - D` is positive.

(P:plane).Normalized<public>()<reads>:plane =
    L := P.Normal.Length()
    if (L > 0.0, N := P.Normal / L) then plane{Normal := N, D := P.D / L} else plane{}

# Signed: negative behind the plane, which is what makes it usable as a half-space test rather than
# only as a distance.
(P:plane).DistanceTo<public>(Point:vector3)<computes>:float = P.Normal.Dot(Point) - P.D

(P:plane).HasPoint<public>(Point:vector3)<decides><transacts>:void = IsZeroApprox[P.DistanceTo(Point)]
(P:plane).IsPointOver<public>(Point:vector3)<decides><computes>:void = P.DistanceTo(Point) > 0.0

# The point on the plane nearest to the origin, and the projection of an arbitrary point onto it.
(P:plane).GetCenter<public>()<computes>:vector3 = P.Normal * P.D
(P:plane).Project<public>(Point:vector3)<computes>:vector3 = Point - P.Normal * P.DistanceTo(Point)

prefix'-'<public>(P:plane)<computes>:plane = plane{Normal := -P.Normal, D := -P.D}

# --- quaternion --------------------------------------------------------------

operator'+'<public>(L:quaternion, R:quaternion)<computes>:quaternion =
    quaternion{X := L.X + R.X, Y := L.Y + R.Y, Z := L.Z + R.Z, W := L.W + R.W}
operator'-'<public>(L:quaternion, R:quaternion)<computes>:quaternion =
    quaternion{X := L.X - R.X, Y := L.Y - R.Y, Z := L.Z - R.Z, W := L.W - R.W}
operator'*'<public>(L:quaternion, S:float)<computes>:quaternion =
    quaternion{X := L.X * S, Y := L.Y * S, Z := L.Z * S, W := L.W * S}
prefix'-'<public>(Q:quaternion)<computes>:quaternion =
    quaternion{X := -Q.X, Y := -Q.Y, Z := -Q.Z, W := -Q.W}

# The Hamilton product, component for component as core/math/quaternion.h writes it. Composition is
# **not** commutative and the order is Godot's: `A * B` applies B first.
operator'*'<public>(L:quaternion, R:quaternion)<computes>:quaternion =
    Nx := L.W * R.X + L.X * R.W + L.Y * R.Z - L.Z * R.Y
    Ny := L.W * R.Y + L.Y * R.W + L.Z * R.X - L.X * R.Z
    Nz := L.W * R.Z + L.Z * R.W + L.X * R.Y - L.Y * R.X
    Nw := L.W * R.W - L.X * R.X - L.Y * R.Y - L.Z * R.Z
    quaternion{X := Nx, Y := Ny, Z := Nz, W := Nw}

(Q:quaternion).LengthSquared<public>()<computes>:float =
    Q.X * Q.X + Q.Y * Q.Y + Q.Z * Q.Z + Q.W * Q.W
(Q:quaternion).Length<public>()<reads>:float = Sqrt(Q.LengthSquared())

(Q:quaternion).Normalized<public>()<reads>:quaternion =
    L := Q.Length()
    if (L > 0.0) then Q * (1.0 / L) else quaternion{}

(Q:quaternion).Dot<public>(Other:quaternion)<computes>:float =
    Q.X * Other.X + Q.Y * Other.Y + Q.Z * Other.Z + Q.W * Other.W

# The conjugate, which is the inverse for a *unit* quaternion -- and Godot's `inverse()` asserts
# normalisation rather than dividing, so this matches it exactly.
(Q:quaternion).Inverse<public>()<computes>:quaternion =
    quaternion{X := -Q.X, Y := -Q.Y, Z := -Q.Z, W := -Q.W}

# Rotating a vector: `v + 2 * (u x (u x v) + w * (u x v))`, with u the vector part. Godot spells it
# this way rather than as a matrix because it is fewer operations for one vector.
(Q:quaternion).Xform<public>(V:vector3)<computes>:vector3 =
    U := vector3{X := Q.X, Y := Q.Y, Z := Q.Z}
    Uv := U.Cross(V)
    V + (Uv * Q.W + U.Cross(Uv)) * 2.0

operator'*'<public>(Q:quaternion, V:vector3)<computes>:vector3 = Q.Xform(V)

# Shortest-arc interpolation. The negation when the dot is negative is what keeps it short -- a
# quaternion and its negation are the same rotation, and without this a slerp past 180 degrees spins
# the long way. Near-parallel falls back to a linear blend, because sin(omega) goes to zero.
(Q:quaternion).Slerp<public>(To:quaternion, Weight:float)<reads>:quaternion =
    RawCos := Q.Dot(To)
    Target := if (RawCos < 0.0) then -To else To
    Cosom := Abs(RawCos)
    if (1.0 - Cosom > 0.00001):
        Omega := ArcCos(Cosom)
        Sinom := Sin(Omega)
        Scale0 := Sin((1.0 - Weight) * Omega) / Sinom
        Scale1 := Sin(Weight * Omega) / Sinom
        Q * Scale0 + Target * Scale1
    else:
        Q * (1.0 - Weight) + Target * Weight

(Q:quaternion).IsEqualApprox<public>(Other:quaternion)<decides><transacts>:void =
    IsEqualApprox[Q.X, Other.X]
    IsEqualApprox[Q.Y, Other.Y]
    IsEqualApprox[Q.Z, Other.Z]
    IsEqualApprox[Q.W, Other.W]

# --- aabb --------------------------------------------------------------------
#
# The 3D rect2, and it carries the same assumption: a negative extent means the box is empty rather
# than inside out, so `Abs` is what a dragged volume calls first.

(B:aabb).GetEnd<public>()<computes>:vector3 = B.Position + B.Size
(B:aabb).GetCenter<public>()<computes>:vector3 = B.Position + B.Size * 0.5
(B:aabb).GetVolume<public>()<computes>:float = B.Size.X * B.Size.Y * B.Size.Z

(B:aabb).HasVolume<public>()<decides><computes>:void =
    B.Size.X > 0.0
    B.Size.Y > 0.0
    B.Size.Z > 0.0

(B:aabb).HasPoint<public>(Point:vector3)<decides><computes>:void =
    Point.X >= B.Position.X
    Point.Y >= B.Position.Y
    Point.Z >= B.Position.Z
    Point.X < B.Position.X + B.Size.X
    Point.Y < B.Position.Y + B.Size.Y
    Point.Z < B.Position.Z + B.Size.Z

(B:aabb).Intersects<public>(Other:aabb)<decides><computes>:void =
    B.Position.X < Other.Position.X + Other.Size.X
    B.Position.X + B.Size.X > Other.Position.X
    B.Position.Y < Other.Position.Y + Other.Size.Y
    B.Position.Y + B.Size.Y > Other.Position.Y
    B.Position.Z < Other.Position.Z + Other.Size.Z
    B.Position.Z + B.Size.Z > Other.Position.Z

(B:aabb).Abs<public>()<computes>:aabb =
    Low := vector3{
        X := B.Position.X + Min(B.Size.X, 0.0),
        Y := B.Position.Y + Min(B.Size.Y, 0.0),
        Z := B.Position.Z + Min(B.Size.Z, 0.0)}
    aabb{Position := Low, Size := B.Size.Abs()}

(B:aabb).Merge<public>(Other:aabb)<computes>:aabb =
    Low := B.Position.Min(Other.Position)
    High := B.GetEnd().Max(Other.GetEnd())
    aabb{Position := Low, Size := High - Low}

(B:aabb).Expand<public>(Point:vector3)<computes>:aabb =
    Low := B.Position.Min(Point)
    High := B.GetEnd().Max(Point)
    aabb{Position := Low, Size := High - Low}

(B:aabb).Grow<public>(Amount:float)<computes>:aabb =
    Offset := vector3{X := Amount, Y := Amount, Z := Amount}
    aabb{Position := B.Position - Offset, Size := B.Size + Offset * 2.0}

# --- basis -------------------------------------------------------------------
#
# **`X`, `Y` and `Z` are Godot's columns, not its rows**, and that is the one thing to hold on to
# here: `Basis.x` is `get_column(0)`. Phase 1 shipped the transposed version of this once already --
# it serialised rows and rebuilt from columns, so every basis that crossed came back transposed --
# and every body below is written in column terms so the mistake cannot come back.
#
# In columns, transforming a vector is just the weighted sum of the columns, which is why the
# operator reads as simply as it does.

operator'*'<public>(B:basis, V:vector3)<computes>:vector3 = B.X * V.X + B.Y * V.Y + B.Z * V.Z

# Column i of `A * B` is `A` applied to column i of `B`, because `(AB)e = A(Be)`. Composition, and
# not commutative: `A * B` applies B first.
operator'*'<public>(L:basis, R:basis)<computes>:basis =
    basis{X := L * R.X, Y := L * R.Y, Z := L * R.Z}

operator'*'<public>(B:basis, S:float)<computes>:basis =
    basis{X := B.X * S, Y := B.Y * S, Z := B.Z * S}

# Rows become columns. For a *rotation* -- an orthonormal basis -- this is also the inverse, which is
# the case scene code actually has; the general inverse is not written.
(B:basis).Transposed<public>()<computes>:basis =
    basis{
        X := vector3{X := B.X.X, Y := B.Y.X, Z := B.Z.X},
        Y := vector3{X := B.X.Y, Y := B.Y.Y, Z := B.Z.Y},
        Z := vector3{X := B.X.Z, Y := B.Y.Z, Z := B.Z.Z}}

# The scalar triple product of the columns, which is the signed volume the basis scales by. Negative
# means it flips handedness.
(B:basis).Determinant<public>()<computes>:float = B.X.Dot(B.Y.Cross(B.Z))

# Each column's length is that axis's scale, which is what `get_scale` reports for a basis without
# shear -- and the sign of the determinant is what tells a flipped one from a rotated one.
(B:basis).GetScale<public>()<reads>:vector3 =
    Magnitude := vector3{X := B.X.Length(), Y := B.Y.Length(), Z := B.Z.Length()}
    if (B.Determinant() < 0.0) then -Magnitude else Magnitude

(B:basis).Scaled<public>(Scale:vector3)<computes>:basis =
    basis{X := B.X * Scale.X, Y := B.Y * Scale.Y, Z := B.Z * Scale.Z}

# --- transform2d -------------------------------------------------------------
#
# Columns again: `X` and `Y` are the basis and `Origin` is the translation.

# The basis alone, which is what a *direction* is transformed by -- a normal or a velocity must not
# pick up the translation, and forgetting that is the classic 2D transform bug.
(T:transform2d).BasisXform<public>(V:vector2)<computes>:vector2 = T.X * V.X + T.Y * V.Y

operator'*'<public>(T:transform2d, V:vector2)<computes>:vector2 = T.BasisXform(V) + T.Origin

operator'*'<public>(L:transform2d, R:transform2d)<computes>:transform2d =
    transform2d{
        X := L.BasisXform(R.X),
        Y := L.BasisXform(R.Y),
        Origin := L * R.Origin}

(T:transform2d).Determinant<public>()<computes>:float = T.X.X * T.Y.Y - T.X.Y * T.Y.X

# The rotation the basis carries, and the scale along each axis.
(T:transform2d).GetRotation<public>()<reads>:float = ArcTan(T.X.Y, T.X.X)
(T:transform2d).GetScale<public>()<reads>:vector2 =
    Magnitude := vector2{X := T.X.Length(), Y := T.Y.Length()}
    if (T.Determinant() < 0.0) then vector2{X := Magnitude.X, Y := -Magnitude.Y} else Magnitude

(T:transform2d).Translated<public>(Offset:vector2)<computes>:transform2d =
    transform2d{X := T.X, Y := T.Y, Origin := T.Origin + Offset}

(T:transform2d).Scaled<public>(Scale:vector2)<computes>:transform2d =
    transform2d{
        X := vector2{X := T.X.X * Scale.X, Y := T.X.Y * Scale.Y},
        Y := vector2{X := T.Y.X * Scale.X, Y := T.Y.Y * Scale.Y},
        Origin := vector2{X := T.Origin.X * Scale.X, Y := T.Origin.Y * Scale.Y}}

# Godot's `affine_inverse`, written out of core/math/transform_2d.cpp rather than derived: the
# component swap and the two sign patterns are its own, and the origin is the *inverted* basis
# applied to the negated origin -- not the original basis, which is the easy mistake.
(T:transform2d).AffineInverse<public>()<decides><computes>:transform2d =
    Det := T.Determinant()
    Det <> 0.0
    Idet := 1.0 / Det
    InvX := vector2{X := T.Y.Y * Idet, Y := -T.X.Y * Idet}
    InvY := vector2{X := -T.Y.X * Idet, Y := T.X.X * Idet}
    Inverted := transform2d{X := InvX, Y := InvY, Origin := vector2{}}
    transform2d{X := InvX, Y := InvY, Origin := Inverted.BasisXform(-T.Origin)}

# --- transform3d -------------------------------------------------------------

operator'*'<public>(T:transform3d, V:vector3)<computes>:vector3 = T.Basis * V + T.Origin

operator'*'<public>(L:transform3d, R:transform3d)<computes>:transform3d =
    transform3d{Basis := L.Basis * R.Basis, Origin := L * R.Origin}

(T:transform3d).Translated<public>(Offset:vector3)<computes>:transform3d =
    transform3d{Basis := T.Basis, Origin := T.Origin + Offset}

(T:transform3d).Scaled<public>(Scale:vector3)<computes>:transform3d =
    transform3d{
        Basis := T.Basis.Scaled(Scale),
        Origin := vector3{
            X := T.Origin.X * Scale.X,
            Y := T.Origin.Y * Scale.Y,
            Z := T.Origin.Z * Scale.Z}}

# The inverse of a transform whose basis is a *rotation*, which is what a camera or a bone has. The
# general affine inverse needs a general basis inverse and is not written; this one is a transpose,
# so it is exact rather than a division.
(T:transform3d).InverseOrthonormal<public>()<computes>:transform3d =
    Inverted := T.Basis.Transposed()
    transform3d{Basis := Inverted, Origin := Inverted * -T.Origin}

# --- projection --------------------------------------------------------------
#
# Four vector4 columns, and the same column rule as basis. Only the products are written: a
# projection is built by the engine (`Camera3D.get_camera_projection()`) and consumed, rather than
# assembled by hand in scene code.

operator'*'<public>(P:projection, V:vector4)<computes>:vector4 =
    P.X * V.X + P.Y * V.Y + P.Z * V.Z + P.W * V.W

operator'*'<public>(L:projection, R:projection)<computes>:projection =
    projection{X := L * R.X, Y := L * R.Y, Z := L * R.Z, W := L * R.W}

# --- the last six @GlobalScope utilities --------------------------------------

# C's `fmod`, which Verse has no float counterpart for -- its own `Mod` is integer. Truncating
# toward zero rather than flooring, which is what makes `FMod(-7.0, 3.0)` answer -1.0 and not 2.0,
# and which is what the angle interpolations below depend on.
FMod<public>(A:float, B:float)<transacts>:float =
    if (B = 0.0):
        A
    else:
        Ratio := A / B
        Truncated := if (Ratio < 0.0) then CeilF(Ratio) else FloorF(Ratio)
        A - B * Truncated

# `nearest_po2`. Written as a recursion rather than Godot's bit-smearing, because the smear is an
# unsigned-32-bit trick and Verse's int is a signed 64.
Po2AtLeast<public>(Value:int, Candidate:int)<transacts>:int =
    if (Candidate >= Value) then Candidate else Po2AtLeast(Value, Candidate * 2)

NearestPo2<public>(Value:int)<transacts>:int = if (Value <= 0) then 0 else Po2AtLeast(Value, 1)

# `step_decimals`: how many decimal places a step implies, which is what an inspector spinbox uses to
# decide how to render itself. The thresholds are Godot's own, nines and all -- they exist to absorb
# float error, so rounding them to 0.1, 0.01 would change the answer at the boundary.
StepDecimals<public>(Step:float)<transacts>:int =
    Magnitude := Abs(Step)
    Decs := Magnitude - FloorF(Magnitude)
    if (Decs >= 0.9999) then 0
    else if (Decs >= 0.09999) then 1
    else if (Decs >= 0.009999) then 2
    else if (Decs >= 0.0009999) then 3
    else if (Decs >= 0.00009999) then 4
    else if (Decs >= 0.000009999) then 5
    else if (Decs >= 0.0000009999) then 6
    else if (Decs >= 0.00000009999) then 7
    else if (Decs >= 0.000000009999) then 8
    else if (Decs >= 0.0000000009999) then 9
    else 0

# The Barry-Goldman form of `cubic_interpolate_in_time`, where the three neighbours carry their own
# timestamps -- which is what an animation track with uneven keys needs. Each zero-denominator guard
# is Godot's own and answers the limit rather than declining.
CubicInterpolateInTime<public>(From:float, To:float, Pre:float, Post:float, Weight:float, ToT:float, PreT:float, PostT:float)<transacts>:float =
    T := Lerp(0.0, ToT, Weight)
    A1 := Lerp(Pre, From, if (PreT = 0.0) then 0.0 else (T - PreT) / -PreT)
    A2 := Lerp(From, To, if (ToT = 0.0) then 0.5 else T / ToT)
    A3 := Lerp(To, Post, if (PostT - ToT = 0.0) then 1.0 else (T - ToT) / (PostT - ToT))
    B1 := Lerp(A1, A2, if (ToT - PreT = 0.0) then 0.0 else (T - PreT) / (ToT - PreT))
    B2 := Lerp(A2, A3, if (PostT = 0.0) then 1.0 else T / PostT)
    Lerp(B1, B2, if (ToT = 0.0) then 0.5 else T / ToT)

# The angle forms. Each neighbour is first brought onto the same turn as `From` -- the
# `fmod(2*diff) - diff` dance is Godot's, and it is what keeps a curve through 350, 10, 20 degrees
# from sweeping backwards through the whole circle.
NearestAngle<public>(Reference:float, Angle:float)<transacts>:float =
    Difference := FMod(Angle - Reference, MathTau)
    Reference + FMod(2.0 * Difference, MathTau) - Difference

CubicInterpolateAngle<public>(From:float, To:float, Pre:float, Post:float, Weight:float)<transacts>:float =
    FromRot := FMod(From, MathTau)
    PreRot := NearestAngle(FromRot, Pre)
    ToRot := NearestAngle(FromRot, To)
    PostRot := NearestAngle(ToRot, Post)
    CubicInterpolate(FromRot, ToRot, PreRot, PostRot, Weight)

CubicInterpolateAngleInTime<public>(From:float, To:float, Pre:float, Post:float, Weight:float, ToT:float, PreT:float, PostT:float)<transacts>:float =
    FromRot := FMod(From, MathTau)
    PreRot := NearestAngle(FromRot, Pre)
    ToRot := NearestAngle(FromRot, To)
    PostRot := NearestAngle(ToRot, Post)
    CubicInterpolateInTime(FromRot, ToRot, PreRot, PostRot, Weight, ToT, PreT, PostT)
