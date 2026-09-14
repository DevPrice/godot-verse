using {/Verse.org/Native}

# The Godot API as ordinary Verse. Nothing here is <native>: every class derives from
# `object`, whose C++ shadow supplies the object representation, and every method bottoms out
# in the handful of native primitives in Godot.native.verse. That is what keeps a mirrored class
# per Godot class from needing a C++ shadow per Godot class.

# --- Variant tags ------------------------------------------------------------

# Godot's Variant::Type, spelled as constants rather than a Verse enum because the tag is a lane
# of `variant` and a native struct's fields are the primitives VNI marshals.
TagNil:int = 0
TagBool:int = 1
TagInt:int = 2
TagFloat:int = 3
TagString:int = 4
TagVector2:int = 5
TagVector2i:int = 6
TagRect2:int = 7
TagRect2i:int = 8
TagVector3:int = 9
TagVector3i:int = 10
TagTransform2D:int = 11
TagVector4:int = 12
TagVector4i:int = 13
TagPlane:int = 14
TagQuaternion:int = 15
TagAabb:int = 16
TagBasis:int = 17
TagTransform3D:int = 18
TagProjection:int = 19
TagColor:int = 20
TagStringName:int = 21
TagNodePath:int = 22
TagRid:int = 23
TagObject:int = 24
TagCallable:int = 25
TagSignal:int = 26
TagDictionary:int = 27
TagArray:int = 28
TagPackedByteArray:int = 29
TagPackedInt32Array:int = 30
TagPackedInt64Array:int = 31
TagPackedFloat32Array:int = 32
TagPackedFloat64Array:int = 33
TagPackedStringArray:int = 34
TagPackedVector2Array:int = 35
TagPackedVector3Array:int = 36
TagPackedColorArray:int = 37
TagPackedVector4Array:int = 38

# --- Global functions --------------------------------------------------------

# Godot's is_instance_valid, spelled the way @GlobalScope spells it rather than as a method on
# the thing being tested, which is how GDScript reads it: `is_instance_valid(node)`. Fails once
# Godot has freed the object; reaching through the handle after that is a runtime error, not a
# failure, so this is the guard to test first when a reference has been stored across frames.
#
# `<decides>` rather than a `logic` return, so that the guard composes with what it guards:
# `if (IsInstanceValid[Node], Parent := Node.GetParent[])` is one condition. A `logic` return
# forced a `?` on every call and read as a value to inspect rather than a question to ask.
#
# Hand-written rather than generated with the rest of the API. Godot's own signature takes a
# Variant, so a faithful mirror would read `IsInstanceValid(Value:variant)` and hand a script the
# one type this package deliberately keeps to itself.
IsInstanceValid<public>(Object:object)<decides><reads>:void = VhIsValid(Object.Handle)?

# Godot's Object.to_string, as Verse's own ToString rather than as a method.
#
# Verse's string interpolation desugars to this name -- `"{X}"` becomes `ToString(X)` -- so declaring
# it here is what makes `Print("hit {Body}")` print what Godot would print, rather than nothing or a
# handle. A *method* could not have done that, and could not have existed anyway: `object.ToString()`
# is ambiguous with /Verse.org/Verse's ToString even though one takes nothing and the other takes a
# string, because arity does not separate a name from Verse's own.
#
# An overload of a stdlib name in this package, which is the same thing the enums' `ToInt` is.
ToString<public>(Value:object)<transacts>:string = VhToString(VhCallValue(Value.Handle, "to_string", array{}))

# --- variant packing ---------------------------------------------------------

# Each of these names the lanes its tag reads and lets the struct default the rest, which is the
# whole reason `variant` is a struct rather than the twenty-three element tuple it would otherwise
# have to be: every packer would spell all twenty-three.
#
# The float lanes carry a math type's components in Godot's own order, and the integer lanes carry
# the integer vectors -- a double stops counting exactly above 2^53, and Godot's are 32-bit values
# that have to survive a round trip.

VhFromLogic(Value:logic)<reads>:variant = variant{Tag := TagBool, I0 := if (Value?) then 1 else 0}
VhFromInt(Value:int)<reads>:variant = variant{Tag := TagInt, I0 := Value}
VhFromFloat(Value:float)<reads>:variant = variant{Tag := TagFloat, F0 := Value}
VhFromString(Value:string)<reads>:variant = variant{Tag := TagString, Text := Value}
VhFromStringName(Value:string)<reads>:variant = variant{Tag := TagStringName, Text := Value}
VhFromNodePath(Value:string)<reads>:variant = variant{Tag := TagNodePath, Text := Value}
VhFromObject(Value:object)<reads>:variant = variant{Tag := TagObject, Ref := Value.Handle}
VhFromRid(Value:int)<reads>:variant = variant{Tag := TagRid, Ref := Value}

# --- variant unpacking -------------------------------------------------------

# Plain lane reads. Nothing here is failable, because FromWire on the host side fills the lane the
# tag names -- a number Godot spelled as an integer where the mirror declared a float has already
# been widened by the time a variant exists.
VhToLogic(Value:variant)<reads>:logic = if (Value.I0 <> 0) then true else false
VhToInt(Value:variant)<reads>:int = Value.I0
VhToFloat(Value:variant)<reads>:float = Value.F0
VhToString(Value:variant)<reads>:string = Value.Text

# Raises unless the value is what the caller was generated to read.
#
# With fixed lanes a mismatch no longer announces itself: reading a vector2 out of an int-tagged
# variant gives (0.0, 0.0) rather than failing to find a slot, which is silently wrong where the
# encoding this replaced was loudly wrong. So the check is explicit, and it is R-TYPE-5's runtime
# half: the message names both the type expected and the tag that arrived.
VhExpect(Value:variant, Tag:int, Expected:string)<reads>:void =
    if (Value.Tag <> Tag):
        VhTypeMismatch(Expected, Value)

# The one place a Godot value is still allowed to be absent. A null object crosses as an
# object-tagged zero rather than as a missing value, so this is what makes GetParent() at the scene
# root, or a FindChild that misses, an ordinary Verse failure -- and what stops a null from being
# handed back as a live-looking reference with handle 0.
#
# Nullability is a property of the type rather than of the method (spec R-TYPE-4): across Godot's
# 5304 documented value-typed returns exactly one is documented as nullable, and it is editor-only,
# so an object return is the only thing that is `<decides>`.
VhToHandle(Value:variant)<decides><reads>:int =
    Value.Ref <> 0
    Value.Ref

# An object-tagged variant as the Verse object it names, at the class it actually is. Every
# generated object return goes through this and then casts, which is where R-SCN-6 lives: the cast
# is what turns `node` into `animated_sprite2d`, and it can only succeed because the host built the
# object rather than the signature did.
VhObjectFrom(Value:variant)<decides><reads>:vh_object = VhObjectOf(VhToHandle[Value])

# --- reference types ---------------------------------------------------------

# Godot's Array, Dictionary, Callable and Signal, as the references they are. Mutating one of these
# is visible to every other holder, including GDScript -- which is what a Godot author expects and
# what marshalling them by value would silently take away.

# Godot's Array, Dictionary, Callable and Signal, as the references they are. Mutating one of
# these is visible to every other holder, including GDScript -- which is what a Godot author
# expects, and what marshalling them by value would silently take away.
#
# The typed element accessors are generated into GodotClasses.native.verse rather than written
# here, because a script cannot unpack a `variant` itself: the packers are module-scoped so that a
# user cannot hold a raw reference that outlives what it names (R-TYPE-7). So every way in and out
# of a container has to be spelled with the element's own type, and there are too many of those
# pairs to keep by hand.
#
# `array` is a reserved word in Verse, so Godot's Array is `godot_array` -- the one place a
# mirrored name cannot be the Godot one lowered.
#
# callable and signal_ref stay here rather than being generated: they have no elements to index, so
# there is nothing per-type to emit.

callable<public> := class<computes>(godot_ref):
    # R-TYPE-3. The arguments are a Verse array of whatever the callee takes, and the result comes
    # back as one of the container element types the accessors above spell.
    CallInt<public>(Args:[]int)<transacts>:int =
        VhToInt(VhRefInvoke(Ref, for (A : Args) { VhFromInt(A) }))
    CallFloat<public>(Args:[]float)<transacts>:float =
        VhToFloat(VhRefInvoke(Ref, for (A : Args) { VhFromFloat(A) }))
    CallString<public>(Args:[]string)<transacts>:string =
        VhToString(VhRefInvoke(Ref, for (A : Args) { VhFromString(A) }))
    CallVoid<public>(Args:[]int)<transacts>:void =
        VhRefInvokeVoid(Ref, for (A : Args) { VhFromInt(A) })

# Godot's Signal, as a value a script holds: what a `signal` -typed property or return hands over,
# and the only spelling a signal the mirror has no accessor for has -- one a GDScript or C# script
# declared, or one made with `add_user_signal` (R-INT-1, R-SIG-6).
#
# The payload is a Godot **Array** of the emission's arguments rather than a typed `t`, and it has
# to be: there is no declaration here to read a type off. The author unpacks it with the accessors
# a `godot_array` already has -- `Args.GetInt[0]`, `Args.GetObject[1]` -- so a wrong expectation
# fails at the unpack, where it can be seen, rather than silently.
signal_ref<public> := class<computes>(godot_ref):

    # Suspends until this signal is emitted. See signal.Await: the same rules about effects
    # apply, and for the same reason.
    #
    # The waiter is a `signal(godot_array)` built for this wait alone. It is not bound to
    # anything and never emits; it is here because the event has to live on a Verse object the host
    # can hold, and that class is one.
    Await<public>()<suspends>:godot_array =
        Waiter := signal(godot_array){}
        Token := VhSignalRefAwait(Ref, Waiter)
        defer:
            VhSignalAwaitEnd(Token)
        Waiter.Ev.Await()

    # Connects Callback, and answers the thing that disconnects it -- `signal.Subscribe` for
    # a signal that has no mirrored accessor.
    #
    # This is the rollback-safe way to receive a foreign signal, and the reason to reach for it
    # over `Object.Connect`: the host registers an `AutoRTFM::OnAbort` that disconnects, so a
    # failure after the connect leaves nothing behind. `Object.Connect` is an unforgiving direct
    # call into Godot and is in docs/nonatomic-methods.md with the rest.
    Subscribe<public>(Callback(:godot_array):void)<transacts>:cancelable =
        connection{Subscription := VhSignalRefSubscribe(Ref, Callback)}

# --- reference packing -------------------------------------------------------

# Every wrapper is adopted on the way in, which is what puts the UObject shadow behind it and so
# what makes dropping it release the table entry.
VhAdopt(Value:godot_ref)<reads>:void = VhAdoptRef(Value)

VhToArray(Value:variant)<reads>:godot_array =
    Made := godot_array{Ref := Value.Ref}
    VhAdopt(Made)
    Made

VhToDictionary(Value:variant)<reads>:dictionary =
    Made := dictionary{Ref := Value.Ref}
    VhAdopt(Made)
    Made

VhToCallable(Value:variant)<reads>:callable =
    Made := callable{Ref := Value.Ref}
    VhAdopt(Made)
    Made

VhToSignal(Value:variant)<reads>:signal_ref =
    Made := signal_ref{Ref := Value.Ref}
    VhAdopt(Made)
    Made

# --- making one ---------------------------------------------------------------------------

# A fresh, empty Godot container, owned by the value that comes back (R-TYPE-2, R-INT-2).
#
# Without these a script could only ever hold a container Godot handed it, which is what made
# `Callv` unusable -- `godot_array{}` compiles and holds reference 0, which crosses as Nil, so the
# one method that takes an argument list could not be given one. Every other way of reaching a
# Godot API that takes an Array was closed for the same reason.
#
# Adopted on the way out, like every other wrapper: that is what puts the UObject shadow behind it,
# and the shadow is what releases the table entry when Verse drops the value.
MakeArray<public>()<reads>:godot_array =
    Made := godot_array{Ref := VhRefNew(TagArray)}
    VhAdopt(Made)
    Made

MakeDictionary<public>()<reads>:dictionary =
    Made := dictionary{Ref := VhRefNew(TagDictionary)}
    VhAdopt(Made)
    Made

# A Verse function as a Godot Callable (R-TYPE-3's other direction, R-INT-4). What makes every
# callback-taking engine API reachable, and what a signal subscribes with.
#
# Parametric rather than one maker per arity, because a Verse function's parameter *is* its tuple:
# `t` is `tuple()` for a handler that takes nothing, `int` for one that takes a number, and
# `tuple(int, string)` for one written as two parameters -- a multi-parameter function satisfies a
# one-tuple-parameter callback, which is what makes `MakeCallable(tuple(int, string), OnStruck)`
# reach `OnStruck(Damage:int, By:string)`.
#
# `<transacts>` on the callback, and it is load-bearing: a function with *no* specifier carries the
# default effect set, which is wider than `<transacts>` and includes `no_rollback`, so it could not
# be called from where this one will be. A handler therefore says `<transacts>`.
#
# Only a method bound to a script instance is accepted, which is checked at the far end rather than
# here: a function this cannot name answers 0 and the callable names nothing. See
# src/verse_callable.h for why that half of Godot's own design is the one taken.
MakeCallable<public>(t:type, Callback(:t)<transacts>:void)<transacts>:callable =
    Made := callable{Ref := VhCallableFrom(Callback)}
    VhAdopt(Made)
    Made

# Godot's own `Signal(object, "name")` -- the one way to *name* a signal the mirror has no accessor
# for (R-INT-1). `MakeCallable` for the other direction of the same problem.
#
# The name is Godot's own spelling, snake_case: `MakeSignal(Player, "health_changed")`. A signal one
# of the mirrored classes declares has an accessor with a typed payload and should be reached that
# way instead; this is for a signal a GDScript or C# script declared, or one made with
# `add_user_signal`, which is the only kind that has no accessor.
#
# `<reads>` because building a Signal value observes Godot without changing it -- the same rule
# `MakeArray` follows, and for the same reason: nothing else can see the fresh reference yet.
MakeSignal<public>(Owner:object, Name:string)<reads>:signal_ref =
    Made := signal_ref{Ref := VhSignalRefFor(Owner.Handle, Name)}
    VhAdopt(Made)
    Made

VhFromArray(Value:godot_array)<reads>:variant = variant{Tag := TagArray, Ref := Value.Ref}
VhFromDictionary(Value:dictionary)<reads>:variant = variant{Tag := TagDictionary, Ref := Value.Ref}
VhFromCallable(Value:callable)<reads>:variant = variant{Tag := TagCallable, Ref := Value.Ref}
VhFromSignal(Value:signal_ref)<reads>:variant = variant{Tag := TagSignal, Ref := Value.Ref}

# --- packed arrays -----------------------------------------------------------

# Godot's packed arrays are values, but variable-length ones, so they have no lanes in a
# fixed-width variant and ride as a reference like the containers do. What the author sees is a
# Verse array either way: these converters read the whole thing out, and build a fresh one going
# back, so the generated signatures stay `[]float` rather than becoming a handle to unwrap.
VhToInts(Value:variant)<reads>:[]int = VhRefInts(Value.Ref)
VhToFloats(Value:variant)<reads>:[]float = VhRefFloats(Value.Ref)
VhToStrings(Value:variant)<reads>:[]string = VhRefStrings(Value.Ref)

VhFromInts(Tag:int, Values:[]int)<reads>:variant = variant{Tag := Tag, Ref := VhRefFromInts(Tag, Values)}
VhFromFloats(Tag:int, Values:[]float)<reads>:variant = variant{Tag := Tag, Ref := VhRefFromFloats(Tag, Values)}
VhFromStrings(Tag:int, Values:[]string)<reads>:variant = variant{Tag := Tag, Ref := VhRefFromStrings(Tag, Values)}

# The packed arrays whose elements are math structs rather than scalars -- PackedVector2Array
# and its three siblings. One variant per element, because a vector2 has no single lane to
# flatten into the way an int does.
VhFromValues(Tag:int, Values:[]variant)<reads>:variant = variant{Tag := Tag, Ref := VhRefFromValues(Tag, Values)}

VhFromByteArray(Values:[]int)<reads>:variant = VhFromInts(TagPackedByteArray, Values)
VhFromInt32Array(Values:[]int)<reads>:variant = VhFromInts(TagPackedInt32Array, Values)
VhFromInt64Array(Values:[]int)<reads>:variant = VhFromInts(TagPackedInt64Array, Values)
VhFromFloat32Array(Values:[]float)<reads>:variant = VhFromFloats(TagPackedFloat32Array, Values)
VhFromFloat64Array(Values:[]float)<reads>:variant = VhFromFloats(TagPackedFloat64Array, Values)
VhFromStringArray(Values:[]string)<reads>:variant = VhFromStrings(TagPackedStringArray, Values)

# --- signals -----------------------------------------------------------------

# A signal a script declares, as a typed member (R-SIG-1). The member's *type* is the declaration
# and its *name* is the signal's name, so there is no second place to spell it and nothing to
# drift:
#
#     player := class(area2d):
#         Hit<public>:signal() = signal(){}
#         Struck<public>:signal(tuple(int, string)) = signal(tuple(int, string)){}
#
# Shaped like Verse's own `listenable` rather than implementing it, and the difference is not a
# preference: `signalable.Signal` is `no_rollback`, so it would be uncallable from inside the
# transaction every Godot callback runs in. `cancelable` is the one of the four whose domain fits,
# and Subscribe answers it.
#
# The payload is one type, and tuples carry arity above one: a multi-parameter Verse function
# satisfies a one-tuple-parameter callback, because a Verse function's parameter *is* its tuple. So
# `OnStruck(Damage:int, By:string)` subscribes to a `signal(tuple(int, string))` and Godot
# still sees two arguments.
#
# The neighbour to keep it apart from is `signal_ref` above, which is Godot's Signal *value* -- a
# reference to a signal someone else declared, with no type to read a payload off. This one is the
# declaration, and a mirrored engine-signal accessor answers one of these too.
signal<public>(t:type) := class(vh_signal):

    # What an awaiting task suspends on, and what the host signals when Godot emits (R-SIG-5).
    #
    # Verse's own `event(t)`, held rather than implemented. That is what makes `Await` below
    # ordinary parametric Verse with no native and no ABI behind it -- a native cannot be
    # parametric in its return type, and a parametric `Await` native was the one thing the phase
    # budgeted for and turned out not to need. `event` also brings what a bridge would otherwise
    # owe: FIFO resumption, a `Defer` that drops a cancelled awaiter, and GC roots for the tasks
    # suspended on it.
    #
    # Nothing listens to it until something awaits. `Await` connects to Godot for the duration of
    # one wait and disconnects on the way out, resumed or cancelled.
    Ev<public>:event(t) = event(t){}

    # Emits, now. See VhSignalEmit for why "now" is the exception it is.
    Signal<public>(Val:t)<transacts>:void = VhSignalEmit(Id, Val)

    # Connects Callback and answers the thing that disconnects it. Two subscriptions of one handler
    # are two connections with two independent cancels -- equality is by reference, which is what
    # Godot's own lambda callables do and what UEFN's event does.
    #
    # The callback carries **no** effect specifier, matching Verse's own
    # `subscribable<native>(t:type) := interface: Subscribe<public>(Callback(:t):void)<transacts>`.
    # Phase 4 wrote `<transacts>` here on the reading that Verse's own fixes its callback at a
    # domain that could not touch Godot; the lattice is the other way up -- the default effect set
    # *contains* transacts -- and the narrow spelling was the one thing stopping a signal handler
    # from `spawn`ing a task, which is what a game-over sequence is made of. Widening breaks
    # nothing: effects are contravariant in a parameter, so a handler that still says `<transacts>`
    # satisfies this unchanged.
    #
    # Why Epic's own `signalable.Signal` is wide while `subscribable.Subscribe` is not: `Signal`
    # *invokes* the callback, and a `<transacts>` body may not invoke a wide one. That does not
    # reach us -- the host invokes subscribers from C++ through `VFunction::Invoke`, where Verse's
    # effect checking is not in the way.
    Subscribe<public>(Callback(:t):void)<transacts>:cancelable =
        connection{Subscription := VhSignalSubscribe(Id, Callback)}

    # Suspends until Godot emits this signal, and answers the payload -- typed, because `t` is the
    # payload's own type and the event carries it (R-SIG-5, R-ASYNC-2).
    #
    #     MessageTimer.Timeout().Await()
    #     GetTree().CreateTimer(1.0).Timeout().Await()
    #     Hit.Await()                                   # the script's own
    #
    # One method covers a script's own declared signals *and* all 489 mirrored engine-signal
    # accessors, because every one of them answers a `signal(t)`.
    #
    # **No effect specifier, and there cannot be one:** `awaitable.Await` carries `no_rollback`
    # exactly as `signalable.Signal` does, so a `<suspends><transacts>` body that awaits is glitch
    # 3512. Which means a caller has to be wide too -- a mirrored virtual override is (the mirror
    # generates them with no specifier), and after the widening above so is a signal handler.
    # `spawn` is how a script reaches this from one; a `<reads>` body cannot reach it at all.
    #
    # The Godot connection lives exactly as long as the wait. `defer` runs on the way out whether
    # the task resumed or was cancelled -- by `race`, by the node being freed, by a rebuild -- which
    # is what keeps a cancelled wait from leaving a connection behind.
    Await<public>()<suspends>:t =
        Token := VhSignalAwait(Self)
        defer:
            VhSignalAwaitEnd(Token)
        Ev.Await()

# `signal()` for `signal(tuple())`, which is the payload most declarations have and
# which was otherwise spelled twice on every line of them:
#
#     Hit<public>:signal() = signal(){}
#
# Exactly how `/Verse.org/Concurrency` spells its own -- `listenable<public>() := listenable(tuple())`
# in Listenable.native.verse -- rather than an invention.
signal<public>() := signal(tuple())

# What Subscribe answers. Verse's own `cancelable` is implementable -- its `Cancel` is
# `<transacts>` rather than `no_rollback` -- so this is Verse's vocabulary rather than an invention.
#
# Named for what it is on Godot's side rather than for the method that answers it: `Object.connect`
# makes one and `get_signal_connection_list` counts them, and the editor gutter calls them
# connections. The method stays `Subscribe`, which is Verse's word for making one (R-AUD-2).
connection<public> := class(cancelable):
    Subscription:int = 0

    Cancel<override>()<transacts>:void = VhSignalCancel(Subscription)

# --- typed containers --------------------------------------------------------

# Godot's Array with an element type, which is what `typedarray::Node` means and what
# `GetChildren()` returns. R-TYPE-2, and the fix for the gap that mattered most: a plain
# godot_array offers ten typed accessors and none of them is an object, so walking children --
# which is what scene code is -- had no spelling at all.
#
# An ordinary parametric Verse class over the same reference id godot_array holds. The element
# conversion is a function *value* rather than an overridden method, so one class covers every
# element type instead of one generated wrapper class per type.
#
# `Unpack` is <public> because it has to be: a required data member may be no less accessible than
# the constructor, and the class is public so a script can name `typed_array(node)` in a signature.
# That costs nothing, because the gate on building one is `Ref` -- inherited from godot_ref, never
# public, and worthless at its default of 0. A script can write `typed_array(node){Unpack := ...}`
# and get a container naming nothing.
typed_array<public>(t:type) := class<computes>(godot_ref):
    Unpack<public>:type{_(:variant)<decides><reads>:t}
    Pack<public>:type{_(:t)<reads>:variant}

    # How many elements it holds.
    Length<public>()<reads>:int = VhRefSize(Ref)

    # Element at Index. Failable for two reasons that read the same to the caller: the index is out
    # of range, or the element is not a t after all -- Godot's typed arrays are checked on write,
    # but a null object is still a miss.
    Get<public>(Index:int)<decides><reads>:t = Unpack[VhRefGet[Ref, VhFromInt(Index)]]

    # The whole thing as a Verse array, which is a copy: elements that do not convert are dropped
    # rather than failing the call, the way `for` with a failable body already reads.
    ToArray<public>()<reads>:[]t = for (V : VhRefValues(Ref), E := Unpack[V]) { E }

    # Writes through to the container Godot owns, which every other holder sees. An index one past
    # the end appends; further than that is the error GDScript makes it.
    Set<public>(Index:int, Value:t)<transacts>:void = VhRefSet(Ref, VhFromInt(Index), Pack(Value))

    # Appends. One past the end is the only index that grows a Godot container -- its indexed
    # setter refuses anything further rather than resizing -- so this is Set at exactly that index,
    # and it is how a container built in Verse gets its contents.
    Add<public>(Value:t)<transacts>:void = VhRefSet(Ref, VhFromInt(VhRefSize(Ref)), Pack(Value))


# Godot's Dictionary with a key and a value type -- `typeddictionary::int;String`. The same shape as
# typed_array with one more converter, because a key has to be packed to be looked up.
typed_dictionary<public>(k:type, v:type) := class<computes>(godot_ref):
    PackKey<public>:type{_(:k)<reads>:variant}
    Unpack<public>:type{_(:variant)<decides><reads>:v}
    Pack<public>:type{_(:v)<reads>:variant}

    Length<public>()<reads>:int = VhRefSize(Ref)

    # Failable on both counts, and they read the same to the caller: no such key, or a value that is
    # not a v after all.
    Get<public>(Key:k)<decides><reads>:v = Unpack[VhRefGet[Ref, PackKey(Key)]]

    Set<public>(Key:k, Value:v)<transacts>:void = VhRefSet(Ref, PackKey(Key), Pack(Value))

# --- @GlobalScope utilities that are Godot's behaviour rather than Verse's spelling -------------
#
# R-SCN-3/G11. Godot has 114 utility functions and Verse's own stdlib answers most of them, so under
# R-AUD-2 those keep Verse's spelling and are recorded as skips pointing at it. What is left divides
# in two, and only one half is reachable:
#
#   - These. Their *behaviour* is the engine's -- the editor's error list, Godot's own type names,
#     the object registry, the RID allocator -- so there is nothing for Verse to answer with.
#   - The ones whose parameter or return is a `Variant`: `hash`, `var_to_str`, `str_to_var`,
#     `var_to_bytes`, `type_convert`, `is_same`, `weakref`. A script cannot spell `variant`
#     (R-TYPE-7 keeps the packers module-scoped), so these have no signature a script could call
#     even if they were dispatched. They stay skips, with that as the reason.
#
# The print family is `vararg` in Godot and one argument in practice. These take the one, which is
# what a script writes; the N-argument form is not carried and is recorded as such.

# The editor's error list, which is the whole reason these are not Verse's `Print`: a `push_error`
# shows up in the Debugger panel with a stack, where a print goes to stdout and is gone.
PushError<public>(Text:string)<transacts>:void = { VhCallUtility("push_error", array{VhFromString(Text)}) }
PushWarning<public>(Text:string)<transacts>:void = { VhCallUtility("push_warning", array{VhFromString(Text)}) }

# BBCode, which is the one thing Verse's Print cannot do: `PrintRich("[b]bold[/b]")`.
PrintRich<public>(Text:string)<transacts>:void = { VhCallUtility("print_rich", array{VhFromString(Text)}) }

# stderr rather than stdout, and the verbose channel that `--verbose` gates.
PrintErr<public>(Text:string)<transacts>:void = { VhCallUtility("printerr", array{VhFromString(Text)}) }
PrintVerbose<public>(Text:string)<transacts>:void = { VhCallUtility("print_verbose", array{VhFromString(Text)}) }

# No newline, which is what a progress indicator writes.
PrintRaw<public>(Text:string)<transacts>:void = { VhCallUtility("printraw", array{VhFromString(Text)}) }

# Godot's own name for a Variant type number and for an Error code -- `TYPE_VECTOR2`,
# `ERR_FILE_NOT_FOUND`. Both are engine tables, so neither has a Verse answer.
#
# `VariantTypeName` rather than `TypeString`, because `TypeString` is already an *enumerator* in two
# generated enums (`property_hint` and `variant_type`) and a module-level function of that name is
# ambiguous with both. Enumerators are a collision class the generator's parameter table does not
# cover, which is why this one is renamed rather than registered.
VariantTypeName<public>(VariantType:int)<reads>:string = VhToString(VhCallUtilityConst("type_string", array{VhFromInt(VariantType)}))
ErrorString<public>(Error:int)<reads>:string = VhToString(VhCallUtilityConst("error_string", array{VhFromInt(Error)}))

# The object registry. `InstanceFromId` is the inverse of every handle this bridge carries, and it
# fails rather than raising for an id Godot has freed -- which is the same bargain every other
# object-returning call makes (R-TYPE-4).
InstanceFromId<public>(Id:int)<decides><reads>:object =
    object[VhObjectFrom[VhCallUtilityConst("instance_from_id", array{VhFromInt(Id)})]]

IsInstanceIdValid<public>(Id:int)<decides><reads>:void =
    VhToLogic(VhCallUtilityConst("is_instance_id_valid", array{VhFromInt(Id)}))?

# The RID allocator, which a script writing against a Server needs and can get nowhere else.
RidAllocateId<public>()<transacts>:int = VhToInt(VhCallUtility("rid_allocate_id", array{}))

# A RID from its integer, which is the other half of `RidAllocateId`. A RID crosses as that integer
# everywhere in the mirror, so this is an identity on the wire and a *validation* in Godot.
RidFromInt64<public>(From:int)<reads>:int = VhToInt(VhCallUtilityConst("rid_from_int64", array{VhFromInt(From)}))
