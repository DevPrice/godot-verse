using { /Godot.org/Godot }

# R-SIG-1/2/3/4/6: a signal a script declares, emits, and subscribes to.
#
# There is no `@signal` attribute. The member's *type* is the declaration and its *name* is the
# signal's name, so there is no second place to spell either and nothing to drift. A bridge
# attribute exists where the text is the only source -- `@global_class` survives without
# compilation because Godot asks about files it has only scanned -- and a signal list is not one of
# those cases: the host reads declared types out of the semantic program already.
# A struct payload's fields, which is the one spelling that carries *names* to Godot. R-TYPE-7:
# Verse forbids a non-public struct field, so every one of these is `<public>` because it must be.
strike_report := struct:
    Damage<public>:int = 0
    By<public>:string = ""
    Point<public>:vector2 = vector2{}

@global_class
signals := class(node2d):

    # No payload. `tuple()` is the empty tuple, and Godot sees a signal with no arguments.
    Hit<public>:signal() = signal(){}

    # One argument, named for its type because Verse tuples cannot name their elements.
    Scored<public>:signal(int) = signal(int){}

    # Two, as `Arg0` and `Arg1`. A handler still writes two parameters: a multi-parameter Verse
    # function satisfies a one-tuple-parameter callback, because a function's parameter *is* its
    # tuple.
    Struck<public>:signal(tuple(int, string)) = signal(tuple(int, string)){}

    # An object payload, which is what `body_entered` shaped signals carry.
    Touched<public>:signal(node2d) = signal(node2d){}

    # A struct payload: one Godot argument per top-level field, *named by the field*. This is the
    # spelling that gives the connect dialog and "Make Function" real names instead of `Arg0`, and
    # it is why the mapping is not simply "one payload, one argument".
    #
    # It crosses in both directions, and the two are not symmetric. Outbound the struct is taken
    # apart into three Godot arguments; inbound Godot calls the handler with those three and the
    # host builds the struct back, because `Subscribe` is `Callback(:t)` and the handler declares
    # one value. Verse already reads a multi-parameter function as satisfying a one-tuple-parameter
    # callback for the same reason -- a function's parameter *is* its tuple -- so this is that rule
    # reaching the one spelling that carries names.
    Reported<public>:signal(strike_report) = signal(strike_report){}

    EmitReported<public>(Amount:int, Source:string)<transacts>:void =
        Reported.Signal(strike_report{Damage := Amount, By := Source, Point := vector2{X := 3.0, Y := 4.0}})

    var ReportedDamage<public>:int = 0
    var ReportedBy<public>:string = ""
    var ReportedX<public>:float = 0.0

    # One parameter against three Godot arguments. Every field is read, so a field written into the
    # wrong slot is a wrong value rather than something that round-tripped unexamined.
    OnReported<public>(Report:strike_report)<transacts>:void =
        set ReportedDamage = Report.Damage
        set ReportedBy = Report.By
        set ReportedX = Report.Point.X

    SubscribeToReported<public>()<transacts>:void =
        Reported.Subscribe(OnReported)

    ReadReportedDamage<public>()<transacts>:int = ReportedDamage
    ReadReportedBy<public>()<transacts>:string = ReportedBy
    ReadReportedX<public>()<transacts>:float = ReportedX

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

    # Godot's own 489 signals, as an accessor per signal per class: the same `signal(t)` the
    # declaration gives, bound to that handle and that Godot name. This is what makes a scene-file
    # connection optional -- `Timer.Timeout().Subscribe(OnTimeout)` is a spelling now -- and it is
    # what Phase 5's `Timer.Timeout().Await()` will be built on.
    var Renames<public>:int = 0

    # No parameters at all, against a `tuple()` payload: a Verse function's parameter *is* its
    # tuple, so a nullary function and a one-empty-tuple-parameter callback are the same thing.
    OnRenamed<public>()<transacts>:void =
        set Renames = Renames + 1

    SubscribeToRename<public>()<transacts>:void =
        Renamed().Subscribe(OnRenamed)

    ReadRenames<public>()<transacts>:int = Renames

    # R-SIG-6: receiving a signal a *GDScript* node emits. `Subscribe` is for a signal this class
    # declares; this is the general form underneath it, and it is the same Callable either way.
    ConnectTo<public>(Other:object, Name:string)<transacts>:void =
        Other.Connect(Name, MakeCallable(int, OnScored))
