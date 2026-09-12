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
# Not <public>, like the rest of the plumbing: a script that can name a lane can build a variant
# the host will misread (R-TYPE-7).
variant<native> := struct:
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

# The one native class in the API. Everything in GodotApi.native.verse derives from it as
# ordinary Verse, which is what keeps a thousand mirrored classes from needing a thousand C++
# shadows: the VM dispatches through the object's own shape, not through anything native.
#
# It is native because a script's class has to reach C++ somehow -- the host instantiates a user
# subclass and calls into it through a UObject, and only a native ancestor gives the instance a
# UObject representation to be reached by.
object<public><native> := class:
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

