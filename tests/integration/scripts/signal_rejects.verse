using { /Godot.org/Godot }

# R-SIG-1's refusals: four `godot_signal` members that compile and cannot work.
#
# Each one used to be accepted silently -- the member compiled, the signal was absent from Godot,
# and the author found out at the first emission, at runtime, or never. They are decidable from the
# declaration, so the host decides them once in GetClassSignals and reports a vh_signal_reject; the
# editor turns that into a warning at the member's own line and `_get_script_signal_list` drops the
# signal rather than telling Godot about one nothing can emit.
#
# The file is deliberately *valid*: none of these is a compile error, which is exactly the problem.

# A struct whose field is itself a struct, for the nested case below. `stats` would do, but it lives
# in another file and this one should be readable on its own.
reject_inner := struct:
    Value<public>:int = 0

reject_outer := struct:
    Inner<public>:reject_inner = reject_inner{}
    Tag<public>:int = 0

@global_class
signal_rejects := class(node2d):

    # The one that works, so the file proves the pass rejects individually rather than wholesale.
    Fine<public>:godot_signal(int) = godot_signal(int){}

    # VH_SIGNAL_IS_VAR. A signal is an identity, not a value: the binding is minted once against
    # the object the member was constructed on, so reassigning the member leaves the name pointing
    # at a row nothing emits through.
    var Reassignable<public>:godot_signal(int) = godot_signal(int){}

    # VH_SIGNAL_NOT_PUBLIC. Connecting is something done from outside the class, and this member
    # cannot be seen from there. Registered with Godot anyway before this pass existed, which is the
    # opposite of what the design asked for.
    Unseen:godot_signal(int) = godot_signal(int){}

    # VH_SIGNAL_PAYLOAD_NESTED_STRUCT. A struct payload decomposes one level, into one Godot
    # argument per top-level field. Godot has no argument shape for a struct, so `Inner` has nothing
    # to become.
    Nested<public>:godot_signal(reject_outer) = godot_signal(reject_outer){}

    # VH_SIGNAL_PAYLOAD_UNSUPPORTED. An option around something that is not a Godot object: the
    # wire reads a cleared option as a null reference, so `false` and `option{3}` would arrive
    # indistinguishable.
    Maybe<public>:godot_signal(?int) = godot_signal(?int){}

    EmitFine<public>(Points:int)<transacts>:void = Fine.Signal(Points)

    # Emitting a refused signal is the runtime half: the binding carries its reject reason, so each
    # of these says *its own* reason rather than the generic "names nothing" they all used to say.
    # One per reason, because "it was rejected" and "it was rejected for the right reason" are
    # different claims and only the second is worth anything to the author reading the message.
    EmitUnseen<public>(Points:int)<transacts>:void = Unseen.Signal(Points)
    EmitReassignable<public>(Points:int)<transacts>:void = Reassignable.Signal(Points)
    EmitNested<public>()<transacts>:void = Nested.Signal(reject_outer{})
    EmitMaybe<public>()<transacts>:void = Maybe.Signal(option{3})
