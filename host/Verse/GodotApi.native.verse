using {/Verse.org/Native}

# The Godot API as ordinary Verse. Nothing here is <native>: every class derives from
# `object`, whose C++ shadow supplies the object representation, and every method bottoms out
# in the handful of native primitives in Godot.native.verse. That is what keeps a mirrored class
# per Godot class from needing a C++ shadow per Godot class.

# --- Variant tags ------------------------------------------------------------

# Godot's Variant::Type, spelled as constants rather than a Verse enum so that `variant` stays
# a plain tuple VNI can marshal without a struct on the wire.
TagNil<public>:int = 0
TagBool<public>:int = 1
TagInt<public>:int = 2
TagFloat<public>:int = 3
TagString<public>:int = 4
TagVector2<public>:int = 5
TagVector3<public>:int = 9
TagColor<public>:int = 20
TagStringName<public>:int = 21
TagNodePath<public>:int = 22
TagObject<public>:int = 24
TagArray<public>:int = 28
TagPackedStringArray<public>:int = 34

# --- Builtin value types -----------------------------------------------------

vector2<public> := struct:
    X<public>:float = 0.0
    Y<public>:float = 0.0

vector3<public> := struct:
    X<public>:float = 0.0
    Y<public>:float = 0.0
    Z<public>:float = 0.0

color<public> := struct:
    R<public>:float = 0.0
    G<public>:float = 0.0
    B<public>:float = 0.0
    A<public>:float = 1.0

# --- variant packing -----------------------------------------------------

VhFromLogic<public>(Value:logic)<transacts>:variant = (TagBool, array{if (Value?) then 1 else 0}, array{}, array{})
VhFromInt<public>(Value:int)<transacts>:variant = (TagInt, array{Value}, array{}, array{})
VhFromFloat<public>(Value:float)<transacts>:variant = (TagFloat, array{}, array{Value}, array{})
VhFromString<public>(Value:string)<transacts>:variant = (TagString, array{}, array{}, array{Value})
VhFromStringName<public>(Value:string)<transacts>:variant = (TagStringName, array{}, array{}, array{Value})
VhFromNodePath<public>(Value:string)<transacts>:variant = (TagNodePath, array{}, array{}, array{Value})
VhFromVector2<public>(Value:vector2)<transacts>:variant = (TagVector2, array{}, array{Value.X, Value.Y}, array{})
VhFromVector3<public>(Value:vector3)<transacts>:variant = (TagVector3, array{}, array{Value.X, Value.Y, Value.Z}, array{})
VhFromColor<public>(Value:color)<transacts>:variant = (TagColor, array{}, array{Value.R, Value.G, Value.B, Value.A}, array{})
VhFromObject<public>(Value:object)<transacts>:variant = (TagObject, array{Value.Handle}, array{}, array{})

# Verse's <> is a failable comparison, not a boolean operator, so the logic has to be produced.
VhToLogic<public>(Value:variant)<decides><transacts>:logic =
    N := Value(1)[0]
    if (N <> 0) then true else false
VhToInt<public>(Value:variant)<decides><transacts>:int = Value(1)[0]
VhToFloat<public>(Value:variant)<decides><transacts>:float = Value(2)[0]
VhToString<public>(Value:variant)<decides><transacts>:string = Value(3)[0]
VhToStrings<public>(Value:variant)<transacts>:[]string = Value(3)
VhToVector2<public>(Value:variant)<decides><transacts>:vector2 = vector2{X := Value(2)[0], Y := Value(2)[1]}
VhToVector3<public>(Value:variant)<decides><transacts>:vector3 = vector3{X := Value(2)[0], Y := Value(2)[1], Z := Value(2)[2]}
VhToColor<public>(Value:variant)<decides><transacts>:color = color{R := Value(2)[0], G := Value(2)[1], B := Value(2)[2], A := Value(2)[3]}
VhToHandle<public>(Value:variant)<decides><transacts>:int = Value(1)[0]
