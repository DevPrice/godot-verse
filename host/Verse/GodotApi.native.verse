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
IsInstanceValid<public>(Object:object)<decides><transacts>:void = VhIsValid(Object.Handle)?

# --- variant packing ---------------------------------------------------------

# Each of these names the lanes its tag reads and lets the struct default the rest, which is the
# whole reason `variant` is a struct rather than the twenty-three element tuple it would otherwise
# have to be: every packer would spell all twenty-three.
#
# The float lanes carry a math type's components in Godot's own order, and the integer lanes carry
# the integer vectors -- a double stops counting exactly above 2^53, and Godot's are 32-bit values
# that have to survive a round trip.

VhFromLogic(Value:logic)<transacts>:variant = variant{Tag := TagBool, I0 := if (Value?) then 1 else 0}
VhFromInt(Value:int)<transacts>:variant = variant{Tag := TagInt, I0 := Value}
VhFromFloat(Value:float)<transacts>:variant = variant{Tag := TagFloat, F0 := Value}
VhFromString(Value:string)<transacts>:variant = variant{Tag := TagString, Text := Value}
VhFromStringName(Value:string)<transacts>:variant = variant{Tag := TagStringName, Text := Value}
VhFromNodePath(Value:string)<transacts>:variant = variant{Tag := TagNodePath, Text := Value}
VhFromObject(Value:object)<transacts>:variant = variant{Tag := TagObject, Ref := Value.Handle}
VhFromRid(Value:int)<transacts>:variant = variant{Tag := TagRid, Ref := Value}

# --- variant unpacking -------------------------------------------------------

# Plain lane reads. Nothing here is failable, because FromWire on the host side fills the lane the
# tag names -- a number Godot spelled as an integer where the mirror declared a float has already
# been widened by the time a variant exists.
VhToLogic(Value:variant)<transacts>:logic = if (Value.I0 <> 0) then true else false
VhToInt(Value:variant)<transacts>:int = Value.I0
VhToFloat(Value:variant)<transacts>:float = Value.F0
VhToString(Value:variant)<transacts>:string = Value.Text

# Raises unless the value is what the caller was generated to read.
#
# With fixed lanes a mismatch no longer announces itself: reading a vector2 out of an int-tagged
# variant gives (0.0, 0.0) rather than failing to find a slot, which is silently wrong where the
# encoding this replaced was loudly wrong. So the check is explicit, and it is R-TYPE-5's runtime
# half: the message names both the type expected and the tag that arrived.
VhExpect(Value:variant, Tag:int, Expected:string)<transacts>:void =
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
VhToHandle(Value:variant)<decides><transacts>:int =
    Value.Ref <> 0
    Value.Ref

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

callable<public> := class(godot_ref):
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

# Godot's Signal. Held and passed back; connecting and emitting are signals' own requirement
# (spec 5.3) and arrive in Phase 4.
signal_ref<public> := class(godot_ref) {}

# --- reference packing -------------------------------------------------------

# Every wrapper is adopted on the way in, which is what puts the UObject shadow behind it and so
# what makes dropping it release the table entry.
VhAdopt(Value:godot_ref)<transacts>:void = VhAdoptRef(Value)

VhToArray(Value:variant)<transacts>:godot_array =
    Made := godot_array{Ref := Value.Ref}
    VhAdopt(Made)
    Made

VhToDictionary(Value:variant)<transacts>:dictionary =
    Made := dictionary{Ref := Value.Ref}
    VhAdopt(Made)
    Made

VhToCallable(Value:variant)<transacts>:callable =
    Made := callable{Ref := Value.Ref}
    VhAdopt(Made)
    Made

VhToSignal(Value:variant)<transacts>:signal_ref =
    Made := signal_ref{Ref := Value.Ref}
    VhAdopt(Made)
    Made

VhFromArray(Value:godot_array)<transacts>:variant = variant{Tag := TagArray, Ref := Value.Ref}
VhFromDictionary(Value:dictionary)<transacts>:variant = variant{Tag := TagDictionary, Ref := Value.Ref}
VhFromCallable(Value:callable)<transacts>:variant = variant{Tag := TagCallable, Ref := Value.Ref}
VhFromSignal(Value:signal_ref)<transacts>:variant = variant{Tag := TagSignal, Ref := Value.Ref}

# --- packed arrays -----------------------------------------------------------

# Godot's packed arrays are values, but variable-length ones, so they have no lanes in a
# fixed-width variant and ride as a reference like the containers do. What the author sees is a
# Verse array either way: these converters read the whole thing out, and build a fresh one going
# back, so the generated signatures stay `[]float` rather than becoming a handle to unwrap.
VhToInts(Value:variant)<transacts>:[]int = VhRefInts(Value.Ref)
VhToFloats(Value:variant)<transacts>:[]float = VhRefFloats(Value.Ref)
VhToStrings(Value:variant)<transacts>:[]string = VhRefStrings(Value.Ref)

VhFromInts(Tag:int, Values:[]int)<transacts>:variant = variant{Tag := Tag, Ref := VhRefFromInts(Tag, Values)}
VhFromFloats(Tag:int, Values:[]float)<transacts>:variant = variant{Tag := Tag, Ref := VhRefFromFloats(Tag, Values)}
VhFromStrings(Tag:int, Values:[]string)<transacts>:variant = variant{Tag := Tag, Ref := VhRefFromStrings(Tag, Values)}

# The packed arrays whose elements are math structs rather than scalars -- PackedVector2Array
# and its three siblings. One variant per element, because a vector2 has no single lane to
# flatten into the way an int does.
VhFromValues(Tag:int, Values:[]variant)<transacts>:variant = variant{Tag := Tag, Ref := VhRefFromValues(Tag, Values)}

VhFromByteArray(Values:[]int)<transacts>:variant = VhFromInts(TagPackedByteArray, Values)
VhFromInt32Array(Values:[]int)<transacts>:variant = VhFromInts(TagPackedInt32Array, Values)
VhFromInt64Array(Values:[]int)<transacts>:variant = VhFromInts(TagPackedInt64Array, Values)
VhFromFloat32Array(Values:[]float)<transacts>:variant = VhFromFloats(TagPackedFloat32Array, Values)
VhFromFloat64Array(Values:[]float)<transacts>:variant = VhFromFloats(TagPackedFloat64Array, Values)
VhFromStringArray(Values:[]string)<transacts>:variant = VhFromStrings(TagPackedStringArray, Values)

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
typed_array<public>(t:type) := class(godot_ref):
    Unpack<public>:type{_(:variant)<decides><transacts>:t}

    # How many elements it holds.
    Length<public>()<transacts>:int = VhRefSize(Ref)

    # Element at Index. Failable for two reasons that read the same to the caller: the index is out
    # of range, or the element is not a t after all -- Godot's typed arrays are checked on write,
    # but a null object is still a miss.
    Get<public>(Index:int)<decides><transacts>:t = Unpack[VhRefGet[Ref, VhFromInt(Index)]]

    # The whole thing as a Verse array, which is a copy: elements that do not convert are dropped
    # rather than failing the call, the way `for` with a failable body already reads.
    ToArray<public>()<transacts>:[]t = for (V : VhRefValues(Ref), E := Unpack[V]) { E }
