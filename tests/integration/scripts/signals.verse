using { /Godot.org/Godot }
# `listenable` alone needs this. `event` and `cancelable` arrive through the Godot package's own
# imports; /Verse.org/Concurrency does not, and the compiler names the using when it is missing.
using { /Verse.org/Concurrency }

# R-SIG-1/2/3/4/6: a signal a script declares, emits, and subscribes to.
#
# `@export_signal` is what registers a member with Godot, and the member's *name* is the signal's
# name, so there is no second place to spell that and nothing to drift. The member is Verse's own
# `event(t)`, which also satisfies `awaitable(t)` for code that has never heard of Godot, and whose
# Godot connection is made once at construction and held for the instance's life -- a bare event
# offers no hook at the await to connect from, which is the one place R-SIG-5's
# connect-for-the-length-of-one-wait is traded away.
#
# `signal(t)` still declares a signal and is deprecated for it; `signal_rejects.verse` carries the
# one member left that says so. It remains what the engine's own accessors answer.
# A struct payload's fields, which is the one spelling that carries *names* to Godot. R-TYPE-7:
# Verse forbids a non-public struct field, so every one of these is `<public>` because it must be.
strike_report := struct:
    Damage<public>:int = 0
    By<public>:string = ""
    Point<public>:vector2 = vector2{}

@global_class
signals := class(node2d):

    # No payload. `tuple()` is the empty tuple, and Godot sees a signal with no arguments.
    @export_signal
    Hit<public>:event() = event(){}

    # One argument, named for its type because Verse tuples cannot name their elements.
    @export_signal
    Scored<public>:event(int) = event(int){}

    # Two, as `Arg0` and `Arg1`. A handler still writes two parameters: a multi-parameter Verse
    # function satisfies a one-tuple-parameter callback, because a function's parameter *is* its
    # tuple.
    @export_signal
    Struck<public>:event(tuple(int, string)) = event(tuple(int, string)){}

    # An object payload, which is what `body_entered` shaped signals carry.
    @export_signal
    Touched<public>:event(node2d) = event(node2d){}

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
    @export_signal
    Reported<public>:event(strike_report) = event(strike_report){}

    EmitReported<public>(Amount:int, Source:string)<transacts>:void =
        Reported.Emit(strike_report{Damage := Amount, By := Source, Point := vector2{X := 3.0, Y := 4.0}})

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

    EmitTouched<public>(Body:node2d)<transacts>:void = Touched.Emit(Body)

    EmitHit<public>()<transacts>:void = Hit.Emit(())
    EmitScored<public>(Points:int)<transacts>:void = Scored.Emit(Points)
    EmitStruck<public>(Damage:int, By:string)<transacts>:void = Struck.Emit((Damage, By))

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

    # The member's Godot connection is made once at construction rather than per wait, so an
    # emission raised by *GDScript* on this node reaches a Verse subscriber with nothing awaiting.
    # That is what a bare `event(t)` buys and costs: no hook at the await to connect from, so the
    # connection cannot be scoped to one.
    SubscribeToStruck<public>()<transacts>:void =
        Struck.Subscribe(OnStruck)

    # --- the other spelling, supported alongside -------------------------------
    #
    # `signal(t)` declares a signal too, and both are kept because they are not interchangeable:
    # this one satisfies `listenable(t)` and connects only for the length of a wait, where an
    # `event(t)` satisfies `awaitable(t)`/`signalable(t)` and holds one connection for the
    # instance's life. Godot cannot tell them apart, which is what this case is here to hold true.
    @export_signal
    Chimed<public>:signal(int) = signal(int){}

    # `Signal` rather than `Emit`: on a `signal(t)` the emit verb is the class's own method, and it
    # is `<transacts>` because it bottoms out in a native rather than in `signalable.Signal`.
    EmitChimed<public>(Times:int)<transacts>:void = Chimed.Signal(Times)

    SubscribeToChimed<public>()<transacts>:void =
        Chimed.Subscribe(OnScored)

    # The interop the two spellings differ on, as a type question rather than a behavioural one: a
    # `signal(t)` can be handed to Verse code taking a `listenable(t)` and an `event(t)` cannot.
    ChimedAsListenable<public>()<transacts>:listenable(int) = Chimed

    # R-SIG-6: receiving a signal a *GDScript* node emits. `Subscribe` is for a signal this class
    # declares; this is the general form underneath it, and it is the same Callable either way.
    ConnectTo<public>(Other:object, Name:string)<transacts>:void =
        Other.Connect(Name, MakeCallable(int, OnScored))
