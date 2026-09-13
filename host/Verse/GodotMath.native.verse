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
# **Not everything is here, and what is missing is on the record.** Godot has 367 methods and 261
# operators across its 16 math types, and C# solved that by writing them all. Nine types are covered
# here -- the six vectors, `color`, `rect2` and `rect2i` -- plus the `@GlobalScope` scalar math Verse
# has no name for. The **transform family** is what is left: `plane`, `quaternion`, `aabb`, `basis`,
# `transform2d`, `transform3d` and `projection` have nothing, and that is where most of the 466
# remaining skips live.
#
# `tools/gen_verse_api.py` reads *this file* to find out what is written and records everything else
# as a `math_not_written` skip, which is what makes the absence say so in the editor instead of
# being a name that is simply not there (R-SCN-2, docs/phase-4-design.md 8.3). Reading the file
# rather than maintaining a list is the point: **add a method below and its skip disappears on the
# next generation**, with nothing else to update. 466 of the 628 are skips today, from 585.
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
