using { /Godot.org/Godot }

# R-EXP-9's receiving half: `@rpc` on a method, which is what Godot's multiplayer reads out of
# `Script.get_rpc_config()` before it will dispatch a remote call to one.
#
# The words are Godot's own and may be written in any order, separated by spaces or commas --
# `any_peer`/`authority` is who may call it, `call_local`/`call_remote` whether the caller runs
# it too, and `reliable`/`unreliable`/`unreliable_ordered` how it travels. A number among them is
# the channel.
#
# **All of it is one string, and that is not a style choice -- it is the toolchain's, and it says
# so.** An attribute site *references* its constructor before calling it, and Verse refuses to
# reference an overloaded function at all -- "Referencing an overloaded function without immediately
# calling it is **not yet implemented**", naming every candidate. So four arities of `rpc` cannot
# exist and the words travel together. Worth re-checking whenever the engine moves: when that lands,
# or when SOL-972's compile-time evaluation of attributes does, GDScript's own
# `@rpc("any_peer", "call_local")` becomes writable and is the spelling to move to.
#
# **There is no bare `@rpc` either.** An attribute with no argument has to be the attribute
# *class*, and the class is what the constructor builds; the two cannot share a name. GDScript's
# own default is spelled out instead, as `@rpc("authority")`, which says the same thing aloud.
#
# The sending half needs no attribute at all: `Node.Rpc` and `Node.RpcId` are two of the 15 vararg
# methods stage 6 generated, and `SendTakeDamage` below is what a call looks like.
@global_class
rpcs := class(node2d):

    var Damage<public>:int = 0

    var Calls<public>:int = 0

    # Godot's default, said out loud. Only the node's multiplayer authority may call it, it does
    # not run on the caller, and it travels reliably on channel 0.
    @rpc("authority")
    TakeDamage<public>(Amount:int)<transacts>:void =
        set Damage = Damage + Amount
        set Calls = Calls + 1

    # Every field at once, and in an order that is not Godot's own, because the words are matched
    # rather than positional.
    @rpc("unreliable_ordered any_peer call_local 3")
    Nudge<public>(By:int)<transacts>:void =
        set Damage = Damage + By

    # One word from one category, so the other three keep their defaults -- which the host applies
    # rather than the consumer, so there is one statement of what a partial `@rpc` means.
    @rpc("any_peer")
    Ping<public>()<transacts>:void =
        set Calls = Calls + 1

    # No attribute, so this must be absent from the config entirely. Godot reads the keys it is
    # given and nothing else, so an unlisted method is one that cannot be called remotely.
    Ordinary<public>()<transacts>:void =
        set Calls = Calls + 1

    # The sending half, which is `Node.rpc` -- a vararg method, so its arguments are a `[]variant`.
    # Not asserted across a real peer connection here: what a headless single-process run can see
    # is that the call is made and answers, which with no multiplayer peer set is Godot's own
    # ERR_UNCONFIGURED rather than a crash.
    SendTakeDamage<public>(Amount:int)<transacts>:int =
        ToInt(Rpc("TakeDamage", array{VariantFromInt(Amount)}))

    # A word that is not one of Godot's seven. The attribute compiles -- its constructor only has
    # to typecheck -- so without a diagnostic this is a method the author believes is
    # remote-callable and Godot has never heard of. It must be *dropped* from the config rather
    # than registered with whatever survived parsing, which is what the case beside it asserts.
    @rpc("any_peer, callLocal")
    Misspelled<public>()<transacts>:void =
        set Calls = Calls + 1

    ReadDamage<public>()<transacts>:int = Damage
    ReadCalls<public>()<transacts>:int = Calls
