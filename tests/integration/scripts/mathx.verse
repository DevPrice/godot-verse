using { /Godot.org/Godot }

# G13/G11: the math written in Verse, checked against what Godot's own answers.
#
# The point of each case is an **edge**, not a formula. A componentwise op on positive whole numbers
# passes against almost any implementation; what separates a right one from a plausible one is
# `floor(-1.2)`, `-3 / 2` on an integer vector, and `lerp_angle` across the wrap point. Every value
# asserted in test_main.gd was taken from Godot's own function, not from this file.
@global_class
mathx := class(node2d):

    # Godot's floor rounds toward negative infinity, so -1.2 floors to -2 rather than -1.
    FloorY<public>(X:float, Y:float)<transacts>:float = vector2{X := X, Y := Y}.Floor().Y
    CeilY<public>(X:float, Y:float)<transacts>:float = vector2{X := X, Y := Y}.Ceil().Y
    RoundY<public>(X:float, Y:float)<transacts>:float = vector2{X := X, Y := Y}.Round().Y

    SnappedX<public>(X:float, Step:float)<transacts>:float =
        vector2{X := X, Y := 0.0}.Snapped(vector2{X := Step, Y := Step}).X

    # Componentwise, which is the thing about vector min/max a reader gets wrong.
    MinX<public>(AX:float, AY:float, BX:float, BY:float)<transacts>:float =
        vector2{X := AX, Y := AY}.Min(vector2{X := BX, Y := BY}).X
    MaxY<public>(AX:float, AY:float, BX:float, BY:float)<transacts>:float =
        vector2{X := AX, Y := AY}.Max(vector2{X := BX, Y := BY}).Y

    # Integer division truncates toward zero, as C does and as Godot's Vector2i does -- Verse's own
    # `Quotient` floors, so this is the one that would have been off by one for negatives.
    IntDivY<public>(Y:int, S:int)<transacts>:int =
        if (D := vector2i{X := 0, Y := Y} / S) then D.Y else 0

    # A length is a float even for an integer vector.
    IntLength<public>(X:int, Y:int)<transacts>:float = vector2i{X := X, Y := Y}.Length()

    Vector4Dot<public>()<transacts>:float =
        V := vector4{X := 1.0, Y := 2.0, Z := 3.0, W := 4.0}
        V.Dot(V)

    # The short way round: 0 to 6 radians is nearly a full turn, so the halfway point is *negative*.
    # A hand-rolled lerp that ignores wrapping answers 3.0 here.
    LerpAngleHalf<public>(From:float, To:float)<transacts>:float = LerpAngle(From, To, 0.5)

    WrapNegative<public>(X:float, Low:float, High:float)<transacts>:float = WrapF(X, Low, High)

    SmoothstepAt<public>(X:float)<transacts>:float = Smoothstep(0.0, 1.0, X)

    EaseAt<public>(X:float, Curve:float)<transacts>:float = Ease(X, Curve)

    # These were both silently zero once: Verse drops a continuation line that starts with an
    # operator, so the first draft of each answered its first term and nothing else.
    CubicAt<public>()<transacts>:float = CubicInterpolate(0.0, 10.0, -10.0, 20.0, 0.5)
    BezierDerivAt<public>()<transacts>:float = BezierDerivative(0.0, 0.0, 1.0, 1.0, 0.5)

    # Alpha survives both, which is the asymmetry with `*` that Godot has and a reader does not expect.
    DarkenedAlpha<public>()<transacts>:float =
        color{R := 0.5, G := 0.5, B := 0.5, A := 0.25}.Darkened(0.5).A
    LightenedRed<public>()<transacts>:float =
        color{R := 0.5, G := 0.5, B := 0.5, A := 1.0}.Lightened(0.5).R

    # A rect with a negative size contains nothing until `Abs` normalises it, which is what a
    # drag-selection does.
    NegativeRectHasPoint<public>()<transacts>:logic =
        R := rect2{Position := vector2{X := 10.0, Y := 10.0}, Size := vector2{X := -4.0, Y := -2.0}}
        if (R.HasPoint[vector2{X := 8.0, Y := 9.0}]) then true else false

    AbsRectHasPoint<public>()<transacts>:logic =
        R := rect2{Position := vector2{X := 10.0, Y := 10.0}, Size := vector2{X := -4.0, Y := -2.0}}
        if (R.Abs().HasPoint[vector2{X := 8.0, Y := 9.0}]) then true else false

    RectArea<public>(W:float, H:float)<transacts>:float =
        rect2{Position := vector2{}, Size := vector2{X := W, Y := H}}.GetArea()

    # The inverse hyperbolics decline outside their domain rather than answering NaN.
    AcoshOk<public>(X:float)<transacts>:logic = if (Acosh[X]) then true else false

    IsNanOfZeroOverZero<public>()<transacts>:logic =
        Zero := 0.0
        if (IsNan[Zero / Zero]) then true else false

    # The utilities that are Godot's behaviour rather than Verse's spelling.
    TypeNameOf<public>(VariantType:int)<transacts>:string = VariantTypeName(VariantType)
    ErrorNameOf<public>(Code:int)<transacts>:string = ErrorString(Code)
    # Through Godot's own id rather than the bridge's handle: `vh_object.Handle` is internal, and
    # deliberately so -- nothing a script writes should name it.
    SelfFromId<public>()<transacts>:logic =
        Id := GetInstanceId()
        if (Found := InstanceFromId[Id], Found.GetInstanceId() = Id) then true else false

    # --- the transform family ---------------------------------------------------

    # Quaternion composition is not commutative and `A * B` applies B first, so a half-turn about Y
    # then a half-turn about X is a different rotation from the reverse.
    QuatMulW<public>(AX:float, AY:float, AZ:float, AW:float, BX:float, BY:float, BZ:float, BW:float)<transacts>:float =
        A := quaternion{X := AX, Y := AY, Z := AZ, W := AW}
        B := quaternion{X := BX, Y := BY, Z := BZ, W := BW}
        (A * B).W

    # Rotating a vector by a quarter turn about Z takes +X to +Y.
    QuatXformY<public>()<reads>:float =
        Half := Sqrt(2.0) / 2.0
        Q := quaternion{X := 0.0, Y := 0.0, Z := Half, W := Half}
        (Q * vector3{X := 1.0, Y := 0.0, Z := 0.0}).Y

    QuatSlerpW<public>(Weight:float)<reads>:float =
        A := quaternion{X := 0.0, Y := 0.0, Z := 0.0, W := 1.0}
        B := quaternion{X := 0.0, Y := 0.0, Z := 1.0, W := 0.0}
        A.Slerp(B, Weight).W

    # A basis whose columns are scaled axes: the determinant is the volume scale, and `* V` is the
    # weighted sum of the columns. A transposed-vs-columns mistake shows up here as a wrong Y.
    BasisXformY<public>()<transacts>:float =
        B := basis{
            X := vector3{X := 1.0, Y := 2.0, Z := 0.0},
            Y := vector3{X := 0.0, Y := 1.0, Z := 0.0},
            Z := vector3{X := 0.0, Y := 0.0, Z := 1.0}}
        (B * vector3{X := 3.0, Y := 0.0, Z := 0.0}).Y

    BasisDet<public>(SX:float, SY:float, SZ:float)<transacts>:float =
        basis{
            X := vector3{X := SX, Y := 0.0, Z := 0.0},
            Y := vector3{X := 0.0, Y := SY, Z := 0.0},
            Z := vector3{X := 0.0, Y := 0.0, Z := SZ}}.Determinant()

    # A translation must not reach a *direction*, which is the whole point of BasisXform.
    Transform2dXformX<public>(OX:float, VX:float)<transacts>:float =
        T := transform2d{
            X := vector2{X := 2.0, Y := 0.0},
            Y := vector2{X := 0.0, Y := 2.0},
            Origin := vector2{X := OX, Y := 0.0}}
        (T * vector2{X := VX, Y := 0.0}).X

    Transform2dBasisXformX<public>(OX:float, VX:float)<transacts>:float =
        T := transform2d{
            X := vector2{X := 2.0, Y := 0.0},
            Y := vector2{X := 0.0, Y := 2.0},
            Origin := vector2{X := OX, Y := 0.0}}
        T.BasisXform(vector2{X := VX, Y := 0.0}).X

    # Composition, then the affine inverse undoing it: inverse(T) * (T * v) must be v.
    Transform2dRoundTripX<public>(VX:float, VY:float)<transacts>:float =
        T := transform2d{
            X := vector2{X := 0.0, Y := 2.0},
            Y := vector2{X := -3.0, Y := 0.0},
            Origin := vector2{X := 5.0, Y := 7.0}}
        V := vector2{X := VX, Y := VY}
        if (Inv := T.AffineInverse[]) then (Inv * (T * V)).X else -999.0

    Transform3dXformZ<public>()<transacts>:float =
        T := transform3d{
            Basis := basis{
                X := vector3{X := 1.0, Y := 0.0, Z := 0.0},
                Y := vector3{X := 0.0, Y := 1.0, Z := 0.0},
                Z := vector3{X := 0.0, Y := 0.0, Z := 2.0}},
            Origin := vector3{X := 0.0, Y := 0.0, Z := 5.0}}
        (T * vector3{X := 0.0, Y := 0.0, Z := 3.0}).Z

    PlaneDistance<public>(PX:float)<transacts>:float =
        P := plane{Normal := vector3{X := 1.0, Y := 0.0, Z := 0.0}, D := 2.0}
        P.DistanceTo(vector3{X := PX, Y := 0.0, Z := 0.0})

    AabbVolume<public>(W:float, H:float, D:float)<transacts>:float =
        aabb{Position := vector3{}, Size := vector3{X := W, Y := H, Z := D}}.GetVolume()

    # The last six utilities.
    FModAt<public>(A:float, B:float)<transacts>:float = FMod(A, B)
    NearestPo2At<public>(V:int)<transacts>:int = NearestPo2(V)
    StepDecimalsAt<public>(S:float)<transacts>:int = StepDecimals(S)
    CubicAngleAt<public>()<transacts>:float = CubicInterpolateAngle(0.1, 0.2, 0.0, 0.3, 0.5)
