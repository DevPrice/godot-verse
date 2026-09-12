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
