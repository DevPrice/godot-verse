using { /Godot.org/Godot }

# R-ASYNC-1 and R-ASYNC-4, with no Godot behind them.
#
# The integration project can say that awaiting a Godot signal works; only this layer can say what
# happens to two *instances* of one class when one of them raises, because it is the only place
# where a call and a tick can be put in a chosen order with nothing else running.
#
# The event is Verse's own, awaited directly rather than through `signal.Await` -- the Godot
# half of that needs a Godot, and what is being asked here is about scopes rather than signals.

tasks := class(object):

    Ev:event(int) = event(int){}

    var Seen<public>:int = 0
    var Napped<public>:int = 0
    var Reached<public>:int = 0

    # Suspends until Signal, which is what makes it observable that the task survived a tick and
    # that a *different* instance's raise did not take it with it.
    Waiter()<suspends>:void =
        set Reached = 1
        X := Ev.Await()
        set Seen = X

    StartWait<public>():void =
        spawn{Waiter()}

    Fire<public>(X:int):void =
        Ev.Signal(X)

    ReadSeen<public>()<transacts>:int = Seen
    ReadReached<public>()<transacts>:int = Reached

    # The one thing that has no Godot event behind it, so `vh_tick` is what resumes it (D4).
    Napper()<suspends>:void =
        Sleep(0.0)
        set Napped = Napped + 1

    StartNap<public>():void =
        spawn{Napper()}

    ReadNapped<public>()<transacts>:int = Napped

    # An archetype of a mirrored class Godot will not instantiate, which since R-NODE-3 is the
    # cheapest raise a script can spell on purpose: the archetype asks Godot for the object and an
    # abstract class has none to give. It terminates this instance's scope and nothing else's.
    #
    # `node2d{}` was the old spelling, back when it was a handle of 0 that every call found freed.
    # Here it would still raise -- this harness reports every handle dead -- but it would also mint
    # a peer, and the peer arithmetic in objects.verse is worth keeping honest.
    Raise<public>()<transacts>:void =
        Refused := viewport{}
        Refused.QueueFree()

    # The same, from inside a task rather than from a call. The raise unwinds the task; what is
    # asked afterwards is whether the instance can still be called and can still start another.
    Raiser()<suspends>:void =
        Sleep(0.0)
        Raise()

    StartRaiser<public>():void =
        spawn{Raiser()}
