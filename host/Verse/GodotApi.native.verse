using {/Verse.org/Native}

# The Godot API as ordinary Verse. Nothing here is <native>: every class derives from
# godot_object, whose C++ shadow supplies the object representation, and every method bottoms out
# in the handful of native primitives in Godot.native.verse. That is what keeps a mirrored class
# per Godot class from needing a C++ shadow per Godot class.

# --- Variant tags ------------------------------------------------------------

# Godot's Variant::Type, spelled as constants rather than a Verse enum so that godot_value stays
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

# --- godot_value packing -----------------------------------------------------

FromLogic<public>(Value:logic)<transacts>:godot_value = (TagBool, array{if (Value?) then 1 else 0}, array{}, array{})
FromInt<public>(Value:int)<transacts>:godot_value = (TagInt, array{Value}, array{}, array{})
FromFloat<public>(Value:float)<transacts>:godot_value = (TagFloat, array{}, array{Value}, array{})
FromString<public>(Value:string)<transacts>:godot_value = (TagString, array{}, array{}, array{Value})
FromStringName<public>(Value:string)<transacts>:godot_value = (TagStringName, array{}, array{}, array{Value})
FromNodePath<public>(Value:string)<transacts>:godot_value = (TagNodePath, array{}, array{}, array{Value})
FromVector2<public>(Value:vector2)<transacts>:godot_value = (TagVector2, array{}, array{Value.X, Value.Y}, array{})
FromVector3<public>(Value:vector3)<transacts>:godot_value = (TagVector3, array{}, array{Value.X, Value.Y, Value.Z}, array{})
FromColor<public>(Value:color)<transacts>:godot_value = (TagColor, array{}, array{Value.R, Value.G, Value.B, Value.A}, array{})
FromObject<public>(Value:godot_object)<transacts>:godot_value = (TagObject, array{Value.Handle}, array{}, array{})

# Verse's <> is a failable comparison, not a boolean operator, so the logic has to be produced.
ToLogic<public>(Value:godot_value)<decides><transacts>:logic =
    N := Value(1)[0]
    if (N <> 0) then true else false
ToInt<public>(Value:godot_value)<decides><transacts>:int = Value(1)[0]
ToFloat<public>(Value:godot_value)<decides><transacts>:float = Value(2)[0]
ToString<public>(Value:godot_value)<decides><transacts>:string = Value(3)[0]
ToStrings<public>(Value:godot_value)<transacts>:[]string = Value(3)
ToVector2<public>(Value:godot_value)<decides><transacts>:vector2 = vector2{X := Value(2)[0], Y := Value(2)[1]}
ToVector3<public>(Value:godot_value)<decides><transacts>:vector3 = vector3{X := Value(2)[0], Y := Value(2)[1], Z := Value(2)[2]}
ToColor<public>(Value:godot_value)<decides><transacts>:color = color{R := Value(2)[0], G := Value(2)[1], B := Value(2)[2], A := Value(2)[3]}
ToHandle<public>(Value:godot_value)<decides><transacts>:int = Value(1)[0]

# --- The class hierarchy -----------------------------------------------------

godot_node<public> := class(godot_object):

    GetName<public>()<decides><transacts>:string = ToString[CallValue[Handle, "get_name", array{}]]
    IsInsideTree<public>()<decides><transacts>:logic = ToLogic[CallValue[Handle, "is_inside_tree", array{}]]
    GetParent<public>()<decides><transacts>:godot_node = godot_node{Handle := ToHandle[CallValue[Handle, "get_parent", array{}]]}
    GetChild<public>(Index:int)<decides><transacts>:godot_node = godot_node{Handle := ToHandle[CallValue[Handle, "get_child", array{FromInt(Index)}]]}
    GetChildCount<public>()<decides><transacts>:int = ToInt[CallValue[Handle, "get_child_count", array{}]]

    AddChild<public>(Node:godot_node)<transacts>:void = CallVoid(Handle, "add_child", array{FromObject(Node)})
    QueueFree<public>()<transacts>:void = CallVoid(Handle, "queue_free", array{})

godot_canvas_item<public> := class(godot_node):

    IsVisible<public>()<decides><transacts>:logic = ToLogic[CallValue[Handle, "is_visible", array{}]]
    SetVisible<public>(Visible:logic)<transacts>:void = CallVoid(Handle, "set_visible", array{FromLogic(Visible)})
    GetModulate<public>()<decides><transacts>:color = ToColor[CallValue[Handle, "get_modulate", array{}]]
    SetModulate<public>(Modulate:color)<transacts>:void = CallVoid(Handle, "set_modulate", array{FromColor(Modulate)})

godot_node2d<public> := class(godot_canvas_item):

    GetPosition<public>()<decides><transacts>:vector2 = ToVector2[CallValue[Handle, "get_position", array{}]]
    SetPosition<public>(Position:vector2)<transacts>:void = CallVoid(Handle, "set_position", array{FromVector2(Position)})
    GetRotation<public>()<decides><transacts>:float = ToFloat[CallValue[Handle, "get_rotation", array{}]]
    SetRotation<public>(Radians:float)<transacts>:void = CallVoid(Handle, "set_rotation", array{FromFloat(Radians)})
    GetScale<public>()<decides><transacts>:vector2 = ToVector2[CallValue[Handle, "get_scale", array{}]]
    SetScale<public>(Scale:vector2)<transacts>:void = CallVoid(Handle, "set_scale", array{FromVector2(Scale)})
