using {/Verse.org/Native}

# Free functions over instance-id handles. These are the whole native surface: the generated
# class API in GodotApi.verse is ordinary Verse that bottoms out here.
#
# Print is the only one a script can see. The rest are what the mirrored class layer is built out
# of, and reaching past that layer to call them trades a compile error for a runtime type
# mismatch: `VhSetValue(Handle, "position", ...)` is the untyped spelling of `set Position`.
# Verse's default access is the enclosing module, which is exactly the three files in this
# directory, so leaving the specifier off is the whole mechanism.

Print<public><native>(Message:string)<transacts>:void

VhIsValid<native>(Handle:int)<reads>:logic

# One Godot Variant on the wire: a fixed-width struct of scalars, with Tag saying which lanes to
# read. Tag is Godot's own Variant::Type.
#
# Fixed width, and every lane a scalar, for three reasons that all follow from one measurement
# (docs/abi-v2-design.md 1a). It is **hashable** -- a Verse array is Comparable and not Hashable, so
# the array-carrying tuple this replaced could not be a map key and `[variant]variant` would not
# compile. It **allocates nothing**: the old `VhFromFloat` built three arrays to carry one number.
# And it needs no nesting, because Godot's only recursive types are its reference types, which ride
# in `Ref` rather than inline -- which is what godot-cpp does and why it never marshals an Array.
#
# The widest value type is Projection at sixteen floats; Vector4i and Rect2i want four ints, and
# they are ints rather than floats because a double stops counting exactly above 2^53.
#
# <public>, but only the *type* is: every lane below leaves the specifier off, so a script can name
# a variant in a signature and still cannot read a lane or build one the host would misread
# (R-TYPE-7, amended in Phase 2). What makes the type public necessary rather than convenient is
# typed_array: a required data member may be no less accessible than the constructor, so
# `Unpack:type{_(:variant)...}` on a public class forces variant public too.
#
# Nothing follows from a script holding one. Every lane reads as its default -- a Tag of 0 is Nil --
# so the worst a hand-built variant can say is "nothing", and the readers in GodotApi.native.verse
# are what turn one into a value.
variant<public><native> := struct<computes>:
	# Godot's Variant::Type. Decides which of the lanes below carry the value.
	Tag<native>:int = 0

	# A reference rather than a value: an object's Godot instance id, or an id in the
	# GDExtension's reference table for an Array, a Dictionary, a Callable, a Signal or a packed
	# array. Nothing here is ever reconstructed from lanes.
	Ref<native>:int = 0

	# Integer lanes: logic, int, Vector2i/3i/4i, Rect2i.
	I0<native>:int = 0
	I1<native>:int = 0
	I2<native>:int = 0
	I3<native>:int = 0

	# Float lanes, in Godot's own component order: Vector2 is (F0, F1), Transform3D is its Basis'
	# nine followed by its origin's three, Projection is four Vector4s.
	F0<native>:float = 0.0
	F1<native>:float = 0.0
	F2<native>:float = 0.0
	F3<native>:float = 0.0
	F4<native>:float = 0.0
	F5<native>:float = 0.0
	F6<native>:float = 0.0
	F7<native>:float = 0.0
	F8<native>:float = 0.0
	F9<native>:float = 0.0
	F10<native>:float = 0.0
	F11<native>:float = 0.0
	F12<native>:float = 0.0
	F13<native>:float = 0.0
	F14<native>:float = 0.0
	F15<native>:float = 0.0

	# String, StringName and NodePath. They differ by Tag rather than by lane: Godot tells them
	# apart and Verse spells all three `string`.
	Text<native>:string = ""

# Raises a Verse runtime error naming the type that was expected. It never returns: the raise
# unwinds to the root failure context, which is what lets the unpackers in GodotApi.native.verse
# be total functions despite bottoming out in failable array indexing.
VhTypeMismatch<native>(Expected:string, Value:variant)<reads>:void

# Reads and calls happen now; they observe a Godot the transaction cannot roll back.
#
# The effect on each declaration in this file follows one rule, which Phase 4.5 wrote down: a native
# is `<reads>` if calling it cannot change Godot, and cannot change any reference the bridge is
# already holding. Building a *fresh* reference is not a change by that rule -- nothing else can
# observe it yet -- which is why `VhRefNew` and the `VhRefFrom...` family are `<reads>` while
# `VhRefSet` is not. Everything that reaches Godot to mutate it, or that hands Godot something of
# Verse's (`VhCallableFrom`, the signal family), is `<transacts>`.
#
# VhCallValue is total. A dead receiver, an unknown method and a value that does not fit the wire
# all raise inside the host, and a null object comes back as an object-tagged zero rather than as
# a missing value -- so there is nothing left for the caller to handle. Property reads keep their
# failure: Godot cannot tell an absent property from a nil one.
VhCallValue<native>(Handle:int, Method:string, Args:[]variant)<transacts>:variant
VhGetValue<native>(Handle:int, Property:string)<decides><reads>:variant

# The same call, narrowed. 6728 of Godot's 14,933 methods are `const` *and* answer a value, and a
# call to one observes the scene without changing it -- which is `<reads>`, not `<transacts>`. The
# distinction is not cosmetic: an explicit specifier *narrows* in Verse, so a `<reads>` helper a
# script writes may not call a `<transacts>` function, and before this every read of Godot state
# forced `<transacts>` onto its caller and from there onto that caller's callers, a file at a time.
#
# A separate native rather than a narrower `VhCallValue`, because the mutating half genuinely is
# `<transacts>` and one declaration cannot be both. The C++ behind the two is the same call; what
# differs is what the compiler will let a caller be.
#
# **`const` and answering, not `const` alone.** Godot's `const` means "does not mutate the C++
# object", which is not the same as "has no effect": the 38 methods that are const and return
# *nothing* are `OS.set_environment`, `OS.delay_msec`, `CanvasItem.draw_string` and 35 more of that
# shape, and every one of them does something a later read can see. They stay `<transacts>`, and
# docs/phase-4.5-design.md 11 records that its own table called them "honest, trivially".
VhCallValueConst<native>(Handle:int, Method:string, Args:[]variant)<reads>:variant

# Mutations defer to transaction commit, so a failed expression leaves the scene untouched.
VhCallVoid<native>(Handle:int, Method:string, Args:[]variant)<transacts>:void
VhSetValue<native>(Handle:int, Property:string, Value:variant)<transacts>:void

# Engine::get_singleton. Fails for a name Godot does not register, which is what the generated
# accessors in GodotClasses.native.verse are `<decides>` for.
VhSingleton<native>(Name:string)<decides><reads>:int

# The Verse object a Godot handle names, built by the host as the most derived mirrored class the
# object actually is -- or, for a node carrying a Verse script, that script's own object.
#
# This is what makes a downcast mean anything (R-SCN-6). Building the object in Verse instead, as
# `node{Handle := ...}`, gives back the class the *signature* named rather than the class the
# object *is*, so `animated_sprite2d[GetNode("Sprite")]` could never succeed.
#
# Total, and never `false`: a handle Godot has freed, or one whose class this mirror does not
# carry, answers a bare `vh_object`, which every cast then declines. The caller wrote a failable
# cast, so a failure is an answer; raising from here would be a different question's error.
VhObjectOf<native>(Handle:int)<reads>:vh_object

# A Verse function as a Godot Callable, answered as a reference id in the same table an Array rides
# in (R-TYPE-3's other direction, R-INT-4, and what a signal subscribes with).
#
# `any` rather than a function type, because one native has to serve every arity: a Verse function's
# parameter *is* its tuple, so `(:t)->void` covers them all, and a native cannot be parametric. The
# type check is in the public wrapper above it, which is ordinary parametric Verse.
#
# 0 for a value that is not a Verse function bound to a live script instance. That is the only shape
# 4a accepts -- Godot's own plain lambda, the unbound case, is anchored to the script resource and
# is Godot's known leak (GH-102327), so the half taken here is the half that dies with its object.
VhCallableFrom<native>(Callback:any)<transacts>:int

# --- signals -----------------------------------------------------------------

# The binding half of a script-declared signal (R-SIG-1). The typed half is `signal(t)` in
# GodotApi.native.verse, which is ordinary parametric Verse over this -- the same split as
# `vh_object` and `object`, and for the same two reasons: a native class cannot be parametric, and
# Verse cannot reopen a class to add the typed API afterwards.
#
# Id is written by the host at construction, the way Handle is: it names the owner, the signal's
# name and the payload's declared shape, all of which the host read out of the semantic program.
# A `signal` a script builds for itself holds 0 and names nothing, which is the same bargain
# `godot_array{}` makes.
vh_signal<public><native> := class:
    Id<native>:int = 0

# Emission is **immediate**, unlike every other void mutation in the mirror, and that is a stated
# exception rather than an oversight (docs/phase-4-design.md 6.4): handlers run synchronously, as
# they do in GDScript, so "emit, then read what the handler changed" behaves the way a Godot author
# expects. The cost is stated rather than hidden -- if the emitting transaction later aborts, the
# handlers have already run. Phase 4.5 audits that set.
VhSignalEmit<native>(Id:int, Payload:any)<transacts>:void

# Connects, and answers a subscription id -- 0 if the callback is not a method bound to a live
# script instance. Unlike emission this is compensated rather than deferred or immediate-and-
# forgotten: it mutates Godot *and* returns a value, so an aborted transaction has to disconnect,
# which is what the host registers with AutoRTFM::OnAbort. Phase 4 wrote that compensation against
# Verse::Stm::OnRollback and Phase 4.5's S-3 measured it doing nothing; HostScript.cpp says why.
VhSignalSubscribe<native>(Id:int, Callback:any)<transacts>:int

# Idempotent, as `event_subscription.Cancel` is in UEFN: a second Cancel does nothing.
VhSignalCancel<native>(Subscription:int)<transacts>:void

# Binds one of *Godot's own* 489 signals, for the accessors the generator emits per class --
# `Timer.Timeout()`, `Button.Pressed()`. A script-declared signal is bound at construction instead,
# because the host walks the class's members for it; an engine signal has no member to walk.
#
# Class and Accessor name the generated accessor, which is where the payload's declared type comes
# from: its return type is a `signal(t)` instantiation, and the type argument is on it as a
# substitution table -- the same road a member's payload is read by. Name is Godot's own spelling of
# the signal, which is what `connect` and `emit_signal` are given.
#
# One row per (object, signal) rather than one per call, so `Timer.Timeout()` in a loop does not
# grow the table.
VhSignalBind<native>(Handle:int, Class:string, Accessor:string, Name:string)<transacts>:int

# --- awaiting -----------------------------------------------------------------
#
# `signal(t).Await()` is ordinary Verse over `/Verse.org/Verse`'s own `event(t)` (R-SIG-5).
# These two are the whole of what the host owes it: connect a Godot connection that feeds that
# event for the duration of one wait, and take it away again.
#
# The host reaches the event by peeking the `Ev` field off the signal object, which is why the
# object rather than its id crosses: `Timer.Timeout()` mints a *fresh* `signal` on every
# call, and several of them share one binding id, so an id could not say which one is waiting.

# Connects, and answers a token naming the connection. The signal object is held by the host for
# the life of the token, so a temporary the accessor built survives the wait. 0 for a signal that
# names nothing or that Godot was never told about -- the wait then never resumes, which is what
# awaiting a signal nobody emits means anywhere.
VhSignalAwait<native>(Signal:vh_signal)<transacts>:int

# Disconnects and releases what the token named. Idempotent, and reached from the `defer` of the
# awaiting body, so a cancelled wait leaves no connection behind.
VhSignalAwaitEnd<native>(Token:int)<transacts>:void

# The same, for a Godot Signal the mirror has no accessor for -- one a GDScript or C# script
# declared, or one made with `add_user_signal` (R-INT-1). Ref is the signal reference; Waiter is a
# `signal(godot_array)` whose event the emission's arguments arrive on as one Godot Array.
VhSignalRefAwait<native>(Ref:int, Waiter:vh_signal)<transacts>:int

# Connects Callback to a foreign Godot Signal and answers a subscription id, the way
# VhSignalSubscribe does for a mirrored one -- compensated on abort, which is what
# `Object.Connect` cannot be. The handler receives the emission's arguments as one Godot Array.
VhSignalRefSubscribe<native>(Ref:int, Callback:any)<transacts>:int

# Godot's own `Signal(object, "name")`, as a reference id. The direct analogue of VhCallableFrom,
# and needed for the same reason: without it a script could *receive* a Signal and never name one,
# so a signal the mirror has no accessor for would be unreachable however good the machinery behind
# it was. 0 for a handle Godot has freed.
VhSignalRefFor<native>(Handle:int, Name:string)<reads>:int

# --- sleeping ----------------------------------------------------------------

# Suspends the calling task for Seconds of *real* time, resuming at the next `vh_tick` whose
# deadline has passed.
#
# Deliberately not a `SceneTreeTimer`: it has to work where there is no scene tree at all -- the
# smoke test, the probe, a `@tool` script. The cost is stated rather than avoided. It ignores
# `Engine.time_scale`, it counts wall-clock under `--fixed-fps`, and it keeps counting in a paused
# game. For game timing await a Timer or `GetTree[].CreateTimer[Seconds].Timeout()`: those are the
# engine's clock and behave the way a Godot author expects.
#
# A negative Seconds returns immediately; 0.0 means "resume at the next tick", which is the
# cheapest way to yield a frame.
Sleep<public><native>(Seconds:float)<suspends>:void

# A call with no object, for Godot's 114 statics and its `@GlobalScope` utilities: neither has a
# handle to ride VhCallValue on, which is the whole of what OQ-11 asked about the *free function*
# half. (The value-type half turned out to need no ABI at all -- see GodotMath.native.verse.)
VhCallStatic<native>(Class:string, Method:string, Args:[]variant)<transacts>:variant
VhCallUtility<native>(Name:string, Args:[]variant)<transacts>:variant

# `VhCallUtility` for a utility that only looks something up: `type_string`, `error_string`,
# `instance_from_id`, `is_instance_id_valid`, `rid_from_int64`. Judged one at a time rather than
# read off a flag, because `extension_api.json` carries no `is_const` for a utility -- and the
# distinction is real: `randf` and `randomize` move the global RNG, so a `<reads>` spelling of them
# would let a replay silently diverge.
#
# No static counterpart, and none is needed: not one of Godot's 114 statics is `const`.
VhCallUtilityConst<native>(Name:string, Args:[]variant)<reads>:variant

# --- reference values --------------------------------------------------------

# A Godot value the wire cannot copy: an Array, a Dictionary, a Callable, a Signal, or one of the
# ten packed arrays. It holds an id in a table the GDExtension owns, the way `object` holds a Godot
# instance id -- which is godot-cpp's own split, where a value type is copied and a reference type
# is a handle every operation is asked of.
#
# Native so that it has a UObject shadow, and so that dropping the last Verse reference to it
# releases the table entry. That only works for an object that has crossed to native at least once,
# which is why VhAdoptRef exists and why every wrapper is built through it.
godot_ref<public><native> := class<computes>:
    Ref<native>:int = 0

# Forces the UObject shadow into existence and takes ownership of the id.
#
# A Verse object that never crosses to native is a VM cell with no UObject behind it, so nothing
# would ever run to release what it holds. Passing it here is what materialises the shadow; the
# release then happens on the collection that drops it.
VhAdoptRef<native>(Value:godot_ref)<reads>:void

# Element access on an Array, a Dictionary or a packed array. `<decides>` because a key that is
# absent and an index out of range are ordinary misses rather than errors -- which is the whole
# reason a Dictionary read composes into a guard.
VhRefGet<native>(Ref:int, Key:variant)<decides><reads>:variant
VhRefSet<native>(Ref:int, Key:variant, Value:variant)<transacts>:void
VhRefSize<native>(Ref:int)<reads>:int

# A fresh empty container of the Godot type Tag names.
VhRefNew<native>(Tag:int)<reads>:int

# Invokes a Callable. What makes a callback-taking engine API reachable (R-TYPE-3).
VhRefInvoke<native>(Ref:int, Args:[]variant)<transacts>:variant
VhRefInvokeVoid<native>(Ref:int, Args:[]variant)<transacts>:void

# The whole container at once, for the bulk converters. One native function per element shape
# rather than one per Godot type: eleven packed and unpacked types share four payloads.
VhRefInts<native>(Ref:int)<reads>:[]int
VhRefFloats<native>(Ref:int)<reads>:[]float
VhRefStrings<native>(Ref:int)<reads>:[]string
VhRefValues<native>(Ref:int)<reads>:[]variant

# The reverse: a fresh container of Tag holding these.
VhRefFromInts<native>(Tag:int, Values:[]int)<reads>:int
VhRefFromFloats<native>(Tag:int, Values:[]float)<reads>:int
VhRefFromStrings<native>(Tag:int, Values:[]string)<reads>:int
VhRefFromValues<native>(Tag:int, Values:[]variant)<reads>:int

# The one native class in the API, and the root of every mirrored one. Everything above it is
# ordinary Verse, which is what keeps a thousand mirrored classes from needing a thousand C++
# shadows: the VM dispatches through the object's own shape, not through anything native.
#
# It is native because a script's class has to reach C++ somehow -- the host instantiates a user
# subclass and calls into it through a UObject, and only a native ancestor gives the instance a
# UObject representation to be reached by.
#
# `vh_object` rather than `object`, since Phase 2: `object` is now the *mirror* of Godot's own Object
# class, with its 46 reachable methods, and it derives from this. Verse cannot reopen a class, so
# those methods could not be added here; and a generated `godot_object` beside a native `object`
# would have put two base names in completion where the one a user reaches for first is the empty
# one. Nothing a script writes should name this type -- `object` is the base to derive from.
vh_object<public><native> := class:
    # Godot instance id. The host writes it at construction; it never changes afterwards, and it
    # keeps its value after Godot frees the object -- which is what makes IsInstanceValid the only
    # way to tell a live reference from a stale one.
    #
    # Not public, for that same reason: a handle is the one thing that outlives what it names, so
    # a script holding one instead of the object holds a number no guard can check.
    Handle<native>:int = 0

    # Godot's _notification, which is the one script-level hook the generator cannot reach:
    # `_notification` is in no part of extension_api.json -- Object declares zero virtuals there,
    # and neither _to_string, _get, _set nor _get_property_list appears anywhere in it. They are
    # hooks Godot offers to *scripts* rather than methods it registers in ClassDB, so no amount of
    # generator work produces them and R-NODE-8 does not ride R-NODE-7 after all.
    #
    # The rest of that set -- _ToString, _Get, _Set, _GetPropertyList, _ValidateProperty -- is
    # R-NODE-10 and sits in 4b, where _Get and _Set can be designed beside the export machinery
    # they overlap with.
    #
    # What is *not* here any more is Ready, Process and PhysicsProcess. Since Phase 4 every one of
    # Godot's 1413 virtuals is generated onto the class that declares it, so `_Ready` is a member
    # of `node` -- which is where Godot declares it, and which is what stops a Resource-derived
    # script from having a `_Process` it could never receive.
    _Notification<public>(What:int):void = {}

