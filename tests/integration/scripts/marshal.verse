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

    # R-EXP-1 with a *Godot* enum, which is what 4.3 pulled forward from Phase 4: the inspector shows
    # a dropdown of the enumerators and stores the one the author picked. The stored integer is the
    # ordinal rather than Godot's own value for the enumerator -- which is what GDScript and C# store
    # too, and is self-consistent because the enumerator is the identity on the Verse side: what
    # reaches Godot is ToInt of whichever one the ordinal named.
    @export
    var Mode<public>:node_process_mode = node_process_mode.Pausable

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

    # --- R-TYPE-2: a container the script built for itself --------------------
    #
    # The bare archetype, which is the spelling a Godot author reaches for first. It used to hold
    # reference 0 and cross as Nil, so a script could only ever hand Godot a container Godot had
    # handed it first; a container mints from its own data-member default now. What these assert on
    # the far side is that Godot got an Array it can read, write and store -- a reference of 0 would
    # arrive as null and fail every one of them.
    MadeArray<public>()<reads>:godot_array = godot_array{}

    MadeArrayFilled<public>():godot_array =
        Made := godot_array{}
        Made.AddInt(7)
        Made.AddString("x")
        Made

    MadeDictionary<public>():dictionary =
        Made := dictionary{}
        Made.SetInt("hp", 3)
        Made

    # The typed container has no bare archetype -- `Unpack` and `Pack` are required members with no
    # defaults -- so its maker supplies those two and no `Ref`, which is what mints it.
    MadeNodeArray<public>()<reads>:typed_array(node) = MakeNodeArray()

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

    _Ready<override>():void =
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
                variant_type.TypeInt => "int"
                variant_type.TypeFloat => "float"
                variant_type.TypeString => "string"
                variant_type.TypeVector2 => "vector2"
                variant_type.TypeNil => "nil"
                _ => "other"
        else:
            "missing"

    IntAt<public>(A:godot_array, Index:int)<decides>:int =
        V := A.GetVariant[Index]
        V.AsInt[]

    Vector2XAt<public>(A:godot_array, Index:int)<decides>:float =
        V := A.GetVariant[Index]
        V.AsVector2[].X

    # Asking for the wrong type is a failure, not an error -- which is the whole reason the readers
    # are <decides> rather than raising the way a mirrored getter does.
    StringAtFails<public>(A:godot_array, Index:int)<decides>:string =
        V := A.GetVariant[Index]
        V.AsString[]

    # And building one, which is the other direction: VariantInt rather than an overload, for
    # the reason in gen_verse_api.py's VARIANT_LANES comment.
    PutInt<public>(A:godot_array, Index:int, Value:int):void =
        A.SetVariant(Index, VariantInt(Value))

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
    Base64Of<public>(Bytes:[]int):string =
        GetMarshalls().RawToBase64(Bytes)

    BytesOfBase64<public>(Text:string):[]int =
        GetMarshalls().Base64ToRaw(Text)

    # --- R-TYPE-2, R-SCN-1: objects in a container ---------------------------
    #
    # GetChildren() returns typedarray::Node, which the generator skipped entirely before this phase:
    # a godot_array offers ten typed element accessors and not one of them is an object, so walking
    # children -- which is what scene code is -- had no spelling. It is a typed_array(node) now, and
    # what this proves is that its elements are real mirrored objects with methods, not handles.
    ChildCount<public>():int = GetChildren().Length()

    FirstChildName<public>()<decides>:string =
        Children := GetChildren()
        Child := Children.Get[0]
        Child.GetName()

    # The whole container as a Verse array, and a method called on each element.
    ChildNames<public>():string =
        var Joined:string = ""
        for (Child : GetChildren().ToArray()):
            set Joined = "{Joined}{Child.GetName()}/"
        Joined

    # Past the end is a miss rather than an error, the same answer an absent Dictionary key gives.
    HasChildAt<public>(Index:int)<decides>:void =
        Children := GetChildren()
        Children.Get[Index]
        false?

    # --- R-SCN-5: Godot's enums as Verse enums -------------------------------
    #
    # `SetProcessMode(2)` used to compile and say nothing about what 2 meant. Now the type is the
    # documentation, and a wrong integer is a compile error rather than a subtly wrong scene.
    #
    # ProcessMode reaches Verse as a *property*, which is the case that matters most and the one
    # Godot's own metadata hides: it reports process_mode as an int, and only get_process_mode says
    # enum::Node.ProcessMode. 515 of Godot's 994 int properties are really enums.
    SetAlways<public>():void = set ProcessMode = node_process_mode.Always

    ProcessModeName<public>():string =
        case (ProcessMode):
            node_process_mode.Inherit => "inherit"
            node_process_mode.Pausable => "pausable"
            node_process_mode.WhenPaused => "when_paused"
            node_process_mode.Always => "always"
            node_process_mode.Disabled => "disabled"

    ModeName<public>():string =
        case (Mode):
            node_process_mode.Inherit => "inherit"
            node_process_mode.Pausable => "pausable"
            node_process_mode.WhenPaused => "when_paused"
            node_process_mode.Always => "always"
            node_process_mode.Disabled => "disabled"

    # The integer is still reachable, which is what makes a bitfield spellable: a combination of
    # flags is not an enumerator, so a bitfield parameter stays an int and Verse's own BitOr does
    # the work GDScript's `|` does.
    ProcessModeInt<public>():int = ToInt(ProcessMode)

    CtrlAndShift<public>():int =
        BitOr(ToInt(key_modifier_mask.MaskCtrl), ToInt(key_modifier_mask.MaskShift))

    # --- R-INT-2: calling a GDScript-defined method, by name -----------------
    #
    # `callv` is an ordinary method on Godot's Object, and Object is mirrored now that Variant and
    # Array both cross -- so the requirement the spec calls "the one with no existing path" arrives
    # as a side effect of 4.4 rather than as work of its own. The argument Array comes from the
    # caller, since a script cannot mint a fresh Godot container.
    CallSibling<public>(Target:object, Method:string, Args:godot_array)<decides>:int =
        Target.Callv(Method, Args).AsInt[]

    # The same call, with the argument list built here rather than handed over. This is what
    # `godot_array{}` could not do: it compiles and holds reference 0, which crosses as Nil, so
    # `Callv` had no way to be given arguments from Verse at all (R-TYPE-2, R-INT-2).
    CallSiblingWith<public>(Target:object, Method:string, Value:int)<decides>:int =
        Args := MakeArray()
        Args.AddInt(Value)
        Target.Callv(Method, Args).AsInt[]

    # A container built in Verse and handed *back* to Godot as a value, which is the other half:
    # what comes out the far side has to be the thing that was built, elements and all.
    MakeMixed<public>(Count:int, Label:string):godot_array =
        Made := MakeArray()
        for (I := 0..Count - 1):
            Made.AddInt(I)
        Made.AddString(Label)
        Made

    MakeLookup<public>(Key:string, Value:int):dictionary =
        Made := MakeDictionary()
        Made.SetInt(Key, Value)
        Made

    # A *typed* container, which no script could build either: the element converters are
    # module-scoped, so `typed_array(node){...}` cannot be spelled outside the Godot package.
    # The length rather than the container: a `typed_array(t)` has no entry in the host's reference
    # table, so it has no spelling as a *return* across the ABI. What is being asked here is that
    # the container can be built and filled at all, which is what had no spelling before.
    MakeChildListLength<public>(First:node, Second:node):int =
        Made := MakeNodeArray()
        Made.Add(First)
        Made.Add(Second)
        Made.Length()

    # And that what went in comes back out at its own type.
    MakeChildListFirstName<public>(First:node, Second:node)<decides>:string =
        Made := MakeNodeArray()
        Made.Add(First)
        Made.Add(Second)
        Made.Get[1].GetName()

    # --- R-INT-4 / R-SIG-3: a Verse function as a Godot Callable -------------
    #
    # The reference table pointing the other way: Godot holds a Verse value. The handler writes a
    # var so the call is observable from GDScript afterwards -- a Callable made this way returns
    # nothing, which is what a signal handler is.
    var CallbackSeen<public>:int = 0

    OnCalled<public>(Value:int)<transacts>:void =
        set CallbackSeen = Value

    OnCalledTwice<public>(First:int, Second:int)<transacts>:void =
        set CallbackSeen = First * 100 + Second

    MakeOnCalled<public>()<transacts>:callable = MakeCallable(int, OnCalled)

    # Two parameters against a one-tuple-parameter callback, which is the whole reason a signal
    # needs no per-arity family: a Verse function's parameter *is* its tuple.
    MakeOnCalledTwice<public>()<transacts>:callable = MakeCallable(tuple(int, int), OnCalledTwice)

    ReadCallbackSeen<public>()<transacts>:int = CallbackSeen

    # --- OQ-11 / R-SCN-3: the math types, as ordinary Verse ------------------
    #
    # No handle and no ABI: Verse has type-based extension methods and definable operators, so
    # `Position + Velocity * Delta` is Verse code over Verse data. Godot's own C# does the same.
    VecAdd<public>(A:vector2, B:vector2)<transacts>:vector2 = A + B
    VecScale<public>(A:vector2, S:float)<transacts>:vector2 = A * S
    VecLength<public>(A:vector2)<transacts>:float = A.Length()
    VecDot<public>(A:vector2, B:vector2)<transacts>:float = A.Dot(B)

    # Godot's own edge case, not merely its formula: normalizing a zero vector answers zero rather
    # than NaN, which is what core/math/vector2.cpp does.
    VecNormalized<public>(A:vector2)<transacts>:vector2 = A.Normalized()

    # `Radians` rather than `Angle`: an extension method is a module-level definition, so
    # `(V:vector2).Angle()` declares `operator'.Angle'` and a local named `Angle` is ambiguous with
    # it. That is the same rule Verse's own `Abs`, `Clamp` and `Length` already impose.
    VecRotated<public>(A:vector2, Radians:float)<transacts>:vector2 = A.Rotated(Radians)

    # --- R-SCN-3: constants, statics, and the one dispatched utility family --
    #
    # Verse has no constant and no static *on* a type, so each class's go into an inline module of
    # its own: `NodeStatics.NotificationReady`, `Vector2Statics.Up`. The `...Statics` suffix is
    # load-bearing -- a module named `Node` or `Tree` would be a top-level name an author reaches
    # for, and Verse refuses a local that resolves ambiguously against one.
    ReadyNotification<public>()<transacts>:int = NodeStatics.NotificationReady
    UpVector<public>()<transacts>:vector2 = Vector2Statics.Up
    ZeroVector<public>()<transacts>:vector2 = Vector2Statics.Zero

    # The random family is the one exception to R-AUD-2: Verse has `GetRandomFloat`, but a
    # Verse-side RNG would silently ignore Seed, so these steer Godot's own stream. Seeding and
    # asking twice is what proves it is Godot's.
    SeededRandom<public>(Base:int)<transacts>:int =
        GodotStatics.SeedRandom(Base)
        GodotStatics.RandiRange(0, 1000000)

    # Object's reflection, which is the rest of what mirroring it bought.
    ClassOf<public>(Target:object):string = Target.GetClass()

    NodeNameOf<public>(Target:node):string = Target.GetName()

    NodeClassOf<public>(Target:node):string = Target.GetClass()

    # The other direction, and the reason a predicate being `<decides>` costs an author nothing:
    # `logic{...}` is Verse's own macro for the failable-to-logic conversion, the inverse of `X?`.
    CanCall<public>(Target:object, Method:string):logic = logic{Target.HasMethod[Method]}

    # --- Godot's Object.to_string, as Verse's own ToString --------------------
    #
    # Not a method: `object.ToString()` is ambiguous with /Verse.org/Verse's ToString even though one
    # takes nothing and the other takes a string. A module-level overload is better than a renamed
    # method anyway, because Verse's string interpolation desugars to exactly this name -- so
    # `"{Node}"` prints what Godot would print, with nothing written to make it.
    Described<public>(Target:object):string = ToString(Target)

    Interpolated<public>(Target:object):string = "<{Target}>"

    # --- R-LANG-6: a library file, resolved with no import -------------------
    #
    # HelperDouble and HelperAnswer live in helpers.verse, which declares no class and so cannot go
    # on a node. One flat scope is what makes them visible here with nothing written to import them.
    CallHelper<public>(Value:int):int = HelperDouble(Value)
    ReadHelperConstant<public>():int = HelperAnswer
    CallHelperString<public>(Name:string):string = HelperGreeting(Name)

    # --- a variant as a script method's own parameter and return type ---------
    #
    # Everything that names a `variant` today is a *native* declaration, marshalled by
    # GodotBindings; whether the ordinary script-call wire carries one is a different question,
    # and R-NODE-10's _Get and _Set are what need the answer.
    EchoVariant<public>(V:variant):variant = V

    VariantKindName<public>(V:variant):string =
        case (VariantKind(V)):
            variant_type.TypeInt => "int"
            variant_type.TypeString => "string"
            variant_type.TypeNil => "nil"
            _ => "other"

    NilVariant<public>():variant = variant{}

    # --- R-SCN-2: Godot's vararg entry points --------------------------------
    #
    # 33 of Godot's methods are `Variant...` and the mirror carried none of them until Phase 4b
    # stage 6. Each is emitted as two *arities* of one name: the fixed prefix alone, and the prefix
    # plus one `[]variant` tail. Both are exercised here, because the pair is the thing that could
    # be wrong -- a call resolving to the other arity would still compile.
    CallOther<public>(Target:object, Method:string, Value:int)<transacts>:int =
        if (N := Target.Call(Method, array{VariantInt(Value)}).AsInt[]) then N else -1

    # The no-tail arity of the same name.
    CallOtherNoArgs<public>(Target:object, Method:string)<transacts>:string =
        if (S := Target.Call(Method).AsString[]) then S else "?"

    # Two arguments, so the tail is more than a single value dressed as an array.
    CallOtherTwo<public>(Target:object, Method:string, A:int, B:int)<transacts>:int =
        Args := array{VariantInt(A), VariantInt(B)}
        if (N := Target.Call(Method, Args).AsInt[]) then N else -1

    # --- R-INT-9: the same reach, from a `<reads>` function ------------------
    #
    # `Call` and `Callv` are `<transacts>`, because `Object::call` dispatches to anything at all.
    # So until `CallConst` existed, reading one value off a GDScript node pulled `<transacts>`
    # onto every function above it, a file at a time -- docs/dodge-the-creeps.md wall 8. These
    # four are the same calls as the four above with the specifier narrowed, and **the fact that
    # this file compiles is half the assertion**: the effect would be refused at the call site,
    # not here, so a regression reads as a build failure rather than as a wrong number.
    CallOtherConst<public>(Target:object, Method:string, Value:int)<reads>:int =
        if (N := Target.CallConst(Method, VariantInt(Value)).AsInt[]) then N else -1

    # The no-argument arity, which is the one `Method:string` alone has to distinguish: a tuple of
    # one repeated type is an array of it, so without that fixed parameter this and the loose form
    # below would be one overload.
    CallOtherConstNoArgs<public>(Target:object, Method:string)<reads>:string =
        if (S := Target.CallConst(Method).AsString[]) then S else "?"

    # The loose form, with the tail built here.
    CallOtherConstTwo<public>(Target:object, Method:string, A:int, B:int)<reads>:int =
        Args := array{VariantInt(A), VariantInt(B)}
        if (N := Target.CallConst(Method, Args).AsInt[]) then N else -1

    # And the `godot_array` spelling. The array is the caller's, because `MakeArray` is
    # `<transacts>` and could not be called from here -- which is the honest shape: building a
    # Godot container is a write, and only the *call* was ever const.
    CallvOtherConst<public>(Target:object, Method:string, Args:godot_array)<reads>:int =
        if (N := Target.CallvConst(Method, Args).AsInt[]) then N else -1

    # A `<reads>` function calling a `<reads>` function that reaches Godot. This is the one that
    # could not be written at all before, and it is why the addition exists rather than being a
    # convenience: narrowing is contagious downward, so one `<transacts>` leaf used to decide the
    # specifier of everything that reached it.
    SumOtherConst<public>(Target:object, Method:string, A:int, B:int)<reads>:int =
        CallOtherConst(Target, Method, A) + CallOtherConst(Target, Method, B)

    # Object.emit_signal, which is the other half of what stage 6's "done when" names.
    EmitOn<public>(Target:object, Signal:string, Value:int)<transacts>:int =
        ToInt(Target.EmitSignal(Signal, array{VariantInt(Value)}))

    EmitOnNoArgs<public>(Target:object, Signal:string)<transacts>:int =
        ToInt(Target.EmitSignal(Signal))

    # And the *loose* arities, which are what a call written the GDScript way resolves to --
    # `Call("test", VariantInt(1))` rather than `Call("test", array{VariantInt(1)})`. Same
    # call as the array form beside them, which is exactly what the cases assert: the two spellings
    # have to reach the same body, or one of them is resolving to an overload nobody meant.
    CallOtherLoose<public>(Target:object, Method:string, Value:int)<transacts>:int =
        if (N := Target.Call(Method, VariantInt(Value)).AsInt[]) then N else -1

    CallOtherLooseTwo<public>(Target:object, Method:string, A:int, B:int)<transacts>:int =
        if (N := Target.Call(Method, VariantInt(A), VariantInt(B)).AsInt[]) then N else -1

    EmitOnLoose<public>(Target:object, Signal:string, Value:int)<transacts>:int =
        ToInt(Target.EmitSignal(Signal, VariantInt(Value)))

    # A vararg on a *reference* has no fixed parameter in front of its tail, so it takes exactly
    # one loose value and the array form for anything more. Two would be a tuple, and a tuple of
    # variants is `[]variant` -- one type rather than two overloads.
    CallCallableLoose<public>(C:callable, Value:int)<transacts>:int =
        if (N := C.Call(VariantInt(Value)).AsInt[]) then N else -1

    # --- the six vararg methods that live on a *reference* -------------------
    #
    # `Callable.call`, `Callable.bind`, `Signal.emit` and three more are methods of a Godot
    # *builtin type* rather than of an Object, so `VhCallValue` -- which takes an object handle --
    # could never have reached them however they were spelled. ABI 8.7's `VhRefCall` is the route.
    #
    # None of them has a zero-argument arity, and that is the same finding as `GDScript.new`: a
    # Verse function's parameters are its tuple, so `Call()` beside `Call(:[]variant)` is not two
    # arities of one name. `array{}` is what an empty argument list is written as.
    CallCallable<public>(C:callable, Value:int)<transacts>:int =
        if (N := C.Call(array{VariantInt(Value)}).AsInt[]) then N else -1

    # Godot's own currying: `bind` appends its arguments *after* the call's own.
    CallBound<public>(C:callable, Bound:int, Value:int)<transacts>:int =
        Curried := C.Bind(array{VariantInt(Bound)})
        if (N := Curried.Call(array{VariantInt(Value)}).AsInt[]) then N else -1

    CallCallableNoArgs<public>(C:callable)<transacts>:string =
        if (S := C.Call(array{}).AsString[]) then S else "?"

    # A foreign signal -- one a GDScript declared or `add_user_signal` made -- emitted from Verse.
    EmitForeign<public>(Owner:object, Name:string, Value:int)<transacts>:void =
        MakeSignal(Owner, Name).Emit(array{VariantInt(Value)})

    EmitForeignNoArgs<public>(Owner:object, Name:string)<transacts>:void =
        MakeSignal(Owner, Name).Emit(array{})

    # `MakeVariant` -- one builder over `any`, against the 38 named ones. The host decides the lane
    # from what the value says about itself, which is why these go out through a real Godot call
    # rather than being read straight back: the assertion that matters is that Godot received the
    # right *type*, not that Verse round-tripped its own struct.
    MetaFromValue<public>(Target:object, Key:string, Value:int)<transacts>:void =
        if (V := MakeVariant[Value]):
            Target.SetMeta(Key, V)

    MetaFromString<public>(Target:object, Key:string, Value:string)<transacts>:void =
        if (V := MakeVariant[Value]):
            Target.SetMeta(Key, V)

    # The one that proves the mechanism: nothing declared this a vector2. The host read the class off
    # the value and found its layout.
    MetaFromVector<public>(Target:object, Key:string, X:float, Y:float)<transacts>:void =
        if (V := MakeVariant[vector2{X := X, Y := Y}]):
            Target.SetMeta(Key, V)

    # And what it refuses. An array cannot say which of Godot's packings it is, so the failure is the
    # contract rather than a gap -- 1 means it built one, which would be the bug.
    MakeFromArray<public>()<transacts>:int =
        if (V := MakeVariant[array{1, 2, 3}]) then 1 else 0

    # A RID out of a real Godot call. This is the end-to-end form of the lane bug: `get_canvas_item`
    # answers a live RID, and every mirrored method returning one answered 0 while the packer wrote
    # `Ref` and the generated reader read `I0`.
    CanvasRid<public>(Target:canvas_item)<reads>:int = Target.GetCanvasItem().Id

    # And a RID through a variant, which is the same lane one level up.
    RoundTripRid<public>(Value:int)<reads>:int =
        if (R := VariantRid(rid{Id := Value}).AsRid[]) then R.Id else -1

    # A RID is not an int any more, which is the type safety the struct buys: as a bare `int` these
    # two lanes were the same Verse type and nothing caught a mix-up.
    RidIsNotAnInt<public>()<reads>:int =
        if (N := VariantRid(rid{Id := 7}).AsInt[]) then N else -1

    # A `rid` as a *declared* type rather than as a variant payload. This is the other half of the
    # classification question `variant` already had: a type the host does not recognise falls into
    # whatever arm is left, silently. `rid` is not in the math layout table, so if nothing claims it
    # it is read as an ordinary user struct and Godot is asked for a one-field tuple instead.
    TakeRid<public>(R:rid)<reads>:int = R.Id

    GiveRid<public>(Value:int)<reads>:rid = rid{Id := Value}

    # --- R-NODE-9: what a parameter's declared class reaches Godot as ---------
    #
    # A PropertyInfo with no class beside its type is all Godot has to go on, and what it draws
    # for one is `Object` -- in a call hint, in the connect dialog and in the documentation --
    # whatever the declaration said. The bodies are trivial on purpose: what is under test is the
    # description of the method, and nothing here is ever called.

    # A mirrored class, which Godot knows by its own name.
    TakesTimer<public>(Clock:timer):int = 0

    # A class of the project's own that Godot *has* registered, which it knows by the PascalCase
    # of the file's stem rather than by the Verse spelling.
    TakesMarshal<public>(Other:marshal):int = 0

    # One it has not: no @global_class, so nothing can name it, and the nearest mirrored class is
    # reported instead. A name Godot cannot resolve would be worse than a less specific one it
    # can, which is GDScript's own _find_narrowest_native_or_global_class.
    TakesHelper<public>(Helper:marshal_helper):int = 0

    # An optional reference, which is an object to Godot exactly as a plain one is: null is the
    # empty case, and it is the one value every object slot can hold. So the two spellings differ
    # in what Verse makes the body prove and in nothing Godot is told.
    TakesMaybeTimer<public>(Clock:?timer):int = 0

    # And the other end of the call, which asks the same question of the result.
    GivesTimer<public>(Clock:timer):timer = Clock

    # --- what null satisfies, and at which kind of class ----------------------
    #
    # Null is the one value every object slot can hold and the empty option is what Verse spells it
    # as, so an optional parameter takes it whatever class it names. These are called with null and
    # with a real node, because the host resolves the declared class by a lookup per package and it
    # used to make that decision *before* looking at whether a value had arrived at all -- so a
    # parameter of the project's own class refused Godot's own null (`by-hand-findings.md` B34).

    MaybeTimerIsEmpty<public>(Clock:?timer)<transacts>:logic =
        if (Clock?) then false else true

    MaybeMarshalIsEmpty<public>(Other:?marshal)<transacts>:logic =
        if (Other?) then false else true

    # The non-empty half, which is what says the refusal was not simply moved: the object that
    # arrives is the node's own instance, so a member of it reads what that script holds.
    MaybeMarshalCounter<public>(Other:?marshal)<transacts>:int =
        if (M := Other?) then M.Counter else -1

    # A bare one of the project's own classes, which had no spelling on this wire at all.
    MarshalCounter<public>(Other:marshal)<transacts>:int = Other.Counter

    # --- null through a *mirrored* parameter ----------------------------------
    #
    # `Node.set_owner` carries no `"meta": "required"`, so Godot accepts null there and the mirror
    # declares `Owner:?node`. That spelling is the whole of how a Verse script says null to Godot:
    # a class has no value meaning nothing, so before this the call could not be made at all.
    # `GetOwner[]` is the reader, and it is `<decides>` for the same reason in the other direction.

    # Three calls rather than one, because a write to Godot defers to the transaction's commit: a
    # body that set the owner and then read it back saw what was there before it ran, which is the
    # rule `spec.md` records beside R-AUD-1 and is a property of every Godot write, not of this one.

    SetOwnerTo<public>(N:node)<transacts>:void = SetOwner(option{N})

    ClearOwner<public>()<transacts>:void = SetOwner(false)

    HasOwner<public>()<reads>:logic = if (GetOwner[]) then true else false

    # The same metadata at the other end of the call. `SceneTree.get_root` is marked as a result
    # that cannot be null, so the mirror declares it total -- `GetRoot()` and not `GetRoot[]` -- and
    # this body would not compile against a `<decides>` one, which is the whole of what it asserts.
    #
    # The tree is a parameter rather than `GetTree[]`, because the editor-side driver runs its
    # first pass from the SceneTree's own `_init`: the root exists, but a node added to it has not
    # entered the tree yet, so `GetTree` declines there and succeeds in an export. Nothing about
    # this case is meant to be about that.
    RootName<public>(Tree:scene_tree)<transacts>:string = Tree.GetRoot().GetName()

# The unregistered class TakesHelper names. Ordinary Verse -- a helper beside the class the file
# is named after -- and the shape every project writes sooner or later.
marshal_helper := class(node2d):
    Amount<public>:int = 1
