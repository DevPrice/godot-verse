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
