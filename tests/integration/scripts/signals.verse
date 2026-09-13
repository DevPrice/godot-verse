using { /Godot.org/Godot }

# R-SIG-1/2/3/4/6: a signal a script declares, emits, and subscribes to.
#
# There is no `@signal` attribute. The member's *type* is the declaration and its *name* is the
# signal's name, so there is no second place to spell either and nothing to drift. A bridge
# attribute exists where the text is the only source -- `@global_class` survives without
# compilation because Godot asks about files it has only scanned -- and a signal list is not one of
# those cases: the host reads declared types out of the semantic program already.
@global_class
signals := class(node2d):

    # No payload. `tuple()` is the empty tuple, and Godot sees a signal with no arguments.
    Hit<public>:godot_signal(tuple()) = godot_signal(tuple()){}

    # One argument, named for its type because Verse tuples cannot name their elements.
    Scored<public>:godot_signal(int) = godot_signal(int){}

    # Two, as `Arg0` and `Arg1`. A handler still writes two parameters: a multi-parameter Verse
    # function satisfies a one-tuple-parameter callback, because a function's parameter *is* its
    # tuple.
    Struck<public>:godot_signal(tuple(int, string)) = godot_signal(tuple(int, string)){}

    # An object payload, which is what `body_entered` shaped signals carry.
    Touched<public>:godot_signal(node2d) = godot_signal(node2d){}

    EmitTouched<public>(Body:node2d)<transacts>:void = Touched.Signal(Body)

    EmitHit<public>()<transacts>:void = Hit.Signal(())
    EmitScored<public>(Points:int)<transacts>:void = Scored.Signal(Points)
    EmitStruck<public>(Damage:int, By:string)<transacts>:void = Struck.Signal((Damage, By))

    # --- the subscribing half -------------------------------------------------

    var Seen<public>:int = 0
    var SeenBy<public>:string = ""

    OnScored<public>(Points:int)<transacts>:void =
        set Seen = Seen + Points

    OnStruck<public>(Damage:int, By:string)<transacts>:void =
        set Seen = Damage
        set SeenBy = By

    var First:?cancelable = false
    var Second:?cancelable = false

    # Two subscriptions of one handler are two connections, each with its own cancel: equality is
    # by reference, which is what Godot's own lambda callables do and what UEFN's event does.
    SubscribeTwice<public>()<transacts>:void =
        set First = option{Scored.Subscribe(OnScored)}
        set Second = option{Scored.Subscribe(OnScored)}

    CancelFirst<public>()<transacts>:void =
        if (Sub := First?):
            Sub.Cancel()

    # Idempotent, as event_subscription::Cancel is in UEFN.
    CancelFirstAgain<public>()<transacts>:void =
        if (Sub := First?):
            Sub.Cancel()
            Sub.Cancel()

    ReadSeen<public>()<transacts>:int = Seen
    ReadSeenBy<public>()<transacts>:string = SeenBy
    ResetSeen<public>()<transacts>:void =
        set Seen = 0
        set SeenBy = ""

    # R-SIG-6: receiving a signal a *GDScript* node emits. `Subscribe` is for a signal this class
    # declares; this is the general form underneath it, and it is the same Callable either way.
    ConnectTo<public>(Other:object, Name:string)<transacts>:void =
        Other.Connect(Name, MakeCallable(int, OnScored))
