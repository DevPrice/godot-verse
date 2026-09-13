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
# **Not everything is here, and the gap is not yet on the record.** Godot has 367 methods and 261
# operators across its 16 math types, and C# solved that by writing them all. This writes what the
# yardstick and ordinary scene code reach, and only for `vector2`, `vector2i`, `vector3` and
# `color`: the other twelve types have no operators at all, and the long tail --
# `bezier_interpolate`, `cubic_interpolate`, `orthonormalized`, `slerp`, `snapped`, the
# Basis/Quaternion conversions -- is absent everywhere.
#
# `docs/phase-4-design.md` 8.3 asked for each of those to be **recorded as a skip with a reason**,
# so the gap says so in the editor rather than being a name that is simply not there (R-SCN-2).
# That is not built: `tools/gen_verse_api.py` never enumerates `builtin_classes[*].methods`, so
# only these types' *constants* appear in `src/verse_api_skipped.h`. It is the largest hole left in
# R-SCN-2's promise and it is tracked in phase-4-design.md 13.5.

# --- vector2 -----------------------------------------------------------------

operator'+'<public>(L:vector2, R:vector2)<computes>:vector2 = vector2{X := L.X + R.X, Y := L.Y + R.Y}
operator'-'<public>(L:vector2, R:vector2)<computes>:vector2 = vector2{X := L.X - R.X, Y := L.Y - R.Y}
operator'*'<public>(L:vector2, S:float)<computes>:vector2 = vector2{X := L.X * S, Y := L.Y * S}
operator'*'<public>(S:float, R:vector2)<computes>:vector2 = vector2{X := R.X * S, Y := R.Y * S}
prefix'-'<public>(V:vector2)<computes>:vector2 = vector2{X := -V.X, Y := -V.Y}

# Componentwise, which is what Godot's `Vector2 * Vector2` means -- not a dot product.
operator'*'<public>(L:vector2, R:vector2)<computes>:vector2 = vector2{X := L.X * R.X, Y := L.Y * R.Y}

# Division is failable because float division is: Godot answers `inf` for a zero divisor and Verse
# declines, and declining is the honest one. The caller who knows the divisor is non-zero writes the
# brackets; the one who does not was going to get `inf` and not notice.
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

# `floor()`, `ceil()` and `round()` are **not written**, and the reason is worth recording rather
# than leaving as a gap: Verse's own Floor, Ceil and Round answer an `int`, Godot's answer a vector
# of whole floats, and Verse has no int-to-float conversion to bridge them with. They are in the
# recorded skip list with everything else this file does not carry.

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
