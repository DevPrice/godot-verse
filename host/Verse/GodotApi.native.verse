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
