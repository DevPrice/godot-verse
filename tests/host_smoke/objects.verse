using { /Godot.org/Godot }

# R-NODE-3, with no Godot behind it. What the harness supplies instead of an engine is a counter:
# every InstantiateClass it is asked for answers a fresh fake id and every ReleaseObject gives one
# back, so "how many peers does this project hold" is a number the driver can read at any point.
#
# That is the only place the two questions this phase turns on can be asked. One is arithmetic --
# instantiating a *scripted node* must mint nothing, because the host already made that node and an
# unconditional mint would leak one Godot object per node while the scene looked entirely normal.
# The other is a lifetime: a peer is released when the Verse value holding it is collected, and a
# collection is something only vh_collect_garbage can make happen.

# A plain Verse class with a Godot base and no node anywhere in sight. `helper{}` below is the
# whole feature.
helper := class(ref_counted):
    Tag<public>:int = 0

    Doubled<public>()<computes>:int = Tag * 2

# Two deep, because a script may extend another script and the block clause is declared on neither.
sub_helper := class(helper):
    Extra<public>:int = 0

objects := class(object):

    # Held across calls, so the driver can ask about a peer that is still reachable and then about
    # one that is not.
    var Kept<public>:?helper = false

    MakeOne<public>()<transacts>:int =
        H := helper{Tag := 21}
        H.Doubled()

    MakeTwoDeep<public>()<transacts>:int =
        H := sub_helper{Tag := 3, Extra := 4}
        H.Doubled() + H.Extra

    # The peer outlives the call, so the collection the driver forces afterwards must not take it.
    KeepOne<public>()<transacts>:void =
        set Kept = option{helper{Tag := 7}}

    ReadKept<public>()<transacts>:int =
        if (H := Kept?) then H.Tag else -1

    DropKept<public>()<transacts>:void =
        set Kept = false

    # Something to call that touches no peer. The driver calls it between collections so the VM's
    # registers stop naming the object the script has just let go of -- what makes release "within
    # a cycle or two" rather than "on the next one".
    Churn<public>()<transacts>:int = 1

    # A mint inside a computation that then fails. The failure drops the deferred writes; what the
    # driver is asking is whether it also gives the peer back (docs/phase-4b-design.md 4.4).
    MintThenFail<public>()<decides><transacts>:void =
        H := helper{Tag := 99}
        H.Doubled() = -1
