using { /Godot.org/Godot }

# R-SIG-1's refusals: four members that compile and cannot work.
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
    @export_signal
    Fine<public>:event(int) = event(int){}

    # VH_SIGNAL_IS_VAR. A signal is an identity, not a value: the binding is minted once against
    # the object the member was constructed on, so reassigning the member leaves the name pointing
    # at a row nothing emits through.
    @export_signal
    var Reassignable<public>:event(int) = event(int){}

    # There is no access-level refusal here, and its absence is the point: a member's access level
    # is not tested at all. `signals.verse` carries the four non-public members that register, and
    # says why connecting was never a Verse-visibility question.

    # VH_SIGNAL_PAYLOAD_NESTED_STRUCT. A struct payload decomposes one level, into one Godot
    # argument per top-level field. Godot has no argument shape for a struct, so `Inner` has nothing
    # to become.
    @export_signal
    Nested<public>:event(reject_outer) = event(reject_outer){}

    # VH_SIGNAL_PAYLOAD_UNSUPPORTED. An option around something that is not a Godot object: the
    # wire reads a cleared option as a null reference, so `false` and `option{3}` would arrive
    # indistinguishable.
    @export_signal
    Maybe<public>:event(?int) = event(?int){}

    # VH_SIGNAL_NEEDS_ATTRIBUTE. Well formed in every other way -- it is the missing
    # `@export_signal` and nothing else, which is what makes it the fixture for the reject rather
    # than for the ladder above it. A `signal(t)` has no purpose but Godot, so opting out is far
    # likelier to be a forgotten line than a decision, and the alternative to this warning is a
    # Node panel that is empty for no stated reason.
    Forgotten<public>:signal(int) = signal(int){}

    EmitFine<public>(Points:int)<transacts>:void = Fine.Emit(Points)

    # Emitting a refused signal is the runtime half: the binding carries its reject reason, so each
    # of these says *its own* reason rather than the generic "names nothing" they all used to say.
    # One per reason, because "it was rejected" and "it was rejected for the right reason" are
    # different claims and only the second is worth anything to the author reading the message.
    EmitReassignable<public>(Points:int)<transacts>:void = Reassignable.Emit(Points)
    EmitNested<public>()<transacts>:void = Nested.Emit(reject_outer{})
    EmitMaybe<public>()<transacts>:void = Maybe.Emit(option{3})
    EmitForgotten<public>(Points:int)<transacts>:void = Forgotten.Signal(Points)
