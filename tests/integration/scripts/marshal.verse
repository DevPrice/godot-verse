using { /Godot.org/Godot }

# The marshalling matrix, as a script Godot attaches to a node and calls into.
#
# Every method here exists to be called from GDScript with arguments and to hand a value back, so
# one call checks a type in both directions. The class is named after its file, which is what makes
# a .verse file usable as a script at all.
@global_class
marshal := class(node2d):

    @export
    var Counter<public>:int = 0

    # --- scalars -------------------------------------------------------------

    EchoInt<public>(Value:int):int = Value
    EchoFloat<public>(Value:float):float = Value
    EchoString<public>(Value:string):string = Value
    EchoLogic<public>(Value:logic):logic = Value

    # Arithmetic rather than an echo, so a value that crossed as the wrong width shows up as a
    # wrong answer instead of surviving a round trip that never looked at it.
    AddInts<public>(A:int, B:int):int = A * 1000000 + B
    AddFloats<public>(A:float, B:float):float = A / B

    # --- strings -------------------------------------------------------------

    # Verse `string` is `[]char`, so length is what proves the bytes arrived rather than a pointer.
    StringLength<public>(Value:string):int = Value.Length
    Concat<public>(A:string, B:string):string = "{A}|{B}"

    # --- math types ----------------------------------------------------------
    # One method per lane shape: two floats, three, four, and the nested ones whose components the
    # packer has to walk a tree to reach. Each returns something derived from what it was given, so
    # a lane written in the wrong slot shows up as a wrong number rather than surviving unread.

    EchoVector2<public>(V:vector2):vector2 = vector2{X := V.Y, Y := V.X}
    EchoVector2i<public>(V:vector2i):vector2i = vector2i{X := V.Y, Y := V.X}
    EchoVector3<public>(V:vector3):vector3 = vector3{X := V.Z, Y := V.X, Z := V.Y}
    EchoVector4<public>(V:vector4):vector4 = vector4{X := V.W, Y := V.Z, Z := V.Y, W := V.X}
    EchoColor<public>(C:color):color = color{R := C.A, G := C.B, B := C.G, A := C.R}
    EchoQuaternion<public>(Q:quaternion):quaternion = Q

    # Nested: rect2 is two vector2s, and the sum proves every component arrived where it belongs.
    Rect2Sum<public>(R:rect2):float = R.Position.X + R.Position.Y * 10.0 + R.Size.X * 100.0 + R.Size.Y * 1000.0

    # Two levels of nesting, and the type with the most lanes of any Godot value.
    Transform3DSum<public>(T:transform3d):float =
        T.Basis.X.X + T.Basis.Y.Y * 10.0 + T.Basis.Z.Z * 100.0 + T.Origin.X * 1000.0

    # A basis' X is Godot's own first *column*, which is what makes this the transpose check.
    BasisColumnX<public>(B:basis):vector3 = B.X

    # --- reference types -----------------------------------------------------
    # Array and Dictionary are references in Godot, so a mutation here has to be visible to the
    # caller. That is the property the whole reference design exists to keep, and the only one a
    # round trip cannot check -- a copy would round-trip perfectly and still be wrong.

    ArrayLength<public>(Items:godot_array):int = Items.Length()

    ArrayFirstInt<public>(Items:godot_array)<decides>:int = Items.GetInt[0]

    # Writes through the reference. GDScript sees it.
    ArrayAppendTo<public>(Items:godot_array, Index:int, Value:int):void =
        Items.SetInt(Index, Value)

    DictLookup<public>(Data:dictionary, Key:string)<decides>:int = Data.GetInt[Key]

    DictWrite<public>(Data:dictionary, Key:string, Value:int):void =
        Data.SetInt(Key, Value)

    DictLength<public>(Data:dictionary):int = Data.Length()

    # R-TYPE-3: a Callable the script was handed, invoked with arguments.
    CallIt<public>(Fn:callable, Arg:int):int = Fn.CallInt(array{Arg})

    # --- packed arrays -------------------------------------------------------
    # Values on Godot's side and Verse arrays on this one, whatever the wire does between.

    SumFloats<public>(Values:[]float):float =
        var Total:float = 0.0
        for (V : Values):
            set Total = Total + V
        Total

    JoinStrings<public>(Values:[]string):string =
        var Joined:string = ""
        for (V : Values):
            set Joined = "{Joined}{V}/"
        Joined

    MakeFloats<public>(Count:int):[]float =
        for (I := 0..Count - 1):
            1.0 * I

    # --- arity ---------------------------------------------------------------

    NoArgs<public>():int = 42

    # --- failure -------------------------------------------------------------

    # <decides>: declining is an outcome, not an error. GDScript sees nil.
    Positive<public>(Value:int)<decides>:int =
        Value > 0
        Value

    # --- state ---------------------------------------------------------------

    # A method with a side effect the caller can read back through the exported member, which is
    # what says the call reached this instance rather than the class default.
    Bump<public>(By:int):int =
        set Counter = Counter + By
        Counter

    # --- virtuals ------------------------------------------------------------

    Ready<override>():void =
        Print("marshal ready")

    # --- R-TYPE-7 amended: reading a Godot Variant ---------------------------
    #
    # A Godot Array is heterogeneous, and these are what a script can now say about one. VariantKind
    # branches when the type is unknown; As<GodotType> asks when it is expected, and fails rather
    # than raising when the answer is no. A script still cannot reach a lane: `V.I0` does not
    # compile, and neither does `variant{Tag := 2}`.
    KindAt<public>(A:godot_array, Index:int):string =
        if (V := A.GetVariant[Index]):
            case (VariantKind(V)):
                variant_kind.TypeInt => "int"
                variant_kind.TypeFloat => "float"
                variant_kind.TypeString => "string"
                variant_kind.TypeVector2 => "vector2"
                variant_kind.TypeNil => "nil"
                _ => "other"
        else:
            "missing"

    IntAt<public>(A:godot_array, Index:int)<decides>:int =
        V := A.GetVariant[Index]
        AsInt[V]

    Vector2XAt<public>(A:godot_array, Index:int)<decides>:float =
        V := A.GetVariant[Index]
        AsVector2[V].X

    # Asking for the wrong type is a failure, not an error -- which is the whole reason the readers
    # are <decides> rather than raising the way a mirrored getter does.
    StringAtFails<public>(A:godot_array, Index:int)<decides>:string =
        V := A.GetVariant[Index]
        AsString[V]

    # And building one, which is the other direction: VariantFromInt rather than an overload, for
    # the reason in gen_verse_api.py's VARIANT_LANES comment.
    PutInt<public>(A:godot_array, Index:int, Value:int):void =
        A.SetVariant(Index, VariantFromInt(Value))

    # --- R-TYPE-1: a packed array whose elements are structs -----------------
    #
    # PackedVector2Array crosses as a reference like every container, but each element is its own
    # variant rather than a lane in a flat sequence.
    SumVector2Xs<public>(Values:[]vector2):float =
        var Total:float = 0.0
        for (V : Values):
            set Total = Total + V.X
        Total

    MakeVector2s<public>(Count:int):[]vector2 =
        for (I := 0..Count - 1):
            vector2{X := 1.0 * I, Y := 2.0 * I}

    # The other direction, and the one nothing tested: a packed array handed to a *mirrored Godot*
    # method rides through the reference table rather than being marshalled as a script return.
    # Marshalls is a singleton and both of these are pure, so a headless Godot can answer.
    Base64Of<public>(Bytes:[]int)<decides>:string =
        Marshalls := GetMarshalls[]
        Marshalls.RawToBase64(Bytes)

    BytesOfBase64<public>(Text:string)<decides>:[]int =
        Marshalls := GetMarshalls[]
        Marshalls.Base64ToRaw(Text)

    # --- R-LANG-6: a library file, resolved with no import -------------------
    #
    # HelperDouble and HelperAnswer live in helpers.verse, which declares no class and so cannot go
    # on a node. One flat scope is what makes them visible here with nothing written to import them.
    CallHelper<public>(Value:int):int = HelperDouble(Value)
    ReadHelperConstant<public>():int = HelperAnswer
    CallHelperString<public>(Name:string):string = HelperGreeting(Name)
