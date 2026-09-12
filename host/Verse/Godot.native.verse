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

VhIsValid<native>(Handle:int)<transacts>:logic

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
variant<public><native> := struct:
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
VhTypeMismatch<native>(Expected:string, Value:variant)<transacts>:void

# Reads and calls happen now; they observe a Godot the transaction cannot roll back.
#
# VhCallValue is total. A dead receiver, an unknown method and a value that does not fit the wire
# all raise inside the host, and a null object comes back as an object-tagged zero rather than as
# a missing value -- so there is nothing left for the caller to handle. Property reads keep their
# failure: Godot cannot tell an absent property from a nil one.
VhCallValue<native>(Handle:int, Method:string, Args:[]variant)<transacts>:variant
VhGetValue<native>(Handle:int, Property:string)<decides><transacts>:variant

# Mutations defer to transaction commit, so a failed expression leaves the scene untouched.
VhCallVoid<native>(Handle:int, Method:string, Args:[]variant)<transacts>:void
VhSetValue<native>(Handle:int, Property:string, Value:variant)<transacts>:void

# Engine::get_singleton. Fails for a name Godot does not register, which is what the generated
# accessors in GodotClasses.native.verse are `<decides>` for.
VhSingleton<native>(Name:string)<decides><transacts>:int

# --- reference values --------------------------------------------------------

# A Godot value the wire cannot copy: an Array, a Dictionary, a Callable, a Signal, or one of the
# ten packed arrays. It holds an id in a table the GDExtension owns, the way `object` holds a Godot
# instance id -- which is godot-cpp's own split, where a value type is copied and a reference type
# is a handle every operation is asked of.
#
# Native so that it has a UObject shadow, and so that dropping the last Verse reference to it
# releases the table entry. That only works for an object that has crossed to native at least once,
# which is why VhAdoptRef exists and why every wrapper is built through it.
godot_ref<public><native> := class:
    Ref<native>:int = 0

# Forces the UObject shadow into existence and takes ownership of the id.
#
# A Verse object that never crosses to native is a VM cell with no UObject behind it, so nothing
# would ever run to release what it holds. Passing it here is what materialises the shadow; the
# release then happens on the collection that drops it.
VhAdoptRef<native>(Value:godot_ref)<transacts>:void

# Element access on an Array, a Dictionary or a packed array. `<decides>` because a key that is
# absent and an index out of range are ordinary misses rather than errors -- which is the whole
# reason a Dictionary read composes into a guard.
VhRefGet<native>(Ref:int, Key:variant)<decides><transacts>:variant
VhRefSet<native>(Ref:int, Key:variant, Value:variant)<transacts>:void
VhRefSize<native>(Ref:int)<transacts>:int

# A fresh empty container of the Godot type Tag names.
VhRefNew<native>(Tag:int)<transacts>:int

# Invokes a Callable. What makes a callback-taking engine API reachable (R-TYPE-3).
VhRefInvoke<native>(Ref:int, Args:[]variant)<transacts>:variant
VhRefInvokeVoid<native>(Ref:int, Args:[]variant)<transacts>:void

# The whole container at once, for the bulk converters. One native function per element shape
# rather than one per Godot type: eleven packed and unpacked types share four payloads.
VhRefInts<native>(Ref:int)<transacts>:[]int
VhRefFloats<native>(Ref:int)<transacts>:[]float
VhRefStrings<native>(Ref:int)<transacts>:[]string
VhRefValues<native>(Ref:int)<transacts>:[]variant

# The reverse: a fresh container of Tag holding these.
VhRefFromInts<native>(Tag:int, Values:[]int)<transacts>:int
VhRefFromFloats<native>(Tag:int, Values:[]float)<transacts>:int
VhRefFromStrings<native>(Tag:int, Values:[]string)<transacts>:int
VhRefFromValues<native>(Tag:int, Values:[]variant)<transacts>:int

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

    # Godot's _ready, _process and _physics_process. Defined here so a script overrides rather
    # than declares them, and so a script that wants only one of the three still compiles.
    Ready<public>():void = {}
    Process<public>(Delta:float):void = {}
    PhysicsProcess<public>(Delta:float):void = {}

