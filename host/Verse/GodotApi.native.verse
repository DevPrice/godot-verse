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

# The payload slots, made total. A slot the tag promised but did not deliver means the host packed
# something the caller was not generated for, which is a bug in the bridge rather than a value that
# might legitimately be absent -- so it raises. VhTypeMismatch unwinds and never returns, which is
# what makes the fallbacks below unreachable and these functions total.
VhIntAt<public>(Value:variant, Index:int, Expected:string)<transacts>:int =
    if (N := Value(1)[Index]):
        N
    else:
        VhTypeMismatch(Expected, Value)
        0

VhFloatAt<public>(Value:variant, Index:int, Expected:string)<transacts>:float =
    if (F := Value(2)[Index]):
        F
    else:
        VhTypeMismatch(Expected, Value)
        0.0

VhTextAt<public>(Value:variant, Index:int, Expected:string)<transacts>:string =
    if (S := Value(3)[Index]):
        S
    else:
        VhTypeMismatch(Expected, Value)
        ""

# Verse's <> is a failable comparison, not a boolean operator, so the logic has to be produced.
VhToLogic<public>(Value:variant)<transacts>:logic =
    if (VhIntAt(Value, 0, "logic") <> 0) then true else false
VhToInt<public>(Value:variant)<transacts>:int = VhIntAt(Value, 0, "int")
VhToFloat<public>(Value:variant)<transacts>:float = VhFloatAt(Value, 0, "float")
VhToString<public>(Value:variant)<transacts>:string = VhTextAt(Value, 0, "string")
VhToStrings<public>(Value:variant)<transacts>:[]string = Value(3)
VhToVector2<public>(Value:variant)<transacts>:vector2 = vector2{X := VhFloatAt(Value, 0, "vector2"), Y := VhFloatAt(Value, 1, "vector2")}
VhToVector3<public>(Value:variant)<transacts>:vector3 = vector3{X := VhFloatAt(Value, 0, "vector3"), Y := VhFloatAt(Value, 1, "vector3"), Z := VhFloatAt(Value, 2, "vector3")}
VhToColor<public>(Value:variant)<transacts>:color = color{R := VhFloatAt(Value, 0, "color"), G := VhFloatAt(Value, 1, "color"), B := VhFloatAt(Value, 2, "color"), A := VhFloatAt(Value, 3, "color")}

# The one place a Godot value is still allowed to be absent. A null object crosses as an
# object-tagged zero rather than as a missing slot, so this is what makes GetParent() at the scene
# root, or a FindChild that misses, an ordinary Verse failure -- and what stops a null from being
# handed back as a live-looking reference with handle 0, which is what it used to do.
VhToHandle<public>(Value:variant)<decides><transacts>:int =
    H := VhIntAt(Value, 0, "object")
    H <> 0
    H
