#!/usr/bin/env python3
"""Generates host/Verse/GodotClasses.native.verse from godot-cpp/gdextension/extension_api-4-7.json.

Mirrors a subset of the Godot class hierarchy as ordinary Verse classes deriving from the
hand-written native `object` (see host/Verse/Godot.native.verse) and calling the packing
helpers in host/Verse/GodotApi.native.verse. See host/Verse/GodotApi.native.verse's own header
comment for why that keeps a thousand mirrored classes from needing a thousand C++ shadows.

Class names are unprefixed: `/Godot.org/Godot` is the namespace, and a name a script's other
`using` also defines is disambiguated at the use site as `(/Godot.org/Godot:)node2d`, the way
Epic's own libraries disambiguate their two `vector3` types.
"""

import argparse
import json
import re
from collections import Counter, defaultdict, namedtuple
from pathlib import Path

GENERATED_PATH = "host/Verse/GodotClasses.native.verse"
CLASSES_HEADER_PATH = "src/verse_api_classes.h"
MATH_LAYOUT_HEADER_PATH = "host/Private/GodotMathLayout.gen.h"
CLASS_NAMES_HEADER_PATH = "host/Private/GodotClassNames.gen.h"
DEFAULT_CLASSES_FILE = "tools/verse_api_classes.txt"
KEYWORDS_HEADER = "src/verse_keywords.h"
EXTENSION_API = "godot-cpp/gdextension/extension_api-4-7.json"

# The hand-written native class every mirrored class descends from, in Godot.native.verse. Named
# `vh_object` rather than `object` since Phase 2, because `object` is now the mirror of Godot's own
# Object class: Verse cannot reopen a class, so Object's 46 methods could not be added to the
# hand-written one, and a generated `godot_object` beside a native `object` would have put two base
# names in completion where the one a user reaches for first is the empty one.
NATIVE_ROOT = "vh_object"

# What the hand-written native root carries, and so what no generated member may shadow. Since
# Phase 4 that is two things: the handle, and the one script-level hook extension_api.json does not
# describe. Godot's three lifecycle virtuals moved to `node`, where Godot declares them.
BASE_MEMBER_NAMES = {"Handle", "_Notification"}

# /Verse.org/Verse is in scope in every generated body, and Verse reports an ambiguity rather
# than shadowing, so a parameter named Min breaks any method that mentions it. The standard
# library's names are compiler intrinsics rather than a .verse digest, so there is nothing to
# enumerate; this list is what the full-API generation actually collided with, plus the obvious
# siblings. Over-listing costs nothing but an ArgN parameter name.
#
# This package's own module-level names are in scope for the same reason and belong here too --
# Print and IsInstanceValid, the two functions GodotApi.native.verse exports.
# The names a *member* of a mirrored class may not take, because Verse's own definition of that name
# is ambiguous with it. Confirmed by the compiler rather than guessed: generating the whole API with
# no guard at all reports exactly these, and `Length`, `Reverse`, `Sign`, `Shuffle` and the rest of
# the plausible list are not among them. To confirm a new one, drop the guard, generate, and read the
# errors -- and add it here rather than widening the guess, because over-listing here costs a public
# name where over-listing VERSE_STDLIB_NAMES below costs nothing.
#
# `Min` and `Max` are here as *data*, and only as data: a `var` has no signature to be told apart
# by, so a property of that name is ambiguous with /Verse.org/Verse's function where a *method* of
# the same name is not. Confirmed both ways -- a class carrying `Max()` and `Min(:int)` compiles
# clean. If a Godot class ever does declare a method named one of these, classify_method asserts
# rather than letting the collision reach the compiler as an error hundreds of lines from its cause.
VERSE_AMBIGUOUS_MEMBER_NAMES = {"Min", "Max", "ToString"}

# What such a property is called instead. A property is worth keeping as a property -- `set X.Maximum
# = 1.0` is the spelling the exercise is for -- and the alternative was dropping it so that Godot's
# `GetMax()` and `SetMax()` carried the value, which is a worse deal for the six properties in this
# position than one word of English is.
#
# The invented name is the only one in the mirror, and it is only reachable through it: `Max` and
# `get_max` are both recorded as skipped and both point here, so either spelling an author tries is
# answered by the editor with the one that works.
PROPERTY_RENAMES = {"Min": "Minimum", "Max": "Maximum"}

# Godot members that are reachable as a module-level function instead of as a method, because Verse
# already gives the name a meaning worth keeping.
#
# `ToString` is the only one, and it is not a mere collision: Verse's string interpolation *desugars*
# to it -- `"{X}"` becomes `ToString(X)` (Desugarer.cpp's DesugarInterpolatedString) -- so a
# module-level `ToString(:object)` makes `"{MyNode}"` print what Godot's own to_string says. A method
# named ToString could not have done that, and would have been ambiguous with the stdlib besides.
# GodotApi.native.verse holds the function.
FREE_FUNCTION_REPLACEMENTS = {("Object", "to_string"): "ToString(Value)"}

VERSE_STDLIB_NAMES = {
    "Int", "Float", "Logic", "Char", "Rational",
    # Verse's bitwise intrinsics and its float constants, all of them $BuiltIn rather than a package
    # anything imports -- which is why they are not obvious and why the enum pass found them.
    "BitAnd", "BitOr", "BitXor", "BitNot", "BitShift", "BitLshift", "BitRshift",
    "Inf", "NaN", "Pi", "TwoPi", "E", "Epsilon",
    "Abs", "Ceil", "Floor", "Round", "Sqrt", "Min", "Max", "Sign", "Clamp", "Lerp", "Mod",
    "Sin", "Cos", "Tan", "ArcSin", "ArcCos", "ArcTan", "Pow", "Exp", "Ln",
    "Print", "Err", "Sleep", "Length", "Slice", "Reverse", "Shuffle", "Concatenate", "Fits",
    "ToString", "ToDiagnostic", "ToInt", "ToFloat", "ToChar", "ToRational",
    "IsInstanceValid",
    # The math extension methods in GodotMath.native.verse. An extension method is a *module-level*
    # definition -- `(V:vector2).Angle()` declares `operator'.Angle'` beside everything else -- and
    # Verse resolves a bare `Angle` against it, so a parameter of that name is ambiguous rather than
    # shadowing. Eight methods on Node3D and two others reported exactly this the first time the
    # math landed. The same constraint reaches a script's locals, which is the price of the idiom
    # and is Epic's own: SpatialMath declares `(V:vector2).Length` too.
    "Angle", "AngleTo", "AngleToPoint", "Cross", "Dot", "DistanceTo", "DistanceSquaredTo",
    "LengthSquared", "Normalized", "Rotated", "Vector2FromAngle",
    # The scalar @GlobalScope math GodotMath carries, and the extension methods added with it. Every
    # one is a module-level definition and so a name a mirrored *parameter* may not reuse -- two of
    # these (`Remap` on EditorExportPlugin.add_file, `Aspect` on XRInterface.get_projection_for_view)
    # were live collisions the moment they were written, and the compiler reported them in generated
    # code rather than in the file that caused them.
    "MathPi", "MathTau", "FloorF", "CeilF", "RoundF", "Snapped", "IsEqualApprox", "IsZeroApprox",
    "InverseLerp", "Remap", "MoveToward", "Smoothstep", "WrapF", "PingPong", "AngleDifference",
    "LerpAngle", "RotateToward", "DegToRad", "RadToDeg",
    "Floor", "Ceil", "Round", "DirectionTo", "LimitLength", "Aspect", "Slide", "Bounce", "Reflect",
    "Project", "Orthogonal",
    "Expand", "Merge", "Grow", "HasPoint", "HasArea", "Intersects", "GetEnd", "GetCenter", "GetArea",
    "Darkened", "Lightened", "Inverted", "GetLuminance",
    "Sinh", "Cosh", "Tanh", "Asinh", "Acosh", "Atanh", "Ease", "LinearToDb", "DbToLinear",
    "CubicInterpolate", "BezierInterpolate", "BezierDerivative", "IsNan", "IsInf", "IsFinite",
    "TruncatedQuotient",
    # GodotApi.native.verse's hand-written utility wrappers.
    "PushError", "PushWarning", "PrintRich", "PrintErr", "PrintVerbose", "PrintRaw",
    "VariantTypeName", "ErrorString", "InstanceFromId", "IsInstanceIdValid", "RidAllocateId",
    "RidFromInt64",
    # The transform family's methods, and the last six utilities.
    "IsPointOver", "Inverse", "Xform", "Slerp", "GetVolume", "HasVolume", "Transposed",
    "Determinant", "GetScale", "Scaled", "BasisXform", "GetRotation", "Translated",
    "AffineInverse", "InverseOrthonormal",
    "FMod", "Po2AtLeast", "NearestPo2", "StepDecimals", "CubicInterpolateInTime", "NearestAngle",
    "CubicInterpolateAngle", "CubicInterpolateAngleInTime",
}

TypeInfo = namedtuple("TypeInfo", ["verse_type", "pack_fn", "pack_decides", "unpack_fn", "unpack_decides"])

# Godot type -> (verse type, packer, packer<decides>, unpacker, unpacker<decides>). Packer/unpacker
# names and their <decides>-ness come straight from GodotApi.native.verse; a method that calls a
# <decides> function must itself be <decides>, but only a <decides> callee is invoked with [...]
# rather than (...) -- see GodotApi.native.verse's bracket-discipline comment.
#
# Every unpacker here is total. A dead receiver or a value the wire cannot carry raises inside the
# host rather than failing, so a mirrored method that returns a *value* cannot fail and must not
# claim it can -- `if (P := GetPosition[])` would otherwise read as handling a case that never
# arrives. Object returns are the exception: a null Godot object is a real absence, and TypeResolver
# gives those VhToHandle, which is the one unpacker still marked <decides>.
SCALAR_TYPES = {
    "bool": TypeInfo("logic", "VhFromLogic", False, "VhToLogic", False),
    "int": TypeInfo("int", "VhFromInt", False, "VhToInt", False),
    "int32": TypeInfo("int", "VhFromInt", False, "VhToInt", False),
    "int64": TypeInfo("int", "VhFromInt", False, "VhToInt", False),
    "uint32": TypeInfo("int", "VhFromInt", False, "VhToInt", False),
    "float": TypeInfo("float", "VhFromFloat", False, "VhToFloat", False),
    "String": TypeInfo("string", "VhFromString", False, "VhToString", False),
    "StringName": TypeInfo("string", "VhFromStringName", False, "VhToString", False),
    "NodePath": TypeInfo("string", "VhFromNodePath", False, "VhToString", False),
    "RID": TypeInfo("int", "VhFromRid", False, "VhToInt", False),

    # The reference types. They cross as an id in the GDExtension's table rather than as a copy,
    # because Godot's Array and Dictionary have reference semantics an author can observe and a
    # Callable cannot be decomposed at all -- see src/verse_ref_table.h.
    "Array": TypeInfo("godot_array", "VhFromArray", False, "VhToArray", False),
    "Dictionary": TypeInfo("dictionary", "VhFromDictionary", False, "VhToDictionary", False),
    "Callable": TypeInfo("callable", "VhFromCallable", False, "VhToCallable", False),
    "Signal": TypeInfo("signal_ref", "VhFromSignal", False, "VhToSignal", False),

    # The packed arrays are references on the wire for the same reason -- variable length, and a
    # fixed-width variant has no lanes for them -- but the author sees a Verse array either way:
    # the converters read the whole container out and build a fresh one going back.
    "PackedByteArray": TypeInfo("[]int", "VhFromByteArray", False, "VhToInts", False),
    "PackedInt32Array": TypeInfo("[]int", "VhFromInt32Array", False, "VhToInts", False),
    "PackedInt64Array": TypeInfo("[]int", "VhFromInt64Array", False, "VhToInts", False),
    "PackedFloat32Array": TypeInfo("[]float", "VhFromFloat32Array", False, "VhToFloats", False),
    "PackedFloat64Array": TypeInfo("[]float", "VhFromFloat64Array", False, "VhToFloats", False),
    "PackedStringArray": TypeInfo("[]string", "VhFromStringArray", False, "VhToStrings", False),
}

# Godot's math types are added to this table at import time from MATH_LAYOUT below, so that one
# list drives the struct, the packers and the type table alike: a type present in one and missing
# from another is the failure that arrangement exists to make impossible.
#
# Deliberately absent: the ten packed arrays. They are value types, but variable-length ones, and a
# fixed-width variant has no lanes for them -- they ride as a reference id with bulk converters
# (spec R-TYPE-1). Until that lands the generator skips the methods that use them, which is part of
# the coverage report's "unsupported_type" count.


def repo_root() -> Path:
    return Path(__file__).resolve().parent.parent


def resolve(root: Path, p: str) -> Path:
    path = Path(p)
    return path if path.is_absolute() else (root / path)


def load_reserved_words(header_path: Path) -> set:
    text = header_path.read_text(encoding="utf-8")
    m = re.search(r"reserved_words\[\]\s*=\s*\{(.*?)\};", text, re.DOTALL)
    if not m:
        raise ValueError(f"could not find reserved_words[] in {header_path}")
    return set(re.findall(r'"((?:[^"\\]|\\.)*)"', m.group(1)))


def load_api(api_path: Path) -> dict:
    with api_path.open(encoding="utf-8") as f:
        return json.load(f)


def split_pascal(name: str) -> list:
    if not name:
        return []
    tokens = []
    cur = name[0]
    for i in range(1, len(name)):
        prev, c = name[i - 1], name[i]
        split = (prev.islower() and c.isupper()) or (
            prev.isupper() and c.isupper() and i + 1 < len(name) and name[i + 1].islower()
        )
        if split:
            tokens.append(cur)
            cur = c
        else:
            cur += c
    tokens.append(cur)
    return tokens


def verse_class_name(godot_name: str) -> str:
    return "_".join(t.lower() for t in split_pascal(godot_name))


def pascal_member_name(godot_name: str) -> str:
    """`set_v_size_flags` -> `SetVSizeFlags`, with nothing said about whether the name is usable."""
    parts = [p for p in godot_name.split("_") if p]
    return "".join(p[0].upper() + p[1:] for p in parts)


def verse_method_name(godot_name: str) -> str:
    return pascal_member_name(godot_name)


def verse_virtual_name(godot_name: str) -> str:
    """`_ready` -> `_Ready`. Godot's own leading underscore is kept, and it is load-bearing.

    Counted before it was decided (docs/phase-4-design.md 7.1). A virtual against a *method* of the
    same PascalCase name collides 834 times, almost entirely on server-extension classes nobody
    derives from -- that alone would have been survivable. What decides it is the eight collisions
    against a **signal**: `Node.ready` vs `_ready`, `CanvasItem.draw` vs `_draw`, `Control.gui_input`
    vs `_gui_input`, `BaseButton.pressed` vs `_pressed`, and four more, all on the classes an
    ordinary script derives from. Both are things a script touches.

    So Godot's own disambiguation is kept rather than thrown away. It is also what a Godot developer
    types in GDScript and what C# generates (`public override void _Ready()`), so R-AUD-1 is served
    rather than strained.
    """
    return "_" + pascal_member_name(godot_name) if godot_name.startswith("_") else pascal_member_name(godot_name)


# What an unoverridden virtual answers. A void one needs nothing; anything else needs a value, and
# where Godot's own default is "not handled" this is what that must mean.
#
# A type with no entry here and no rule below is a *skip*, recorded with its reason rather than
# quietly absent: an object return has no zero value a script could write, and a typed container
# would have to mint a Godot Array on every call Godot makes to a virtual nobody overrode.
VIRTUAL_SCALAR_DEFAULTS = {
    "logic": "false",
    "int": "0",
    "float": "0.0",
    "string": '""',
    "variant": "variant{}",
    "godot_array": "godot_array{}",
    "dictionary": "dictionary{}",
    "callable": "callable{}",
    "signal_ref": "signal_ref{}",
}


def virtual_default(info, enums: dict):
    """The default body for a virtual returning `info`, or None when there is nothing to write."""
    if info is None:
        return "{}"
    verse_type = info.verse_type
    if verse_type in VIRTUAL_SCALAR_DEFAULTS:
        return VIRTUAL_SCALAR_DEFAULTS[verse_type]
    if verse_type.startswith("[]"):
        return "array{}"
    if verse_type in MATH_STRUCT_NAMES:
        return f"{verse_type}{{}}"
    for enum in enums.values():
        if enum.verse_name == verse_type and enum.values:
            return f"{verse_type}.{enum.values[0][0]}"
    return None


def verse_param_name(godot_name: str, index: int, reserved_words: set, used: set, members: set) -> str:
    # A parameter that matches a member of the enclosing class is ambiguous, not shadowing:
    # Tween.set_parallel(parallel) and Tween.parallel() collide, as does any argument named
    # `process` against `object`'s Process.
    candidate = verse_method_name(godot_name)
    if (not candidate or candidate in reserved_words or candidate in used
            or candidate in members or candidate in VERSE_STDLIB_NAMES):
        return f"Arg{index}"
    return candidate


class TypeResolver:
    def __init__(self, emitted: set, parent_map: dict, class_names: set, enums: dict):
        self.emitted = emitted
        self.parent_map = parent_map
        self.class_names = class_names
        self.enums = enums
        self._ancestor_cache = {}
        # Element-type suffix -> the element's own TypeInfo, for every typed array the API asked
        # about. Filled during generation and drained by emit_typed_array_converters.
        self.typed_arrays = {}
        # The same for typed dictionaries, keyed by the two halves' suffixes joined.
        self.typed_dictionaries = {}

    def nearest_emitted_ancestor(self, godot_class: str):
        if godot_class in self._ancestor_cache:
            return self._ancestor_cache[godot_class]
        cur = self.parent_map.get(godot_class)
        result = None
        while cur:
            if cur in self.emitted:
                result = cur
                break
            cur = self.parent_map.get(cur)
        self._ancestor_cache[godot_class] = result
        return result

    def classify(self, godot_type: str):
        """Returns a TypeInfo, or None if unsupported."""
        if godot_type in SCALAR_TYPES:
            return SCALAR_TYPES[godot_type]
        if godot_type.startswith("enum::"):
            info = self.enums.get(godot_type[len("enum::"):])
            if info is None:
                return SCALAR_TYPES["int"]
            stem = enum_converter_stem(info.verse_name)
            return TypeInfo(info.verse_name, f"VhFrom{stem}", False, f"VhTo{stem}", False)
        if godot_type.startswith("bitfield::"):
            # A bitfield *value* can be a combination, which no enum value can hold, so the parameter
            # stays an int and flags are combined explicitly with Verse's own BitOr over ToInt. The
            # enum is still declared, so the flags have names.
            return SCALAR_TYPES["int"]
        if godot_type.startswith(TYPED_ARRAY_PREFIX):
            return self.classify_typed_array(godot_type[len(TYPED_ARRAY_PREFIX):])
        if godot_type.startswith(TYPED_DICT_PREFIX):
            return self.classify_typed_dictionary(godot_type[len(TYPED_DICT_PREFIX):])
        if "," in godot_type:
            return self.classify_union(godot_type)
        if godot_type in self.class_names:
            target = godot_type if godot_type in self.emitted else self.nearest_emitted_ancestor(godot_type)
            if target is None:
                return None
            return TypeInfo(verse_class_name(target), "VhFromObject", False, "VhToHandle", True)
        return None

    def classify_typed_array(self, element: str):
        """`typedarray::Node`'s element half, as a typed_array(node) TypeInfo -- or None.

        R-TYPE-2, and the gap that mattered most: a plain godot_array offers ten typed element
        accessors and not one of them is an object, so `GetChildren()` returned a container whose
        elements no script could read. Walking children is what scene code *is*.

        Recording the element type rather than emitting here, because the converters are module-level
        definitions and this is called from inside a class body. emit_typed_array_converters writes
        the ones that were actually asked for.
        """
        godot_element = typed_array_element_type(element)
        if godot_element is None:
            return None
        element_info = self.classify(godot_element)
        if element_info is None:
            return None
        # No typed array of a typed array: Godot spells none, and nesting the parametric class would
        # need a converter whose own type mentions t twice.
        if element_info.verse_type.startswith("typed_array("):
            return None
        suffix = typed_array_suffix(godot_element, element_info)
        self.typed_arrays[suffix] = element_info
        return TypeInfo(f"typed_array({element_info.verse_type})",
                        f"VhFrom{suffix}Array", False, f"VhTo{suffix}Array", False)

    def classify_typed_dictionary(self, spelling: str):
        """`typeddictionary::int;String`'s two halves, as a typed_dictionary(int, string)."""
        key_spelling, _, value_spelling = spelling.partition(";")
        key_godot = typed_array_element_type(key_spelling)
        value_godot = typed_array_element_type(value_spelling)
        if key_godot is None or value_godot is None:
            return None
        key_info = self.classify(key_godot)
        value_info = self.classify(value_godot)
        if key_info is None or value_info is None:
            return None
        if any(i.verse_type.startswith(("typed_array(", "typed_dictionary(")) for i in (key_info, value_info)):
            return None
        suffix = typed_array_suffix(key_godot, key_info) + typed_array_suffix(value_godot, value_info)
        self.typed_dictionaries[suffix] = (key_info, value_info)
        return TypeInfo(f"typed_dictionary({key_info.verse_type}, {value_info.verse_type})",
                        f"VhFrom{suffix}Dict", False, f"VhTo{suffix}Dict", False)

    def classify_union(self, godot_type: str):
        """`BaseMaterial3D,ShaderMaterial` and its thirty siblings, as their nearest shared ancestor.

        Godot spells a parameter that accepts several classes as a comma-separated list, sometimes
        with `-Excluded` members -- `Texture2D,-AtlasTexture` is "a Texture2D but not an atlas". The
        exclusions carry no information a static type can hold, so they are dropped and what is left
        is widened to the one class every member derives from. That is exactly the type Godot itself
        checks against at the call, so nothing is lost that the engine enforces.
        """
        members = [name for name in godot_type.split(",") if name and not name.startswith("-")]
        if not members or any(name not in self.class_names for name in members):
            return None
        common = members[0]
        for name in members[1:]:
            common = self.common_ancestor(common, name)
            if common is None:
                return None
        return self.classify(common)

    def ancestry(self, godot_class: str) -> list:
        chain = []
        cur = godot_class
        while cur is not None and cur != "Object":
            chain.append(cur)
            cur = self.parent_map.get(cur)
        return chain

    def common_ancestor(self, a: str, b: str):
        b_chain = set(self.ancestry(b))
        for name in self.ancestry(a):
            if name in b_chain:
                return name
        return None


# Godot's math types as Verse structs, each mirroring its own member tree so that
# `Transform.Basis.X.X` reads the way `transform.basis.x.x` does in GDScript.
#
# Written out rather than read from extension_api.json's `members`, which lists *properties* and
# so includes both the derived and the redundant: `Rect2.end` is `position + size`, and
# `Plane.x/y/z` are the components of `Plane.normal` spelled again. Flattening those would put six
# lanes where the wire carries four. What is here is the canonical decomposition, and it has to
# agree component for component with src/verse_value.cpp -- MATH_LANES below is the check that it
# does, and tests/verse_api_gen asserts it.
#
# Data only. The ~30 methods each of these carries in Godot (`Length`, `Normalized`, `Rotated`) are
# R-SCN-3, which the roadmap puts in Phase 2 -- and generating them there for all sixteen beats
# adopting Verse's own `vector2` here for two of them, since Verse's `vector3` is deprecated and
# fourteen of the sixteen have no counterpart at all.
#
# Order is dependency order: a struct's members are declared before it.
MATH_LAYOUT = {
    "Vector2": [("x", "float"), ("y", "float")],
    "Vector2i": [("x", "int"), ("y", "int")],
    "Vector3": [("x", "float"), ("y", "float"), ("z", "float")],
    "Vector3i": [("x", "int"), ("y", "int"), ("z", "int")],
    "Vector4": [("x", "float"), ("y", "float"), ("z", "float"), ("w", "float")],
    "Vector4i": [("x", "int"), ("y", "int"), ("z", "int"), ("w", "int")],
    "Rect2": [("position", "Vector2"), ("size", "Vector2")],
    "Rect2i": [("position", "Vector2i"), ("size", "Vector2i")],
    "Plane": [("normal", "Vector3"), ("d", "float")],
    "Quaternion": [("x", "float"), ("y", "float"), ("z", "float"), ("w", "float")],
    "AABB": [("position", "Vector3"), ("size", "Vector3")],
    # x, y and z are Godot's own names for the basis' three columns (Basis.xml), and columns are
    # what crosses -- Basis(Vector3, Vector3, Vector3) is set_columns on the Godot side.
    "Basis": [("x", "Vector3"), ("y", "Vector3"), ("z", "Vector3")],
    "Transform2D": [("x", "Vector2"), ("y", "Vector2"), ("origin", "Vector2")],
    "Transform3D": [("basis", "Basis"), ("origin", "Vector3")],
    "Projection": [("x", "Vector4"), ("y", "Vector4"), ("z", "Vector4"), ("w", "Vector4")],
    "Color": [("r", "float"), ("g", "float"), ("b", "float"), ("a", "float")],
}

MATH_TYPES = list(MATH_LAYOUT)
MATH_STRUCT_NAMES = {verse_class_name(name) for name in MATH_LAYOUT}

# Godot's packed array of a struct, where it has one. An array of these crosses as that packed
# type rather than as a plain Array.
MATH_PACKED_ARRAYS = {
    "Vector2": "VH_VARIANT_PACKED_VECTOR2_ARRAY",
    "Vector3": "VH_VARIANT_PACKED_VECTOR3_ARRAY",
    "Vector4": "VH_VARIANT_PACKED_VECTOR4_ARRAY",
    "Color": "VH_VARIANT_PACKED_COLOR_ARRAY",
}

# The vh_variant_tag enumerator for a Godot type, as the C header spells it.
def math_variant_tag(godot_name: str) -> str:
    return "VH_VARIANT_" + "_".join(t.upper() for t in split_pascal(godot_name))

# How many lanes of each kind a type occupies, as src/verse_value.cpp writes it and as
# GodotBindings.cpp's LanesFor reads it. Stated here so a layout edited on one side and not the
# other fails a test rather than truncating a value at runtime.
MATH_LANES = {
    "Vector2": (0, 2), "Vector2i": (2, 0), "Vector3": (0, 3), "Vector3i": (3, 0),
    "Vector4": (0, 4), "Vector4i": (4, 0), "Rect2": (0, 4), "Rect2i": (4, 0),
    "Plane": (0, 4), "Quaternion": (0, 4), "AABB": (0, 6), "Basis": (0, 9),
    "Transform2D": (0, 6), "Transform3D": (0, 12), "Projection": (0, 16), "Color": (0, 4),
}

# The Verse tag constant for a Godot type, as GodotApi.native.verse spells it.
MATH_TAGS = {name: "Tag" + ("Aabb" if name == "AABB" else name) for name in MATH_LAYOUT}

for _godot_math in MATH_TYPES:
    SCALAR_TYPES[_godot_math] = TypeInfo(
        verse_class_name(_godot_math), f"VhFrom{_godot_math}", False, f"VhTo{_godot_math}", False)

# Godot's Variant itself, now that a script can name the type (R-TYPE-7, amended). The converters
# are the identity: the wire carries exactly this, so nothing is packed or unpacked.
SCALAR_TYPES["Variant"] = TypeInfo("variant", "VhFromVariant", False, "VhToVariant", False)

# The packed arrays of a math struct, added from VARIANT_LANES below so that one table drives the
# reader, the converter and the type table alike.


# --- Godot's enums -----------------------------------------------------------
#
# R-SCN-5. Every one of Godot's 758 enums becomes a real Verse enum rather than a magic integer, so
# `SetProcessMode(node_process_mode.Always)` compiles and `SetProcessMode(2)` stops compiling. That
# is the phase's one deliberate break of existing scripts, and it is the R-AUD-1 win: a Godot
# developer reads the enumerator, not the number.
#
# The wire is unchanged -- an enum still crosses as the int it is. What changes is the Verse
# signature and a pair of generated converters per enum, in ordinary Verse over `case`, because a
# `<native>` Verse enum needs a hand-written C++ shadow and 758 of those is not a thing to write.

GodotEnum = namedtuple("GodotEnum", ["key", "verse_name", "is_bitfield", "values", "stripped"])


def enum_verse_name(owner: str, godot_name: str) -> str:
    """`Node.ProcessMode` -> `node_process_mode`; `Error` -> `error`.

    Class-qualified because 96 bare enum names repeat across classes -- Mode, Operator, Param -- and
    the whole project shares one flat scope. A global enum has no owner to qualify it with, which is
    also true of it in Godot.
    """
    parts = ([verse_class_name(part) for part in owner.split(".")] if owner else []) + [
        verse_class_name(part) for part in godot_name.split(".")
    ]
    return "_".join(parts)


def enum_converter_stem(verse_name: str) -> str:
    return "".join(part[0].upper() + part[1:] for part in verse_name.split("_"))


def _common_prefix_words(names: list) -> list:
    """The `_`-delimited words every one of these names starts with, never consuming the last word."""
    if len(names) < 2:
        return []
    splits = [n.split("_") for n in names]
    prefix = []
    for i in range(min(len(s) for s in splits) - 1):
        word = splits[0][i]
        if not all(s[i] == word for s in splits):
            break
        prefix.append(word)
    return prefix


def _pascal_enumerator(name: str) -> str:
    return "".join(w[0].upper() + w[1:].lower() for w in name.split("_") if w)


def enumerator_names(godot_names: list) -> tuple:
    """Verse names for one enum's enumerators, and whether the shared prefix was stripped.

    The rule is the enumerators' own longest shared prefix rather than the enum's name, because
    Godot's prefixing is only half consistent: deriving `PROCESS_MODE_` from `ProcessMode` works, and
    the same derivation fails for 357 of 736 class enums. The enumerators always agree with each
    other.

    Stripping is **all or nothing per enum**, and it is abandoned when any stripped name would be an
    illegal identifier (`SOURCE_2D_TEXTURE`), a duplicate of another in the same enum, a reserved
    word, or ambiguous with a Verse stdlib name -- an enumerator is ambiguous with a function of the
    same name exactly as a data member is, which is why Variant::Type keeps its `TYPE_` and reads
    `variant_type.TypeInt`. Per enum rather than per enumerator so that one enum reads consistently:
    a `Bool` beside a `TypeInt` would be worse than either.
    """
    prefix = _common_prefix_words(godot_names)
    if prefix:
        cut = len("_".join(prefix)) + 1
        short = [_pascal_enumerator(name[cut:]) for name in godot_names]
        legal = all(name and not name[0].isdigit() for name in short)
        if (legal and len(set(short)) == len(short)
                and not any(name in VERSE_STDLIB_NAMES or name in RESERVED_WORDS for name in short)):
            return short, True
    return [_pascal_enumerator(name) for name in godot_names], False


def collect_enums(api: dict) -> dict:
    """Every Godot enum, keyed by the way a signature spells it -- `Node.ProcessMode`, `Error`.

    Two kinds of enumerator are dropped, and each would otherwise be a lie:

    - a `_MAX` sentinel, which is a count rather than a value and the one thing Godot renumbers
      between releases -- twelve of them moved between 4.6 and 4.7 (docs/phase-2-design.md, Stage 0);
    - an alias, a second name for a value another enumerator already has. A Verse enum has one name
      per value and the int -> enum conversion would have two `case` arms for one number.
    """
    sources = []
    for godot_class in api["classes"]:
        sources += [(godot_class["name"], e) for e in godot_class.get("enums", [])]
    for builtin in api.get("builtin_classes", []):
        sources += [(builtin["name"], e) for e in builtin.get("enums", [])]
    sources += [(None, e) for e in api.get("global_enums", [])]

    collected = {}
    for owner, godot_enum in sources:
        seen_values = set()
        kept = []
        for value in godot_enum["values"]:
            if value["name"].endswith("_MAX") or value["value"] in seen_values:
                continue
            seen_values.add(value["value"])
            kept.append(value)
        if not kept:
            continue

        names, stripped = enumerator_names([value["name"] for value in kept])
        key = f"{owner}.{godot_enum['name']}" if owner else godot_enum["name"]
        verse_name = enum_verse_name(owner, godot_enum["name"])
        if verse_name in collected:
            raise ValueError(f"two Godot enums both spell themselves {verse_name}")
        collected[key] = GodotEnum(
            key=key,
            verse_name=verse_name,
            is_bitfield=bool(godot_enum.get("is_bitfield")),
            values=list(zip(names, [value["value"] for value in kept])),
            stripped=stripped,
        )
    return collected


def check_enum_names(enums: dict, api: dict) -> None:
    """Asserts no enum's name collides with a mirrored class', and that stripping stayed unique.

    A generation failure rather than a silently shadowed name: Verse forbids shadowing outright, so a
    collision here is a compile error hundreds of lines from its cause.
    """
    class_names = {verse_class_name(c["name"]) for c in api["classes"]}
    for info in enums.values():
        if info.verse_name in class_names:
            raise ValueError(f"enum {info.key} spells itself {info.verse_name}, which is a mirrored class")
        names = [name for name, _ in info.values]
        if len(set(names)) != len(names):
            raise ValueError(f"enum {info.key} has two enumerators named the same")


def emit_enums(enums: dict) -> list:
    """The enum declarations and the converters every generated body needs.

    The converters are module-scoped, `Vh`-prefixed and invisible to a script: they exist because a
    mirrored method's *body* has an int and its signature promises an enum. What a script gets is one
    public name, `ToInt`, overloaded across every enum -- which is what makes a bitfield combination
    spellable, since a combination is not an enumerator and a `bitfield::` parameter therefore stays
    an int: `BitOr(ToInt(mouse_button_mask.Left), ToInt(mouse_button_mask.Right))`.
    """
    blocks = []
    for key in sorted(enums, key=lambda k: enums[k].verse_name):
        info = enums[key]
        stem = enum_converter_stem(info.verse_name)
        first = info.values[0][0]

        blocks.append(
            f"# Godot's {key}.\n"
            f"{info.verse_name}<public> := enum:\n"
            + "\n".join(f"    {name}" for name, _ in info.values))

        # variant -> enum, because that is what a generated method body has: the same shape as every
        # other unpacker in the table, so emit_method needs no case for enums. Total, because a
        # mirrored method that returns a *value* must not claim it can fail -- a number no enumerator
        # has is the kind of wrongness VhExpect reports for a mismatched variant, and it raises the
        # same way. The trailing enumerator is unreachable and is there because a case arm has to
        # produce one.
        arms = "\n".join(f"        {number} => {info.verse_name}.{name}" for name, number in info.values)
        blocks.append(
            f"VhTo{stem}(Value:variant)<reads>:{info.verse_name} =\n"
            f"    case (VhToInt(Value)):\n"
            f"{arms}\n"
            f"        _ =>\n"
            f'            VhTypeMismatch("{info.verse_name}", Value)\n'
            f"            {info.verse_name}.{first}")

        blocks.append(
            f"VhFrom{stem}(Value:{info.verse_name})<reads>:variant = VhFromInt(ToInt(Value))")

        # The one public name, and the mapping lives here rather than in the packer so that a
        # bitfield combination has a spelling: BitOr(ToInt(A), ToInt(B)).
        back = "\n".join(f"        {info.verse_name}.{name} => {number}" for name, number in info.values)
        blocks.append(
            f"ToInt<public>(Value:{info.verse_name})<reads>:int =\n"
            f"    case (Value):\n"
            f"{back}")
    return blocks


# Every type a Godot Variant can carry, in Variant::Type order, and what the mirror reads it as.
#
# This is what makes a Variant-typed method usable -- 231 of them, the largest single bucket of the
# coverage report's unsupported_type (docs/phase-2-design.md 2). A script names `variant` in the
# signature and asks with `As<GodotType>[V]`, which is a <decides> free function invoked failably:
# it reads as a cast without being one, which matters because Verse's real cast rejects a struct on
# both sides (1). Nothing is boxed and nothing is unwrapped -- what an AsVector4 hands back is a
# vector4, not a wrapper holding one.
#
# `godot_type` is the Variant::Type enumerator, and the generator resolves its number out of
# extension_api.json and asserts every one is covered exactly once, so a Godot renumbering is a
# generation failure rather than a value read off the wrong lane.
#
# There is no overloaded `VariantFrom`. 1 read Verse as overloading freely on parameter type, and it
# does not -- the compiler rejected two arrangements in a row, each time refusing the *definitions*
# rather than a call:
#
# - **two array-typed overloads are ambiguous whatever their element types**, because `array{}` is a
#   call site that cannot resolve them: an empty array literal has no element type. `string` is
#   `[]char`, so that is every packed array plus String at once;
# - and `VariantFrom(:logic)` is ambiguous with `VariantFrom(:[]char)`, which no reading of
#   "overloads on parameter type" predicts.
#
# So each lane gets its own `VariantFrom<GodotType>`, symmetric with its `As<GodotType>` reader. That
# loses `VariantFrom(42)`, and buys a rule that is one sentence and cannot be undermined by whatever
# Verse's real overload rule turns out to be. Where overloading *is* known to work is within one
# class on distinct non-array types -- `dictionary.GetInt` takes a string, an int or a vector2i.
#
# `kind` is not carried: the variant_kind enumerator is derived from `godot_type` by
# screaming_pascal_case, which keeps Godot's own TYPE_ prefix. Stripping it would give `Int` and
# `Float`, and an enumerator of that name is ambiguous with /Verse.org/Verse's function of the same
# name exactly as a data member would be. The rule is per *enum* rather than per enumerator so one
# enum reads consistently -- a variant_kind holding Bool beside TypeInt would be worse than either.
VariantLane = namedtuple(
    "VariantLane", ["godot_type", "reader", "verse_type", "tag", "to_fn", "from_fn"]
)


def screaming_pascal_case(name: str) -> str:
    """`TYPE_PACKED_VECTOR2_ARRAY` -> `TypePackedVector2Array`.

    The one irregularity is Godot's dimensional suffix: a `d` closing a word with a digit before it
    uppercases, so TRANSFORM2D is Transform2D the way Godot spells it. Same rule as
    verse_pascal_case on the C++ side, for the same reason.
    """
    words = []
    for word in name.split("_"):
        if not word:
            continue
        cased = word[0].upper() + word[1:].lower()
        if len(cased) > 1 and cased[-1] == "d" and cased[-2].isdigit():
            cased = cased[:-1] + "D"
        words.append(cased)
    return "".join(words)


def _math_lanes() -> list:
    return [
        VariantLane(
            godot_type=f"TYPE_{'_'.join(t.upper() for t in split_pascal(name))}",
            reader=name,
            verse_type=verse_class_name(name),
            tag=MATH_TAGS[name],
            to_fn=f"VhTo{name}",
            from_fn=f"VhFrom{name}",
        )
        for name in MATH_TYPES
    ]


VARIANT_LANES = [
    VariantLane("TYPE_BOOL", "Bool", "logic", "TagBool", "VhToLogic", "VhFromLogic"),
    VariantLane("TYPE_INT", "Int", "int", "TagInt", "VhToInt", "VhFromInt"),
    VariantLane("TYPE_FLOAT", "Float", "float", "TagFloat", "VhToFloat", "VhFromFloat"),
    VariantLane("TYPE_STRING", "String", "string", "TagString", "VhToString", "VhFromString"),
] + _math_lanes() + [
    VariantLane("TYPE_STRING_NAME", "StringName", "string", "TagStringName", "VhToString", "VhFromStringName"),
    VariantLane("TYPE_NODE_PATH", "NodePath", "string", "TagNodePath", "VhToString", "VhFromNodePath"),
    VariantLane("TYPE_RID", "Rid", "int", "TagRid", "VhToInt", "VhFromRid"),
    VariantLane("TYPE_OBJECT", "Object", "object", "TagObject", "VhToObject", "VhFromObject"),
    VariantLane("TYPE_CALLABLE", "Callable", "callable", "TagCallable", "VhToCallable", "VhFromCallable"),
    VariantLane("TYPE_SIGNAL", "Signal", "signal_ref", "TagSignal", "VhToSignal", "VhFromSignal"),
    VariantLane("TYPE_DICTIONARY", "Dictionary", "dictionary", "TagDictionary", "VhToDictionary", "VhFromDictionary"),
    VariantLane("TYPE_ARRAY", "Array", "godot_array", "TagArray", "VhToArray", "VhFromArray"),
    VariantLane("TYPE_PACKED_BYTE_ARRAY", "PackedByteArray", "[]int", "TagPackedByteArray", "VhToInts", "VhFromByteArray"),
    VariantLane("TYPE_PACKED_INT32_ARRAY", "PackedInt32Array", "[]int", "TagPackedInt32Array", "VhToInts", "VhFromInt32Array"),
    VariantLane("TYPE_PACKED_INT64_ARRAY", "PackedInt64Array", "[]int", "TagPackedInt64Array", "VhToInts", "VhFromInt64Array"),
    VariantLane("TYPE_PACKED_FLOAT32_ARRAY", "PackedFloat32Array", "[]float", "TagPackedFloat32Array", "VhToFloats", "VhFromFloat32Array"),
    VariantLane("TYPE_PACKED_FLOAT64_ARRAY", "PackedFloat64Array", "[]float", "TagPackedFloat64Array", "VhToFloats", "VhFromFloat64Array"),
    VariantLane("TYPE_PACKED_STRING_ARRAY", "PackedStringArray", "[]string", "TagPackedStringArray", "VhToStrings", "VhFromStringArray"),
    VariantLane("TYPE_PACKED_VECTOR2_ARRAY", "PackedVector2Array", "[]vector2", "TagPackedVector2Array", "VhToVector2s", "VhFromVector2s"),
    VariantLane("TYPE_PACKED_VECTOR3_ARRAY", "PackedVector3Array", "[]vector3", "TagPackedVector3Array", "VhToVector3s", "VhFromVector3s"),
    VariantLane("TYPE_PACKED_COLOR_ARRAY", "PackedColorArray", "[]color", "TagPackedColorArray", "VhToColors", "VhFromColors"),
    VariantLane("TYPE_PACKED_VECTOR4_ARRAY", "PackedVector4Array", "[]vector4", "TagPackedVector4Array", "VhToVector4s", "VhFromVector4s"),
]

# The one lane with no reader: a Variant holding nothing is what VariantKind answers, not something
# to convert. Present so the coverage assertion below sees every Variant::Type.
VARIANT_NIL = "TYPE_NIL"

# The packed arrays of a math struct, which ride as a reference and convert element by element --
# there is no flat lane sequence to read them out of the way VhRefInts reads a PackedInt32Array.
MATH_PACKED_LANES = [lane for lane in VARIANT_LANES if lane.to_fn.startswith("VhTo")
                     and lane.verse_type.startswith("[]")
                     and lane.verse_type[2:] in MATH_STRUCT_NAMES]

# Readers whose <decides>-ness comes from the conversion rather than from the tag check.
VARIANT_DECIDES_CONVERTERS = {"VhToObject"}

# The lane a converter belongs to, by its packer's name. Every VhFrom* in VARIANT_LANES is distinct,
# which makes this the one reliable way from an element's TypeInfo back to its As/VariantFrom pair.
LANE_BY_PACKER = {lane.from_fn: lane.reader for lane in VARIANT_LANES}

# Variant::Type number (as a string, the way extension_api.json spells it inside a typedarray) ->
# enumerator name. Filled by check_variant_lanes, which is the one place that reads the numbers.
GODOT_VARIANT_TYPE_BY_TAG = {}

for _packed in MATH_PACKED_LANES:
    SCALAR_TYPES[_packed.reader] = TypeInfo(
        _packed.verse_type, _packed.from_fn, False, _packed.to_fn, False)


def variant_type_values(api: dict) -> dict:
    """Variant::Type's enumerators and their numbers, from the API rather than from memory."""
    for enum in api.get("global_enums", []):
        if enum["name"] == "Variant.Type":
            return {v["name"]: v["value"] for v in enum["values"]}
    raise ValueError("Variant.Type is not in extension_api.json")


def check_variant_lanes(api: dict) -> dict:
    """Asserts VARIANT_LANES covers Variant::Type exactly, and answers each lane's number.

    TYPE_MAX is excluded deliberately: it is a count rather than a value, and it is the one
    enumerator Godot renumbers between releases -- 4.6 says 39 and 4.7 says 40 for twelve such
    sentinels elsewhere in the API (docs/phase-2-design.md, Stage 0 results).
    """
    values = variant_type_values(api)
    declared = [VARIANT_NIL] + [lane.godot_type for lane in VARIANT_LANES]
    expected = {name for name in values if name != "TYPE_MAX"}
    missing = expected - set(declared)
    extra = set(declared) - expected
    if missing or extra:
        raise ValueError(
            f"VARIANT_LANES does not match Variant::Type: missing {sorted(missing)}, unknown {sorted(extra)}")
    if len(declared) != len(set(declared)):
        raise ValueError("VARIANT_LANES names a Variant::Type twice")
    GODOT_VARIANT_TYPE_BY_TAG.clear()
    GODOT_VARIANT_TYPE_BY_TAG.update({str(number): name for name, number in values.items()})
    return values


def emit_math_packed_converters() -> list:
    """Element-by-element converters for the packed arrays of a math struct.

    PackedVector2Array and its three siblings are the last 207 methods of the unsupported_type
    bucket. They cross as a reference id like every other container, but unlike PackedInt32Array
    their elements are not scalars, so VhRefValues is what reads them -- one variant per element,
    each carrying the struct's lanes.
    """
    lines = []
    for lane in MATH_PACKED_LANES:
        element = lane.verse_type[2:]
        godot_element = next(n for n in MATH_TYPES if verse_class_name(n) == element)
        lines.append(
            f"{lane.to_fn}(Value:variant)<reads>:{lane.verse_type} ="
            f" for (V : VhRefValues(Value.Ref)) {{ VhTo{godot_element}(V) }}")
        lines.append(
            f"{lane.from_fn}(Values:{lane.verse_type})<reads>:variant ="
            f" VhFromValues({lane.tag}, for (V : Values) {{ VhFrom{godot_element}(V) }})")
    return lines


def emit_variant_readers(api: dict, enums: dict) -> list:
    """VariantKind over Godot's own variant_type, one As<GodotType> per lane, and the
    VariantFrom<GodotType> family."""
    values = check_variant_lanes(api)

    blocks = []

    # Godot's own Variant::Type enum, generated with the other 757 rather than declared again here:
    # a second enum for one thing would be two names for one question. Its enumerators keep the
    # TYPE_ prefix, because stripping it gives `Int` and `Float` and an enumerator is ambiguous with a
    # stdlib function of that name.
    kind_enum = enums["Variant.Type"]
    by_number = {number: name for name, number in kind_enum.values}
    case_arms = "\n".join(
        f"        {values[lane.godot_type]} => {kind_enum.verse_name}.{by_number[values[lane.godot_type]]}"
        for lane in VARIANT_LANES if values[lane.godot_type] in by_number)
    blocks.append(
        f"VariantKind<public>(Value:variant)<reads>:{kind_enum.verse_name} =\n"
        "    case (Value.Tag):\n"
        f"{case_arms}\n"
        f"        _ => {kind_enum.verse_name}.{by_number[values[VARIANT_NIL]]}")

    blocks.append("VhToObject(Value:variant)<decides><reads>:object = object[VhObjectFrom[Value]]")

    # Identity, so a `variant` parameter or return needs no special case in emit_method: the wire
    # already carries exactly this.
    blocks.append("VhToVariant(Value:variant)<reads>:variant = Value")
    blocks.append("VhFromVariant(Value:variant)<reads>:variant = Value")

    readers = []
    for lane in VARIANT_LANES:
        convert = (f"{lane.to_fn}[Value]" if lane.to_fn in VARIANT_DECIDES_CONVERTERS
                   else f"{lane.to_fn}(Value)")
        readers.append(
            f"As{lane.reader}<public>(Value:variant)<decides><reads>:{lane.verse_type} =\n"
            f"    Value.Tag = {lane.tag}\n"
            f"    {convert}")
    blocks.extend(readers)

    for lane in VARIANT_LANES:
        blocks.append(
            f"VariantFrom{lane.reader}<public>(Value:{lane.verse_type})<reads>:variant"
            f" = {lane.from_fn}(Value)")
    return blocks


TYPED_ARRAY_PREFIX = "typedarray::"
TYPED_DICT_PREFIX = "typeddictionary::"

# A raw C pointer -- `void*`, `const GDExtensionInitializationFunction*`. Three non-virtual
# methods across the whole API take one, and GDScript cannot call them either: there is no
# scripting spelling for an address. Its own permitted skip rather than an unsupported_type,
# because unsupported_type is the bucket Phase 2 promised to empty.
POINTER_TYPE_RE = re.compile(r"\*\s*$")


def typed_array_element_type(element: str):
    """The Godot type of a typed array's elements, from extension_api.json's spelling for one.

    Usually just the type name -- `typedarray::Node`. An array of *objects* is spelled with the
    property metadata Godot would have used in the inspector instead: `24/17:CompositorEffect` is
    Variant::OBJECT (24) with PROPERTY_HINT_RESOURCE_TYPE (17) naming the class. `27/0:` is a
    Dictionary with no hint and no name, so the variant type is all there is to go on.
    """
    if "/" not in element:
        return element
    tag, _, hinted = element.partition("/")
    _, _, name = hinted.partition(":")
    if name:
        return name
    lane = next((l for l in VARIANT_LANES if l.godot_type == GODOT_VARIANT_TYPE_BY_TAG.get(tag)), None)
    return lane.reader if lane else None


def typed_array_suffix(godot_element: str, element_info: TypeInfo) -> str:
    """The identifier half of `VhToNodeArray`, unique per element type.

    Godot's own type name rather than the Verse one, so the generated converter beside a mirrored
    `GetChildren` reads as the Godot type it came from. `RID` lowers to `Rid` because the Verse and
    C++ sides already spell it that way.
    """
    return "".join(part[0].upper() + part[1:] for part in split_pascal(godot_element))


def element_converters(suffix: str, info: TypeInfo):
    """The reader and writer a typed container's function-valued members are handed.

    `Unpack` and `Pack` are function *values*, and Verse has no anonymous functions, so each has to
    name something. Wherever the element type is one of Variant's own lanes, that is its
    `As<GodotType>` reader and `VariantFrom<GodotType>` builder -- the reader checks the tag and is
    already <decides>, which is the contract exactly. The lane is found by the element's packer rather
    than by its Godot name, because those names do not always agree: RID's reader is `AsRid`.

    A *class* element needs its own pair. Both halves have to name the class -- `VhFromObject` takes
    the base `object` where the member's declared type says `node`, and a Verse function type is not
    satisfied by one that merely accepts a supertype.
    """
    # An object element is checked first: VhFromObject *is* a lane's packer -- Variant's own Object
    # lane -- and taking that branch would hand back the base `object` where the member's declared
    # type says `node`.
    lane = None if info.pack_fn == "VhFromObject" else LANE_BY_PACKER.get(info.pack_fn)
    if lane is not None:
        return f"As{lane}", f"VariantFrom{lane}", []
    read = f"VhTo{suffix}Element"
    write = f"VhFrom{suffix}Element"
    return read, write, [
        f"{read}(Value:variant)<decides><reads>:{info.verse_type} ="
        f" {info.verse_type}[VhObjectFrom[Value]]",
        f"{write}(Value:{info.verse_type})<reads>:variant = VhFromObject(Value)",
    ]


def emit_typed_array_converters(typed_arrays: dict, typed_dictionaries: dict) -> list:
    """The module-scoped converters every typed container the API mentioned needs."""
    blocks = []
    emitted_elements = set()

    def element_pair(suffix: str, info: TypeInfo):
        """The reader and writer names, emitting their definitions the first time they are asked for.

        A class that is both an array's element and a dictionary's value would otherwise have its
        converter pair defined twice.
        """
        read, write, extra = element_converters(suffix, info)
        if read not in emitted_elements:
            emitted_elements.add(read)
            blocks.extend(extra)
        return read, write

    for suffix in sorted(typed_arrays):
        info = typed_arrays[suffix]
        read, write = element_pair(suffix, info)
        blocks.append(
            f"VhTo{suffix}Array(Value:variant)<reads>:typed_array({info.verse_type}) =\n"
            f"    Made := typed_array({info.verse_type})"
            f"{{Ref := Value.Ref, Unpack := {read}, Pack := {write}}}\n"
            f"    VhAdopt(Made)\n"
            f"    Made")
        blocks.append(
            f"VhFrom{suffix}Array(Value:typed_array({info.verse_type}))<reads>:variant ="
            f" variant{{Tag := TagArray, Ref := Value.Ref}}")
        # A script cannot spell the converter pair -- element_pair's functions are module-scoped --
        # so an empty typed container has to be minted here or not at all. R-TYPE-2's other half:
        # the mirrored methods taking a typed Array had nothing a script could pass them.
        blocks.append(
            f"Make{suffix}Array<public>()<reads>:typed_array({info.verse_type}) ="
            f" VhTo{suffix}Array(variant{{Tag := TagArray, Ref := VhRefNew(TagArray)}})")

    for suffix, (key_info, value_info) in sorted(typed_dictionaries.items()):
        _key_read, key_write = element_pair(f"{suffix}Key", key_info)
        value_read, value_write = element_pair(f"{suffix}Value", value_info)
        spelling = f"typed_dictionary({key_info.verse_type}, {value_info.verse_type})"
        blocks.append(
            f"VhTo{suffix}Dict(Value:variant)<reads>:{spelling} =\n"
            f"    Made := {spelling}"
            f"{{Ref := Value.Ref, PackKey := {key_write}, Unpack := {value_read}, Pack := {value_write}}}\n"
            f"    VhAdopt(Made)\n"
            f"    Made")
        blocks.append(
            f"VhFrom{suffix}Dict(Value:{spelling})<reads>:variant ="
            f" variant{{Tag := TagDictionary, Ref := Value.Ref}}")
        blocks.append(
            f"Make{suffix}Dict<public>()<reads>:{spelling} ="
            f" VhTo{suffix}Dict(variant{{Tag := TagDictionary, Ref := VhRefNew(TagDictionary)}})")
    return blocks


# What a script can read out of, or write into, a Godot container.
#
# One entry per element type, because a script cannot spell a `variant`: the packers are
# module-scoped so that a user cannot hold a raw reference (R-TYPE-7), which means every way into
# and out of a container has to be typed. Keyed by the suffix the accessor takes.
CONTAINER_ELEMENTS = [
    # First, and the one that makes the rest optional: a Godot Array is heterogeneous, and before
    # `variant` was nameable there was no way to read an element whose type the script did not
    # already know. The typed accessors below stay because knowing is the common case.
    ("Variant", "variant", "VhFromVariant", "VhToVariant"),
    ("Logic", "logic", "VhFromLogic", "VhToLogic"),
    ("Int", "int", "VhFromInt", "VhToInt"),
    ("Float", "float", "VhFromFloat", "VhToFloat"),
    ("String", "string", "VhFromString", "VhToString"),
    ("Vector2", "vector2", "VhFromVector2", "VhToVector2"),
    ("Vector2i", "vector2i", "VhFromVector2i", "VhToVector2i"),
    ("Vector3", "vector3", "VhFromVector3", "VhToVector3"),
    ("Color", "color", "VhFromColor", "VhToColor"),
    ("Array", "godot_array", "VhFromArray", "VhToArray"),
    ("Dictionary", "dictionary", "VhFromDictionary", "VhToDictionary"),
    # Last, and the only one whose reader can fail for a reason that is not the tag: a null object
    # crosses as an object-tagged zero, so VhToObject is <decides> and every use of it below takes
    # the bracket form. `typed_array(t)` covers the case where the element type is known; this is
    # for the plain heterogeneous Array, where before it there was no way to read an object element
    # at all without a `variant` a script cannot spell (R-TYPE-7).
    ("Object", "object", "VhFromObject", "VhToObject"),
]

# The key types each container is indexed by. An Array takes an int; a Dictionary takes whatever
# Godot lets it, of which these are the ones worth spelling -- a string key is the common case and
# an integer or a Vector2i key is what a tilemap uses.
# The reference wrappers, and the Godot type each stands for. Emitted into the host's layout header
# so the host can type a parameter declared as one of them.
REFERENCE_TYPES = [
    ("godot_array", "VH_VARIANT_ARRAY"),
    ("dictionary", "VH_VARIANT_DICTIONARY"),
    ("callable", "VH_VARIANT_CALLABLE"),
    ("signal_ref", "VH_VARIANT_SIGNAL"),
]

CONTAINER_KEYS = {
    "godot_array": [("Index", "int", "VhFromInt")],
    "dictionary": [
        ("Key", "string", "VhFromString"),
        ("Key", "int", "VhFromInt"),
        ("Key", "vector2i", "VhFromVector2i"),
    ],
}


def emit_container_classes() -> list:
    """godot_array and dictionary, with a typed accessor pair per element type and key type."""
    blocks = []
    for verse_name, keys in CONTAINER_KEYS.items():
        lines = [f"{verse_name}<public> := class<computes>(godot_ref):", ""]
        lines.append("    # How many elements it holds.")
        lines.append("    Length<public>()<reads>:int = VhRefSize(Ref)")
        lines.append("")
        if verse_name == "godot_array":
            lines.append("    # The whole thing as a Verse array of its elements' own type, which is a copy:")
            lines.append("    # Verse's arrays are values, so mutating what this returns reaches nothing.")
            for suffix, verse_type, _, unpack in CONTAINER_ELEMENTS:
                if verse_type in ("godot_array", "dictionary"):
                    continue
                # A reader that can fail filters instead of converting: an element that is not a t
                # is dropped rather than failing the whole call, which is what `for` with a failable
                # binding already reads as and what typed_array(t).ToArray does.
                body = (f"for (V : VhRefValues(Ref), E := {unpack}[V]) {{ E }}"
                        if unpack in VARIANT_DECIDES_CONVERTERS
                        else f"for (V : VhRefValues(Ref)) {{ {unpack}(V) }}")
                lines.append(f"    To{suffix}s<public>()<reads>:[]{verse_type} = {body}")
            lines.append("")

        for suffix, verse_type, pack, unpack in CONTAINER_ELEMENTS:
            read = "{0}[{1}]" if unpack in VARIANT_DECIDES_CONVERTERS else "{0}({1})"
            for key_name, key_type, key_pack in keys:
                # Failable: an absent key and an index out of range are ordinary misses, and a
                # value of another type is a miss too rather than a raise -- asking a container for
                # an int and getting a string back is the caller's question answered "no".
                lines.append(
                    f"    Get{suffix}<public>({key_name}:{key_type})<decides><reads>:{verse_type} ="
                    f" {read.format(unpack, f'VhRefGet[Ref, {key_pack}({key_name})]')}")
            for key_name, key_type, key_pack in keys:
                lines.append(
                    f"    Set{suffix}<public>({key_name}:{key_type}, Value:{verse_type})<transacts>:void ="
                    f" VhRefSet(Ref, {key_pack}({key_name}), {pack}(Value))")
            # An Array grows; a Dictionary has no position to append at.
            if verse_name == "godot_array":
                lines.append(
                    f"    Add{suffix}<public>(Value:{verse_type})<transacts>:void ="
                    f" VhRefSet(Ref, VhFromInt(VhRefSize(Ref)), {pack}(Value))")
        blocks.append("\n".join(lines))
    return blocks


def math_struct_name(godot_name: str) -> str:
    return verse_class_name(godot_name)


def math_leaf_lanes(godot_name: str, path: str = "") -> list:
    """Every scalar leaf under a math type, as (access path, "int"|"float"), depth first."""
    lanes = []
    for member, member_type in MATH_LAYOUT[godot_name]:
        here = f"{path}.{verse_method_name(member)}" if path else verse_method_name(member)
        if member_type in ("float", "int"):
            lanes.append((here, member_type))
        else:
            lanes.extend(math_leaf_lanes(member_type, here))
    return lanes


def emit_math_structs() -> list:
    blocks = []
    for godot_name in MATH_TYPES:
        # `<concrete><computes>`, which Epic's own vector2 also carries: without it a struct
        # literal inside a `<computes>` function is refused -- "this archetype instantiation
        # constructs a class that has the 'transacts' effect" -- and none of the math in
        # GodotMath.verse could construct its own result.
        lines = [f"{math_struct_name(godot_name)}<public> := struct<concrete><computes>:"]
        for member, member_type in MATH_LAYOUT[godot_name]:
            field = verse_method_name(member)
            if member_type == "float":
                lines.append(f"    {field}<public>:float = 0.0")
            elif member_type == "int":
                lines.append(f"    {field}<public>:int = 0")
            else:
                inner = math_struct_name(member_type)
                lines.append(f"    {field}<public>:{inner} = {inner}{{}}")
        blocks.append("\n".join(lines))
    return blocks


def build_math_literal(godot_name: str, reads: dict, path: str) -> str:
    """A struct literal for godot_name, reading each leaf out of `reads` by its access path."""
    parts = []
    for member, member_type in MATH_LAYOUT[godot_name]:
        field = verse_method_name(member)
        here = f"{path}.{field}" if path else field
        if member_type in ("float", "int"):
            parts.append(f"{field} := {reads[here]}")
        else:
            parts.append(f"{field} := {build_math_literal(member_type, reads, here)}")
    return f"{math_struct_name(godot_name)}{{{', '.join(parts)}}}"


def emit_math_packers() -> list:
    blocks = []
    for godot_name in MATH_TYPES:
        name = math_struct_name(godot_name)
        tag = MATH_TAGS[godot_name]
        lanes = math_leaf_lanes(godot_name)
        ints = [path for path, kind in lanes if kind == "int"]
        floats = [path for path, kind in lanes if kind == "float"]

        assigns = [f"Tag := {tag}"]
        assigns += [f"I{i} := Value.{path}" for i, path in enumerate(ints)]
        assigns += [f"F{i} := Value.{path}" for i, path in enumerate(floats)]
        blocks.append(
            f"VhFrom{godot_name}(Value:{name})<reads>:variant = variant{{{', '.join(assigns)}}}")

        reads = {}
        for i, path in enumerate(ints):
            reads[path] = f"Value.I{i}"
        for i, path in enumerate(floats):
            reads[path] = f"Value.F{i}"
        blocks.append(
            f"VhTo{godot_name}(Value:variant)<reads>:{name} =\n"
            f"    VhExpect(Value, {tag}, \"{name}\")\n"
            f"    {build_math_literal(godot_name, reads, '')}")
    return blocks


ClassifiedMethod = namedtuple(
    "ClassifiedMethod",
    ["godot_name", "verse_name", "params", "return_type", "is_void", "default_body", "is_const",
     "godot_return"],
    # A virtual is the only method with a default body, and it is what makes the declaration a
    # declaration rather than a call: everything else dispatches through the handle. `is_const` is
    # Godot's own flag, and it decides `<reads>` against `<transacts>` (docs/phase-4.5-design.md 3).
    defaults=(None, False, ""),
)
ClassifiedProperty = namedtuple(
    "ClassifiedProperty", ["godot_name", "verse_name", "type_info", "getter", "setter", "index"]
)
Param = namedtuple("Param", ["verse_name", "type_info", "default"])


def build_parent_map(classes: list) -> dict:
    return {c["name"]: c.get("inherits") for c in classes}


def compute_emit_set(requested: list, parent_map: dict) -> list:
    """Requested classes plus all their ancestors, in first-seen order.

    Object included, since Phase 2: it is an ordinary mirrored class whose base is the hand-written
    native root, `vh_object`. Before that its API was almost all Variant- and Callable-typed and
    there was nothing to emit.
    """
    emit_order = []
    emit_set = set()
    for name in requested:
        if name not in parent_map:
            continue
        chain = []
        cur = name
        while cur is not None and cur not in emit_set:
            chain.append(cur)
            cur = parent_map.get(cur)
        for c in reversed(chain):
            if c not in emit_set:
                emit_set.add(c)
                emit_order.append(c)
    return emit_order


def class_depth(name: str, parent_map: dict, memo: dict) -> int:
    if name in memo:
        return memo[name]
    parent = parent_map.get(name)
    depth = 0 if parent is None else 1 + class_depth(parent, parent_map, memo)
    memo[name] = depth
    return depth


def read_classes_file(path: Path) -> list:
    names = []
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.split("#", 1)[0].strip()
        if line:
            names.append(line)
    return names


# One skipped member, as the editor will need it: the name a script would have written, and why it
# is not there. R-SCN-2's second half -- "the method I need isn't there and I can't tell why" is the
# failure mode that ends adoption, and a report file nobody opens does not prevent it.
SkippedMember = namedtuple(
    "SkippedMember", ["verse_class", "verse_name", "godot_class", "godot_name", "reason", "detail"])


class Coverage:
    def __init__(self):
        self.classes_emitted = 0
        self.methods_emitted = 0
        self.properties_emitted = 0
        self.signals_emitted = 0
        self.constants_emitted = 0
        self.statics_emitted = 0
        self.utilities_emitted = 0
        self.skip_reasons = Counter()
        self.unsupported_types = Counter()
        # Every skip that costs a *name*, for the editor diagnostic. A skip that costs nothing --
        # a property whose accessors survive under their own names -- still goes in, because the
        # author who wrote the property name needs telling where it went.
        self.skipped_members = []
        # R-AUD-3's list: every emitted method whose `<transacts>` label promises a rollback the
        # bridge cannot perform. A method that mutates Godot *and* answers a value cannot be
        # deferred to commit -- the answer is needed now -- and the bridge forwards it rather than
        # performing it, so it has no inverse to register. Rows are
        # (godot class, godot method, verse class, verse method, return type).
        self.nonatomic = []

    def skip(self, reason: str, member: "SkippedMember | None" = None):
        self.skip_reasons[reason] += 1
        if member is not None:
            self.skipped_members.append(member)

    def unsupported(self, types_seen, member: "SkippedMember | None" = None):
        self.skip_reasons["unsupported_type"] += 1
        if member is not None:
            self.skipped_members.append(member._replace(detail=", ".join(sorted(set(types_seen)))))
        for t in set(types_seen):
            self.unsupported_types[t] += 1


# The math structs whose members are all scalars, as (field names, scalar type). Only these can be
# `var` properties: Verse asks a struct-typed var for a field-named accessor overload per nesting
# level, and for a struct whose members are *themselves* structs there is no single signature that
# satisfies it -- transform3d's two members are a `basis` and a `vector3`, and one getter cannot
# return both. So a nested type stays a pair of ordinary methods; see classify_property.
FLAT_MATH_STRUCTS = {
    verse_class_name(name): (
        [verse_method_name(member) for member, _ in layout],
        layout[0][1],
    )
    for name, layout in MATH_LAYOUT.items()
    if all(member_type in ("float", "int") for _, member_type in layout)
}

VECTOR_FIELDS = {name: fields for name, (fields, _) in FLAT_MATH_STRUCTS.items()}


def verse_default_literal(verse_type: str, default: str):
    """Godot's default_value spelled as a Verse literal, or None when it has no spelling.

    Unrepresentable defaults (null, Callable(), [], {}) leave the parameter required rather than
    inventing a value, so the only cost is that the caller has to pass one.
    """
    if verse_type == "logic":
        return default if default in ("true", "false") else None
    if verse_type == "int":
        return default if re.fullmatch(r"-?\d+", default) else None
    if verse_type == "float":
        if not re.fullmatch(r"-?\d+(\.\d+)?", default):
            return None
        # Godot writes some float defaults as integers, which Verse will not take for a float.
        return default if "." in default else default + ".0"
    if verse_type == "string":
        if re.fullmatch(r'&?"[^"\\]*"', default):
            return default.lstrip("&")
        m = re.fullmatch(r'NodePath\("([^"\\]*)"\)', default)
        return f'"{m.group(1)}"' if m else None

    flat = FLAT_MATH_STRUCTS.get(verse_type)
    if flat:
        fields, scalar = flat
        # Godot spells the constructor with its own class name -- `Vector2i(0, 0)` -- which is the
        # Verse name with the underscores taken out and each word capitalised again.
        godot_name = "".join(part.capitalize() for part in verse_type.split("_"))
        m = re.fullmatch(rf"{re.escape(godot_name)}\(([^)]*)\)", default, re.IGNORECASE)
        if not m:
            return None
        parts = [p.strip() for p in m.group(1).split(",")]
        if len(parts) != len(fields):
            return None
        # An integer vector's components are ints, and Verse will not take 0.0 for one.
        numbers = [verse_default_literal(scalar, p) for p in parts]
        if any(n is None for n in numbers):
            return None
        return verse_type + "{" + ", ".join(f"{f} := {n}" for f, n in zip(fields, numbers)) + "}"
    return None


# Godot methods that answer a value, are not marked `const`, and provably do not mutate.
#
# A mirrored method's effect comes from Godot's `is_const` (docs/phase-4.5-design.md §3), and that
# flag is applied unevenly: `Tween::is_running` is `bool is_running() { return running; }` and is
# not marked const, so without this table it would be `<transacts>` and unusable from a `<reads>`
# helper. These are the exceptions, and they are **read out of Godot's source rather than judged**:
# `tools/audit_const_overrides.py` produced every row, each body is exactly `return <member>;`, and
# the comment on each row is that body. Re-run it against a newer Godot to revise the list.
#
# The audit is one-sided on purpose. A wrong `<reads>` claims more than it should and would let a
# mutation escape the rollback the label promised; a conservative `<transacts>` merely claims less
# than it could, which costs an author a `<transacts>` on their own helper and nothing else. So the
# audit rejects a literal return (a base-class stub) and any name declared `virtual` anywhere —
# which over-rejects, `Tween::is_valid` among them, and that is the right direction to be wrong in.
#
# This is the third hand-curated table in this file and the only one whose rows were *verified*
# rather than decided: VERSE_AMBIGUOUS_MEMBER_NAMES is compiler-confirmed, PROPERTY_RENAMES is
# invented, and this one is quoted.
#
# Most of these rows change nothing today: 54 of the 127 reach a method the mirror emits under its
# own name, and the rest name a getter superseded by a property. They stay because the list is a
# statement about *Godot*, not about what this mirror happens to emit -- a property that stops being
# emitted should bring back a correctly labelled method rather than a silently wrong one.
CONST_OVERRIDES = frozenset([
    ("AudioEffectCapture", "get_buffer_length"),  # return buffer_length_seconds;
    ("AudioEffectDelay", "get_dry"),  # return dry;
    ("ButtonGroup", "is_allow_unpress"),  # return allow_unpress;
    ("CPUParticles2D", "get_split_scale"),  # return split_scale;
    ("CPUParticles3D", "get_split_scale"),  # return split_scale;
    ("CSGMesh3D", "get_mesh"),  # return mesh;
    ("CSGPrimitive3D", "get_flip_faces"),  # return flip_faces;
    ("CharacterBody2D", "get_floor_snap_length"),  # return floor_snap_length;
    ("CharacterBody3D", "get_floor_snap_length"),  # return floor_snap_length;
    ("ConfirmationDialog", "get_cancel_button"),  # return cancel;
    ("EditorFileSystem", "get_filesystem"),  # return filesystem;
    ("EditorFileSystemDirectory", "get_parent"),  # return parent;
    ("EditorInspector", "get_edited_object"),  # return object;
    ("EditorProperty", "get_edited_object"),  # return object;
    ("Engine", "get_frames_drawn"),  # return frames_drawn;
    ("FBXState", "get_allow_geometry_helper_nodes"),  # return allow_geometry_helper_nodes;
    ("GLTFAnimation", "get_original_name"),  # return original_name;
    ("GLTFLight", "get_inner_cone_angle"),  # return inner_cone_angle;
    ("GLTFLight", "get_intensity"),  # return intensity;
    ("GLTFLight", "get_light_type"),  # return light_type;
    ("GLTFLight", "get_outer_cone_angle"),  # return outer_cone_angle;
    ("GLTFLight", "get_range"),  # return range;
    ("GLTFMesh", "get_mesh"),  # return mesh;
    ("GLTFMesh", "get_original_name"),  # return original_name;
    ("GLTFNode", "get_camera"),  # return camera;
    ("GLTFNode", "get_light"),  # return light;
    ("GLTFNode", "get_mesh"),  # return mesh;
    ("GLTFNode", "get_original_name"),  # return original_name;
    ("GLTFNode", "get_parent"),  # return parent;
    ("GLTFNode", "get_skeleton"),  # return skeleton;
    ("GLTFNode", "get_skin"),  # return skin;
    ("GLTFNode", "get_visible"),  # return visible;
    ("GLTFNode", "get_xform"),  # return transform;
    ("GLTFSkeleton", "get_godot_skeleton"),  # return godot_skeleton;
    ("GLTFSkin", "get_godot_skin"),  # return godot_skin;
    ("GLTFSkin", "get_joints"),  # return joints;
    ("GLTFSkin", "get_joints_original"),  # return joints_original;
    ("GLTFSkin", "get_non_joints"),  # return non_joints;
    ("GLTFSkin", "get_roots"),  # return roots;
    ("GLTFSkin", "get_skeleton"),  # return skeleton;
    ("GLTFSkin", "get_skin_root"),  # return skin_root;
    ("GLTFSpecGloss", "get_diffuse_factor"),  # return diffuse_factor;
    ("GLTFSpecGloss", "get_diffuse_img"),  # return diffuse_img;
    ("GLTFSpecGloss", "get_gloss_factor"),  # return gloss_factor;
    ("GLTFSpecGloss", "get_spec_gloss_img"),  # return spec_gloss_img;
    ("GLTFSpecGloss", "get_specular_factor"),  # return specular_factor;
    ("GeometryInstance3D", "is_ignoring_occlusion_culling"),  # return ignore_occlusion_culling;
    ("GodotInstance", "is_started"),  # return started;
    ("Gradient", "get_interpolation_color_space"),  # return interpolation_color_space;
    ("Gradient", "get_interpolation_mode"),  # return interpolation_mode;
    ("GraphEdit", "get_menu_hbox"),  # return menu_hbox;
    ("GraphElement", "is_draggable"),  # return draggable;
    ("GraphElement", "is_selectable"),  # return selectable;
    ("GraphElement", "is_selected"),  # return selected;
    ("GraphFrame", "get_titlebar_hbox"),  # return titlebar_hbox;
    ("GraphNode", "get_titlebar_hbox"),  # return titlebar_hbox;
    ("GridMap", "is_baking_navigation"),  # return bake_navigation;
    ("HTTPRequest", "get_timeout"),  # return timeout;
    ("Input", "is_using_accumulated_input"),  # return use_accumulated_input;
    ("InputEventShortcut", "get_shortcut"),  # return shortcut;
    ("ItemList", "is_scroll_hint_tiled"),  # return tile_scroll_hint;
    ("JavaScriptBridge", "pwa_update"),  # return ERR_UNAVAILABLE;
    ("LineEdit", "get_right_icon"),  # return right_icon;
    ("LineEdit", "is_context_menu_enabled"),  # return context_menu_enabled;
    ("MenuBar", "is_switch_on_hover"),  # return switch_on_hover;
    ("MenuButton", "is_switch_on_hover"),  # return switch_on_hover;
    ("MeshInstance3D", "get_skeleton_path"),  # return skeleton_path;
    ("MultiplayerSynchronizer", "get_replication_config"),  # return replication_config;
    ("NavigationAgent2D", "get_path_max_distance"),  # return path_max_distance;
    ("NavigationAgent3D", "get_path_max_distance"),  # return path_max_distance;
    ("NavigationMesh", "get_agent_radius"),  # return agent_radius;
    ("NoiseTexture2D", "get_bump_strength"),  # return bump_strength;
    ("NoiseTexture2D", "get_noise"),  # return noise;
    ("NoiseTexture2D", "get_seamless"),  # return seamless;
    ("NoiseTexture2D", "get_seamless_blend_skirt"),  # return seamless_blend_skirt;
    ("NoiseTexture2D", "is_normal_map"),  # return as_normal_map;
    ("NoiseTexture3D", "get_noise"),  # return noise;
    ("NoiseTexture3D", "get_seamless"),  # return seamless;
    ("NoiseTexture3D", "get_seamless_blend_skirt"),  # return seamless_blend_skirt;
    ("OpenXRSpatialAnchorCapability", "is_spatial_anchor_supported"),  # return spatial_anchor_supported;
    ("Parallax2D", "get_follow_viewport"),  # return follow_viewport;
    ("Parallax2D", "is_ignore_camera_scroll"),  # return ignore_camera_scroll;
    ("ParallaxBackground", "is_ignore_camera_zoom"),  # return ignore_camera_zoom;
    ("ParticleProcessMaterial", "get_inherit_velocity_ratio"),  # return inherit_emitter_velocity_ratio;
    ("ParticleProcessMaterial", "get_velocity_pivot"),  # return velocity_pivot;
    ("Performance", "get_monitor_modification_time"),  # return _monitor_modification_time;
    ("PhysicalBone3D", "get_simulate_physics"),  # return simulate_physics;
    ("PhysicalBone3D", "is_simulating_physics"),  # return _internal_simulate_physics;
    ("PhysicalBone3D", "is_using_custom_integrator"),  # return custom_integrator;
    ("ProgressBar", "get_fill_mode"),  # return mode;
    ("RigidBody2D", "is_using_custom_integrator"),  # return custom_integrator;
    ("RigidBody3D", "is_using_custom_integrator"),  # return custom_integrator;
    ("ScrollContainer", "get_draw_focus_border"),  # return draw_focus_border;
    ("ScrollContainer", "get_h_scroll_bar"),  # return h_scroll;
    ("ScrollContainer", "get_v_scroll_bar"),  # return v_scroll;
    ("ScrollContainer", "is_scroll_hint_tiled"),  # return tile_scroll_hint;
    ("SkeletonIK3D", "get_target_node"),  # return target_node_path_override;
    ("SkeletonIK3D", "is_running"),  # return internal_active;
    ("SkeletonModification2D", "get_enabled"),  # return enabled;
    ("SkeletonModification2D", "get_modification_stack"),  # return stack;
    ("SkeletonProfile", "get_root_bone"),  # return root_bone;
    ("SkeletonProfile", "get_scale_base_bone"),  # return scale_base_bone;
    ("SpinBox", "get_line_edit"),  # return line_edit;
    ("SpringArm3D", "get_hit_length"),  # return current_spring_length;
    ("SubViewportContainer", "is_mouse_target_enabled"),  # return mouse_target;
    ("TextureProgressBar", "get_fill_degrees"),  # return rad_max_degrees;
    ("TextureProgressBar", "get_fill_mode"),  # return mode;
    ("TextureProgressBar", "get_radial_center_offset"),  # return rad_center_off;
    ("TextureProgressBar", "get_radial_initial_angle"),  # return rad_init_angle;
    ("ThemeDB", "get_default_theme"),  # return default_theme;
    ("ThemeDB", "get_fallback_base_scale"),  # return fallback_base_scale;
    ("ThemeDB", "get_fallback_font"),  # return fallback_font;
    ("ThemeDB", "get_fallback_font_size"),  # return fallback_font_size;
    ("ThemeDB", "get_fallback_icon"),  # return fallback_icon;
    ("ThemeDB", "get_fallback_stylebox"),  # return fallback_stylebox;
    ("ThemeDB", "get_project_theme"),  # return project_theme;
    ("Timer", "is_ignoring_time_scale"),  # return ignore_time_scale;
    ("Tree", "is_scroll_hint_tiled"),  # return tile_scroll_hint;
    ("TreeItem", "is_collapsed"),  # return collapsed;
    ("TreeItem", "is_visible"),  # return visible;
    ("Tween", "is_running"),  # return running;
    ("Viewport", "get_physics_object_picking_first_only"),  # return physics_object_picking_first_only;
    ("VisibleOnScreenEnabler2D", "get_enable_mode"),  # return enable_mode;
    ("VisibleOnScreenEnabler2D", "get_enable_node_path"),  # return enable_node_path;
    ("VisibleOnScreenEnabler3D", "get_enable_mode"),  # return enable_mode;
    ("VisibleOnScreenEnabler3D", "get_enable_node_path"),  # return enable_node_path;
    ("XMLParser", "get_node_type"),  # return node_type;
])


def classify_method(m: dict, resolver: TypeResolver, coverage: Coverage, members: set,
                    godot_class: str = ""):
    """Returns a ClassifiedMethod, or None (and records why in coverage) if the method is skipped."""
    def record(reason: str, detail: str = ""):
        return SkippedMember(verse_class_name(godot_class), verse_method_name(m["name"]),
                             godot_class, m["name"], reason, detail)

    is_virtual = bool(m.get("is_virtual"))
    if m.get("is_static"):
        coverage.skip("static", record("static"))
        return None
    if m.get("is_vararg"):
        coverage.skip("vararg", record("vararg"))
        return None
    if (godot_class, m["name"]) in FREE_FUNCTION_REPLACEMENTS:
        coverage.skip("superseded_by_free_function", record(
            "superseded_by_free_function", f"`{FREE_FUNCTION_REPLACEMENTS[(godot_class, m['name'])]}`"))
        return None
    if not is_virtual and verse_method_name(m["name"]) in VERSE_AMBIGUOUS_MEMBER_NAMES:
        raise ValueError(
            f"{godot_class}.{m['name']} would be a method named "
            f"{verse_method_name(m['name'])}, which Verse's own definition of that name is "
            f"ambiguous with. Give it a FREE_FUNCTION_REPLACEMENTS entry.")

    unsupported_seen = []

    return_value = m.get("return_value")
    is_void = return_value is None
    return_info = None
    if not is_void:
        return_info = resolver.classify(return_value["type"])
        if return_info is None:
            unsupported_seen.append(return_value["type"])

    params = []
    used_param_names = set()
    for i, arg in enumerate(m.get("arguments") or []):
        info = resolver.classify(arg["type"])
        if info is None or info.pack_fn is None:
            unsupported_seen.append(arg["type"])
            continue
        pname = verse_param_name(arg["name"], i, RESERVED_WORDS, used_param_names, members)
        used_param_names.add(pname)
        default = arg.get("default_value")
        params.append(Param(pname, info, verse_default_literal(info.verse_type, default) if default else None))

    if unsupported_seen:
        if all(POINTER_TYPE_RE.search(seen) for seen in unsupported_seen):
            coverage.skip("unmarshallable_pointer", record("unmarshallable_pointer"))
        else:
            coverage.unsupported(unsupported_seen, record("unsupported_type"))
        return None

    # An optional parameter may not be followed by a required one, and a Godot default that had
    # no Verse spelling leaves its parameter required, so trailing defaults only.
    for i in range(len(params)):
        if params[i].default is not None and any(p.default is None for p in params[i + 1:]):
            params[i] = params[i]._replace(default=None)

    default_body = None
    if is_virtual:
        default_body = virtual_default(return_info, resolver.enums)
        if default_body is None:
            coverage.skip("virtual_no_default", record(
                "virtual_no_default", f"`{return_info.verse_type}`" if return_info else ""))
            return None

    return ClassifiedMethod(
        godot_name=m["name"],
        verse_name=verse_virtual_name(m["name"]) if is_virtual else verse_method_name(m["name"]),
        params=params,
        return_type=return_info,
        is_void=is_void,
        default_body=default_body,
        # `const` *and* answering a value. Godot's `const` means "does not mutate the C++ object",
        # which is not the same as "has no effect": the 38 methods that are const and return nothing
        # are `OS.set_environment`, `OS.delay_msec`, `CanvasItem.draw_string` and 35 more of that
        # shape. Every one does something a later read can see, so the conjunction is the test.
        #
        # CONST_OVERRIDES is the other direction, where Godot's flag is missing rather than too
        # broad, and every row in it was read out of Godot's source by tools/audit_const_overrides.py.
        is_const=(bool(m.get("is_const")) or (godot_class, m["name"]) in CONST_OVERRIDES)
        and not is_void,
        # Godot's own spelling of the return type, kept only so the R-AUD-3 appendix can say what
        # shape a non-atomic method is -- an Error, the receiver, an object, or a plain value.
        godot_return=return_value["type"] if return_value else "",
    )


# Godot's `@GlobalScope` utilities that are *dispatched* rather than answered by Verse's own
# stdlib, and the only ones: the random family. R-AUD-2 says Verse's spelling wins where Verse has
# a counterpart, and `GetRandomFloat` is a counterpart -- but a Verse-side RNG would silently ignore
# `seed()` and `randomize()`, so a project that seeds for a replay would get a different game.
# Godot's C# makes the same exception for the same reason.
#
# The other 106 utilities are recorded as skips: most are Verse's own under Verse's name (Abs, Sqrt,
# Clamp, Lerp, ...), and the rest have no dispatch yet because the GDExtension interface offers no
# by-name utility call that takes Variants -- only a ptrcall wanting a signature hash.
# A utility whose Verse name would be ambiguous with something already visible, and what it is
# called instead. `seed` collides with the `Seed` *property* on six classes -- a `var` has no
# signature to be told apart by -- so the free function takes the longer name and the property keeps
# Godot's. Recorded as a skip too, so the editor answers either spelling.
UTILITY_RENAMES = {"seed": "SeedRandom"}

# The same thing for a *constant*, and there is one: Godot's `Color.TAN` is the colour, and Verse's
# `Tan` is the trigonometric function. Data has no signature to be told apart by, and Verse refuses
# an ambiguous definition even inside a module, so the constant takes the longer name. The other
# nine collisions across the whole API are `MIN`/`MAX` (which PROPERTY_RENAMES already answers) and
# `INF` (which has no Verse float literal and is skipped for that).
CONSTANT_RENAMES = {"Tan": "TanColor"}

DISPATCHED_UTILITIES = {
    "randf", "randi", "randf_range", "randi_range", "randfn", "randomize", "seed", "rand_from_seed",
}

# Godot's utility -> the Verse spelling that answers it. R-AUD-2 decides these: where the two differ
# only in *spelling*, Verse's wins, so the utility is not emitted and the author is told what to
# write instead. Without this the skip said "utility_not_dispatched" and nothing else, which told an
# author who typed `floor(x)` exactly nothing.
#
# The `...F` names are GodotMath's, and they exist because Verse's own Floor, Ceil and Round answer
# an *int* where Godot's answer a float -- two different functions, not one renamed.
UTILITY_VERSE_SPELLINGS = {
    "abs": "Abs(X)", "absf": "Abs(X)", "absi": "Abs(X)",
    "ceil": "Ceil[X]", "ceilf": "CeilF(X)", "ceili": "Ceil[X]",
    "floor": "Floor[X]", "floorf": "FloorF(X)", "floori": "Floor[X]",
    "round": "Round[X]", "roundf": "RoundF(X)", "roundi": "Round[X]",
    "clamp": "Clamp(X, Low, High)", "clampf": "Clamp(X, Low, High)", "clampi": "Clamp(X, Low, High)",
    "min": "Min(A, B)", "minf": "Min(A, B)", "mini": "Min(A, B)",
    "max": "Max(A, B)", "maxf": "Max(A, B)", "maxi": "Max(A, B)",
    "sign": "Sgn(X)", "signf": "Sgn(X)", "signi": "Sgn(X)",
    "sqrt": "Sqrt(X)", "exp": "Exp(X)", "log": "Ln(X)", "pow": "Pow(X, Y)",
    "sinh": "Sinh(X)", "cosh": "Cosh(X)", "tanh": "Tanh(X)",
    "asinh": "Asinh(X)", "acosh": "Acosh[X]", "atanh": "Atanh[X]",
    "ease": "Ease(X, Curve)", "db_to_linear": "DbToLinear(Db)", "linear_to_db": "LinearToDb[Linear]",
    "cubic_interpolate": "CubicInterpolate(From, To, Pre, Post, T)",
    "bezier_interpolate": "BezierInterpolate(Start, C1, C2, End, T)",
    "bezier_derivative": "BezierDerivative(Start, C1, C2, End, T)",
    "is_nan": "IsNan[X]", "is_inf": "IsInf[X]", "is_finite": "IsFinite[X]",
    "fmod": "FMod(A, B)", "nearest_po2": "NearestPo2(Value)", "step_decimals": "StepDecimals(Step)",
    "rid_from_int64": "RidFromInt64(From)",
    "cubic_interpolate_in_time": "CubicInterpolateInTime(...)",
    "cubic_interpolate_angle": "CubicInterpolateAngle(From, To, Pre, Post, T)",
    "cubic_interpolate_angle_in_time": "CubicInterpolateAngleInTime(...)",
    "sin": "Sin(X)", "cos": "Cos(X)", "tan": "Tan(X)",
    "asin": "ArcSin(X)", "acos": "ArcCos(X)", "atan": "ArcTan(X)", "atan2": "ArcTan(Y, X)",
    "lerp": "Lerp(A, B, T)", "lerpf": "Lerp(A, B, T)",
    "fmod": "Mod[X, Y]", "posmod": "Mod[X, Y]", "fposmod": "Mod[X, Y]",
    "snapped": "Snapped(X, Step)", "snappedf": "Snapped(X, Step)", "snappedi": "Snapped(X, Step)",
    "is_equal_approx": "IsEqualApprox[A, B]", "is_zero_approx": "IsZeroApprox[X]",
    "inverse_lerp": "InverseLerp(From, To, X)",
    "remap": "Remap(X, InFrom, InTo, OutFrom, OutTo)",
    "move_toward": "MoveToward(From, To, Delta)",
    "rotate_toward": "RotateToward(From, To, Delta)",
    "smoothstep": "Smoothstep(From, To, X)",
    "wrap": "WrapF(X, Min, Max)", "wrapf": "WrapF(X, Min, Max)", "wrapi": "WrapF(X, Min, Max)",
    "pingpong": "PingPong(X, Length)",
    "lerp_angle": "LerpAngle(From, To, T)",
    "angle_difference": "AngleDifference(From, To)",
    "deg_to_rad": "DegToRad(Degrees)", "rad_to_deg": "RadToDeg(Radians)",
    "is_instance_valid": "IsInstanceValid[Object]",
    "print": "Print(Text)",
    # Dispatched, and spelled by hand in GodotApi.native.verse rather than generated, because the
    # Verse signature is deliberately not Godot's: these are vararg there and one argument here.
    "push_error": "PushError(Text)", "push_warning": "PushWarning(Text)",
    "print_rich": "PrintRich(Text)", "printerr": "PrintErr(Text)",
    "print_verbose": "PrintVerbose(Text)", "printraw": "PrintRaw(Text)",
    "type_string": "VariantTypeName(VariantType)", "error_string": "ErrorString(Error)",
    "instance_from_id": "InstanceFromId[Id]", "is_instance_id_valid": "IsInstanceIdValid[Id]",
    "rid_allocate_id": "RidAllocateId()",
}

# Utilities whose parameter or return type is a `Variant`, which R-TYPE-7 keeps a script from
# spelling: the packers are module-scoped, so there is no signature these could be given that a
# script could call. Structural rather than unbuilt, which is why they get a reason of their own.
UTILITY_VARIANT_ONLY = {
    "hash", "typeof", "type_convert", "is_same", "weakref",
    "var_to_str", "str_to_var", "var_to_bytes", "bytes_to_var",
    "var_to_bytes_with_objects", "bytes_to_var_with_objects",
    "str", "printt", "prints",
}


def emit_utility_functions(api: dict, resolver: TypeResolver, coverage: Coverage) -> list:
    """The `@GlobalScope` utilities the bridge dispatches, as module-level Verse functions."""
    blocks = []
    for utility in api.get("utility_functions", []):
        name = utility["name"]
        verse_name = UTILITY_RENAMES.get(name, verse_method_name(name))
        if name not in DISPATCHED_UTILITIES:
            # Three reasons, and only the third is a gap. Which one it is decides what the editor
            # tells an author who typed the Godot name.
            if name in UTILITY_VERSE_SPELLINGS:
                reason, detail = "utility_has_verse_spelling", UTILITY_VERSE_SPELLINGS[name]
            elif name in UTILITY_VARIANT_ONLY:
                reason, detail = "utility_variant_only", ""
            else:
                reason, detail = "utility_not_dispatched", ""
            coverage.skip(reason, SkippedMember("", verse_name, "@GlobalScope", name, reason, detail))
            continue

        params = []
        used = set()
        unsupported = False
        for index, arg in enumerate(utility.get("arguments") or []):
            info = resolver.classify(arg["type"])
            if info is None or info.pack_fn is None:
                unsupported = True
                break
            pname = verse_param_name(arg["name"], index, RESERVED_WORDS, used, set())
            used.add(pname)
            params.append(Param(pname, info, None))
        if unsupported:
            coverage.skip("utility_not_dispatched", SkippedMember(
                "", verse_name, "@GlobalScope", name, "utility_not_dispatched", ""))
            continue

        return_type = utility.get("return_type")
        info = resolver.classify(return_type) if return_type else None
        decl = ", ".join(f"{q.verse_name}:{q.type_info.verse_type}" for q in params)
        args = emit_call_args(params)
        call = f'VhCallUtility("{name}", array{{{args}}})'
        if info is None:
            blocks.append(f"    {verse_name}<public>({decl})<transacts>:void = {{ {call} }}")
        elif info.unpack_decides:
            blocks.append(f"    {verse_name}<public>({decl})<decides><transacts>:{info.verse_type}"
                          f" = {info.unpack_fn}[{call}]")
        else:
            blocks.append(f"    {verse_name}<public>({decl})<transacts>:{info.verse_type}"
                          f" = {info.unpack_fn}({call})")
        if name in UTILITY_RENAMES:
            coverage.skip("utility_renamed", SkippedMember(
                "", verse_method_name(name), "@GlobalScope", name, "utility_renamed",
                f"`{UTILITY_RENAMES[name]}`"))
        coverage.utilities_emitted += 1
    return blocks


def emit_static_methods(api: dict, emit_order: list, resolver: TypeResolver,
                        coverage: Coverage) -> list:
    """Godot's 114 statics, as members of each class's `...Statics` module.

    A static is not a member of the mirrored class: Verse has no `static` keyword, so it goes where
    the class's constants go and is reached the same way -- `TweenStatics.InterpolateValue(...)`.
    """
    by_class = {}
    emitted = set(emit_order)
    for godot_class in api["classes"]:
        if godot_class["name"] not in emitted:
            continue
        for method in godot_class.get("methods", []) or []:
            if not method.get("is_static"):
                continue
            by_class.setdefault(godot_class["name"], []).append(method)

    blocks = []
    for godot_class, methods in sorted(by_class.items()):
        lines = []
        for method in methods:
            verse_name = verse_method_name(method["name"])
            params = []
            used = set()
            unsupported = False
            for index, arg in enumerate(method.get("arguments") or []):
                info = resolver.classify(arg["type"])
                if info is None or info.pack_fn is None:
                    unsupported = True
                    break
                pname = verse_param_name(arg["name"], index, RESERVED_WORDS, used, set())
                used.add(pname)
                params.append(Param(pname, info, None))
            return_value = method.get("return_value")
            info = resolver.classify(return_value["type"]) if return_value else None
            if unsupported or (return_value and info is None) or method.get("is_vararg"):
                coverage.skip("static_unsupported", SkippedMember(
                    verse_class_name(godot_class), verse_name, godot_class, method["name"],
                    "static_unsupported", ""))
                continue

            decl = ", ".join(f"{q.verse_name}:{q.type_info.verse_type}" for q in params)
            args = emit_call_args(params)
            call = f'VhCallStatic("{godot_class}", "{method["name"]}", array{{{args}}})'
            if info is None:
                lines.append(f"    {verse_name}<public>({decl})<transacts>:void = {{ {call} }}")
            elif info.pack_fn == "VhFromObject":
                lines.append(f"    {verse_name}<public>({decl})<decides><transacts>:{info.verse_type}"
                             f" = {info.verse_type}[VhObjectFrom[{call}]]")
            elif info.unpack_decides:
                lines.append(f"    {verse_name}<public>({decl})<decides><transacts>:{info.verse_type}"
                             f" = {info.unpack_fn}[{call}]")
            else:
                lines.append(f"    {verse_name}<public>({decl})<transacts>:{info.verse_type}"
                             f" = {info.unpack_fn}({call})")
            coverage.statics_emitted += 1
        if lines:
            blocks.append((godot_class, lines))
    return blocks


def statics_module_name(godot_class: str) -> str:
    """`Node` -> `NodeStatics`. The suffix is not decoration.

    Verse has no constant on a type, and Epic hit the same wall -- `SpatialMath` writes `Zero2()`
    with a TODO wishing for `vector2.Zero`. What Verse does have is inline modules with qualified
    access, so the constants go in one.

    The suffix is what keeps the module from being a top-level name an author reaches for. Verse
    refuses a *local* that resolves ambiguously against a visible definition, so a module named
    `Tree` would break `if (Tree := GetTree[])`, which the yardstick writes today, and one named
    `Node` would break any local of that name. `...Statics` is a name nobody reaches for.
    """
    # Godot's own spelling of the class, verbatim: `NodeStatics`, `Vector2iStatics`, `AABBStatics`.
    # It is what an author looks for, and a mirrored class name is lowercase, so the two cannot meet.
    return godot_class + "Statics"


def emit_statics_module(godot_class: str, constants: list, resolver: TypeResolver,
                        coverage: Coverage, enums: dict) -> str:
    """One inline module of Godot's constants for a class, or "" when none of them can be written.

    An inline module needs no `using`: `NodeStatics.NotificationReady` reaches it from any script
    that imports the package, which is every script.
    """
    lines = []
    for constant in constants:
        # SCREAMING_SNAKE to PascalCase: `NOTIFICATION_ENTER_TREE` is `NotificationEnterTree`, `UP`
        # is `Up`. verse_method_name keeps the tail of each part as it found it, which is right for
        # `get_max` and wrong for a constant, where the whole name is shouting.
        name = "".join(part.capitalize() for part in constant["name"].split("_") if part)
        # `Vector2i.MIN` would be data named `Min`, which is ambiguous with /Verse.org/Verse's
        # function of that name -- data has no signature to be told apart by. The same rename a
        # property in that position gets, and recorded the same way so either spelling is answered.
        renamed = PROPERTY_RENAMES.get(name) or CONSTANT_RENAMES.get(name)
        if renamed:
            coverage.skip("constant_renamed", SkippedMember(
                verse_class_name(godot_class), name, godot_class, constant["name"],
                "constant_renamed", f"`{renamed}`"))
            name = renamed
        godot_type = constant.get("type")
        if godot_type is None:
            # A class constant, which is always an int -- extension_api.json gives these a value
            # and no type at all.
            lines.append(f"    {name}<public>:int = {constant['value']}")
            continue
        info = resolver.classify(godot_type)
        literal = verse_default_literal(info.verse_type, constant.get("value")) if info else None
        if literal is None:
            # `Vector2.INF` is `Vector2(inf, inf)`, and an infinity has no Verse float literal.
            # Recorded rather than dropped, so the editor can say where it went.
            coverage.skip("constant_no_literal", SkippedMember(
                verse_class_name(godot_class), name, godot_class, constant["name"],
                "constant_no_literal", f"`{constant.get('value')}`"))
            continue
        lines.append(f"    {name}<public>:{info.verse_type} = {literal}")

    if not lines:
        return ""
    return f"{statics_module_name(godot_class)}<public> := module:\n" + "\n".join(lines)


def emit_statics_modules(api: dict, emit_order: list, resolver: TypeResolver,
                         coverage: Coverage, enums: dict) -> list:
    """One module per owner, carrying its constants and its static methods.

    Both for the same reason: Verse has no constant and no static *on* a type, and an inline module
    with qualified access is what it has instead.
    """
    blocks = []
    by_name = {c["name"]: c for c in api["classes"]}
    static_lines = dict(emit_static_methods(api, emit_order, resolver, coverage))
    for name in emit_order:
        constants = by_name[name].get("constants") or []
        block = emit_statics_module(name, constants, resolver, coverage, enums) if constants else ""
        statics = static_lines.get(name)
        if statics:
            if block:
                block += "\n" + "\n".join(statics)
            else:
                block = f"{statics_module_name(name)}<public> := module:\n" + "\n".join(statics)
        if block:
            blocks.append(block)
            coverage.constants_emitted += len(constants)
    for builtin in api.get("builtin_classes", []):
        if builtin["name"] not in MATH_TYPES:
            continue
        constants = builtin.get("constants") or []
        if constants:
            block = emit_statics_module(builtin["name"], constants, resolver, coverage, enums)
            if block:
                blocks.append(block)
                coverage.constants_emitted += block.count("<public>:")
    return blocks


def emit_signal_accessor(godot_class: str, sig: dict, resolver: TypeResolver, coverage: Coverage):
    """One of Godot's own signals, as an accessor answering a `signal(t)` bound to that handle.

    A method rather than a data member, which is what C# does too: a mirror wrapper is built per
    crossing, and a member would have to be filled on each one.

    The payload follows the same rule a declared signal's does -- nothing is `tuple()`, one argument
    is that argument's own type, more are a tuple -- so a handler written with N parameters
    subscribes to a signal Godot emits with N arguments, for the reason a Verse function's parameter
    *is* its tuple.
    """
    verse_class = verse_class_name(godot_class)
    name = verse_method_name(sig["name"])

    infos = []
    for arg in sig.get("arguments") or []:
        info = resolver.classify(arg["type"])
        if info is None or info.pack_fn is None:
            coverage.skip("signal_unsupported_payload", SkippedMember(
                verse_class, name, godot_class, sig["name"],
                "signal_unsupported_payload", f"`{arg['type']}`"))
            return None
        infos.append(info)

    if not infos:
        payload = "tuple()"
    elif len(infos) == 1:
        payload = infos[0].verse_type
    else:
        payload = "tuple(" + ", ".join(i.verse_type for i in infos) + ")"

    return (name, f'    {name}<public>()<transacts>:signal({payload}) ='
                  f' signal({payload}){{Id := VhSignalBind(Handle, "{verse_class}",'
                  f' "{name}", "{sig["name"]}")}}')


def emit_call_args(params) -> str:
    return ", ".join(
        f"{p.type_info.pack_fn}({p.verse_name})" if not p.type_info.pack_decides
        else f"{p.type_info.pack_fn}[{p.verse_name}]"
        for p in params
    )


def emit_method(cm: ClassifiedMethod) -> str:
    param_decl = ", ".join(
        f"{p.verse_name}:{p.type_info.verse_type}" if p.default is None
        else f"?{p.verse_name}:{p.type_info.verse_type} = {p.default}"
        for p in cm.params
    )

    # A virtual is a declaration to override, not a call to make: the body is what Godot's own
    # default means, and a script says `<override>` over it. **No effect specifier**, exactly as the
    # three hand-written lifecycle methods carried none -- the default set is wider than
    # `<transacts>`, so an overriding body may call whatever Godot it likes, where narrowing here
    # would refuse a body that called a specifier-less helper.
    if cm.default_body is not None:
        result = "void" if cm.is_void else cm.return_type.verse_type
        return f"    {cm.verse_name}<public>({param_decl}):{result} = {cm.default_body}"

    args = emit_call_args(cm.params)
    # `<reads>` for a const method, and the native it dispatches through says why. The pair has to
    # agree: a `<reads>` body may not call `VhCallValue`, which is `<transacts>`.
    effect = "<reads>" if cm.is_const else "<transacts>"
    dispatch = "VhCallValueConst" if cm.is_const else "VhCallValue"
    call = f'{dispatch}(Handle, "{cm.godot_name}", array{{{args}}})' if not cm.is_void else None

    if cm.is_void:
        body = f'VhCallVoid(Handle, "{cm.godot_name}", array{{{args}}})'
        return f"    {cm.verse_name}<public>({param_decl})<transacts>:void = {body}"

    ti = cm.return_type
    if ti.pack_fn == "VhFromObject":
        # The cast, not a construction: the host builds the object at the class Godot says it is,
        # and this narrows it to what the signature promised (R-SCN-6). It can decline -- Godot
        # answering a class outside the mirror -- and the method was already <decides> for null.
        body = f"{ti.verse_type}[VhObjectFrom[{call}]]"
    elif ti.unpack_decides:
        body = f"{ti.unpack_fn}[{call}]"
    else:
        body = f"{ti.unpack_fn}({call})"

    effects = f"<decides>{effect}" if ti.unpack_decides else effect
    return f"    {cm.verse_name}<public>({param_decl}){effects}:{ti.verse_type} = {body}"


# `string` is []char, so the compiler asks a string-typed property for (:accessor, :int):char and
# (:accessor, :int, :char):void as well -- element accessors, to make `Node.Text[3]` resolve. What
# a write past the end of the string should do has no answer that is not invented, and an accessor
# may not fail, so these keep their get/set methods instead.
# A `var` property whose type is a container cannot work: Verse asks a struct- or array-typed var
# for field-named accessor overloads it cannot satisfy for these. Godot's own getter and setter are
# emitted as ordinary methods instead, so the value is still reachable.
#
# `string` is here because Verse spells it `[]char`, so the compiler treats it as an array too.
CONTAINER_PROPERTY_TYPES = {"string", "godot_array", "dictionary", "callable", "signal_ref"}

# Names the generated accessor bodies bind. A Godot member that lands on one of these (Range.value
# does) would be ambiguous rather than shadowed at the point the body mentions it.
ACCESSOR_LOCAL_NAMES = ("Accessor", "Field", "Value", "Current")


def property_godot_type(p: dict, methods_by_name: dict) -> str:
    """A property's type, taking the getter's word for it over the property's own.

    Godot's property metadata reports an enum-typed property as a plain `int` -- `Node.process_mode`
    is `int` where `get_process_mode` returns `enum::Node.ProcessMode`. That is true of **515 of the
    994** int properties, including the one R-SCN-5's exit criterion is written about, so without this
    the enums land everywhere except where a script most often reaches for them.

    Only `enum::` is taken this way. A bitfield stays an int on purpose: a combination of flags is not
    an enumerator, so a `var` of the enum type could not hold what Godot puts in it.
    """
    declared = p["type"]
    if declared != "int":
        return declared
    getter = methods_by_name.get(p.get("getter") or "")
    from_getter = ((getter or {}).get("return_value") or {}).get("type", "")
    return from_getter if from_getter.startswith("enum::") else declared


def classify_property(p: dict, resolver: TypeResolver, coverage: Coverage, methods_by_name: dict,
                      godot_class: str = ""):
    """Returns a ClassifiedProperty, or None (and records why) if the property is skipped.

    Every one of these leaves Godot's own getter and setter standing as ordinary methods, so what is
    lost is the `set X.Y = ...` spelling rather than the value -- which is exactly what the recorded
    skip goes on to tell an author who wrote the property name.
    """
    accessors = " and ".join(
        f"`{verse_method_name(n)}()`" for n in (p.get("getter"), p.get("setter")) if n)

    def record(reason: str):
        return SkippedMember(verse_class_name(godot_class), pascal_member_name(p["name"]),
                             godot_class, p["name"], reason, accessors)

    if not p.get("getter") or not p.get("setter"):
        coverage.skip("property_no_accessor_pair", record("property_no_accessor_pair"))
        return None

    info = resolver.classify(property_godot_type(p, methods_by_name))
    if info is None:
        coverage.unsupported([p["type"]], record("unsupported_type"))
        return None
    # A `var` is *data*, and data cannot overload: Verse rejects a member named Max outright
    # because /Verse.org/Verse's Max is in scope at its declaration, where a zero-argument *method*
    # of the same name would have been distinguished by its signature. So the property is dropped
    # and Godot's own getter and setter survive as methods -- GetMax() still reads it; what is lost
    # is only `set Node.Max = ...`.

    if info.verse_type in CONTAINER_PROPERTY_TYPES or info.verse_type.startswith("[]"):
        coverage.skip("property_container_type", record("property_container_type"))
        return None
    # A `variant` is a struct, so the compiler asks a var of that type for a field-named accessor
    # overload per field -- and every one of variant's fields is module-scoped, so none of them can
    # appear in a public signature. Godot's own getter and setter are emitted as methods instead.
    if info.verse_type == "variant":
        coverage.skip("property_variant_type", record("property_variant_type"))
        return None
    # An object-typed property would need a getter that cannot fail, and a null Godot object is
    # exactly the absence VhToHandle reports as failure.
    if info.pack_fn == "VhFromObject":
        coverage.skip("property_object_type", record("property_object_type"))
        return None
    # A nested math struct: see FLAT_MATH_STRUCTS. Skipping it here leaves Godot's own getter and
    # setter to be emitted as ordinary methods, so `GetGlobalTransform()` still reaches it -- what
    # is lost is only the `set Node.GlobalTransform = ...` spelling.
    if info.verse_type in MATH_STRUCT_NAMES and info.verse_type not in FLAT_MATH_STRUCTS:
        coverage.skip("property_nested_struct", record("property_nested_struct"))
        return None

    # A property whose own name is ambiguous with a Verse function takes the name beside it in
    # PROPERTY_RENAMES, and the name it did not get is recorded so the editor can point at the one it
    # did. `set X.Maximum = ...` rather than no property at all.
    verse_name = pascal_member_name(p["name"])
    if verse_name in PROPERTY_RENAMES:
        coverage.skip("property_renamed", record("property_renamed")._replace(
            detail=f"`{PROPERTY_RENAMES[verse_name]}`"))
        verse_name = PROPERTY_RENAMES[verse_name]

    return ClassifiedProperty(
        godot_name=p["name"],
        verse_name=verse_name,
        type_info=info,
        getter=p["getter"],
        setter=p["setter"],
        index=p.get("index"),
    )


def accessor_locals(members: set) -> dict:
    """The names an accessor body binds, renamed away from anything the class already carries."""
    chosen = {}
    for i, base in enumerate(ACCESSOR_LOCAL_NAMES):
        taken = base in members or base in RESERVED_WORDS or base in VERSE_STDLIB_NAMES
        chosen[base] = f"Arg{i}" if taken else base
    return chosen


def emit_property(cp: ClassifiedProperty, names: dict) -> list:
    """The var and the two-to-four accessor overloads the compiler requires for it."""
    accessor, field, value, current = (
        names["Accessor"], names["Field"], names["Value"], names["Current"]
    )
    ti = cp.type_info
    index_arg = f"VhFromInt({cp.index})" if cp.index is not None else ""

    read = f'{ti.unpack_fn}(VhCallValue(Handle, "{cp.getter}", array{{{index_arg}}}))'

    def write(expr: str) -> str:
        args = ", ".join(a for a in (index_arg, f"{ti.pack_fn}({expr})") if a)
        return f'VhCallVoid(Handle, "{cp.setter}", array{{{args}}})'

    get_name = f"{cp.verse_name}Getter"
    set_name = f"{cp.verse_name}Setter"

    # The accessors are epic_internal because their `accessor` parameter is: a definition may be
    # no more accessible than what it depends on. Only the compiler ever names them, at the point
    # it rewrites a read or a write of the public var above.
    lines = [
        f"    var {cp.verse_name}<public><getter({get_name})><setter({set_name})>"
        f":{ti.verse_type} = external {{}}",
        f"    {get_name}<epic_internal>({accessor}:accessor)<transacts>:{ti.verse_type} = {read}",
        f"    {set_name}<epic_internal>({accessor}:accessor, {value}:{ti.verse_type})<transacts>:void"
        f" = {write(value)}",
    ]

    # The compiler demands a field-named overload of each accessor for a struct-typed var, so that
    # `set Node.Position.X = 1.0` could resolve to a read of the whole vector and a write back.
    # Nothing can reach them: it walks a struct's fields without checking that any is assignable,
    # and a Verse struct may not contain a `var`, so the path it is asking about cannot be written.
    # They are emitted to satisfy the check and are dead.
    flat = FLAT_MATH_STRUCTS.get(ti.verse_type)
    if not flat:
        return lines
    fields, scalar = flat

    selects = " else ".join(
        f'if ({field} = "{f}") then {current}.{f}' for f in fields[:-1]
    )
    lines += [
        f"    {get_name}<epic_internal>({accessor}:accessor, {field}:string)<transacts>:{scalar} =",
        f"        {current} := {read}",
        f"        {selects} else {current}.{fields[-1]}",
        f"    {set_name}<epic_internal>({accessor}:accessor, {field}:string, {value}:{scalar}) <transacts>:void =".replace(") <", ")<"),
        f"        {current} := {read}",
    ]
    for i, f in enumerate(fields):
        members = ", ".join(
            f"{g} := {value}" if g == f else f"{g} := {current}.{g}" for g in fields
        )
        guard = "else" if i == len(fields) - 1 else f'{"else " if i else ""}if ({field} = "{f}")'
        lines.append(f"        {guard}:")
        lines.append(f"            {write(ti.verse_type + '{' + members + '}')}")
    return lines


def generate(api: dict, requested: list, coverage: Coverage, enums: dict):
    classes = api["classes"]
    parent_map = build_parent_map(classes)
    class_names = set(parent_map.keys())
    classes_by_name = {c["name"]: c for c in classes}

    emit_order = compute_emit_set(requested, parent_map)
    depth_memo = {}
    emit_order.sort(key=lambda n: (class_depth(n, parent_map, depth_memo), verse_class_name(n)))

    emit_set = set(emit_order)
    resolver = TypeResolver(emit_set, parent_map, class_names, enums)

    inherited_names = {}  # godot class name -> set of Verse names visible to its subclasses
    class_blocks = []
    method_map = []  # (godot class, verse class, godot method, verse method, is virtual) per member
    # Every name any emitted class carries, across all of them. A module-level definition may not
    # share one: see singleton_accessor_name. (member_names below is one class' own set.)
    all_member_names = set()

    for name in emit_order:
        parent = parent_map[name]
        # Godot's Object has no Godot parent; its base is the hand-written native root, whose four
        # members every mirrored class inherits.
        base_names = set(BASE_MEMBER_NAMES) if parent is None else set(inherited_names[parent])
        base_verse = NATIVE_ROOT if parent is None else verse_class_name(parent)

        methods = classes_by_name[name].get("methods", [])

        methods_by_name = {m["name"]: m for m in methods}

        properties = []
        for p in classes_by_name[name].get("properties", []):
            cp = classify_property(p, resolver, coverage, methods_by_name, name)
            if cp is not None:
                properties.append(cp)
        properties.sort(key=lambda cp: cp.verse_name)

        # A property replaces the pair it was built from: `Position` is the point of the exercise,
        # and leaving GetPosition beside it would be two spellings of one thing. Only the pairs
        # that actually became properties are dropped -- a getter whose property was skipped is
        # still the only way to read it.
        superseded = {cp.getter for cp in properties} | {cp.setter for cp in properties}

        # Every name the class will carry, so a parameter can be checked against members that
        # have not been classified yet as well as inherited ones.
        member_names = base_names | {verse_method_name(m["name"]) for m in methods}
        member_names |= {verse_method_name(sg["name"]) for sg in classes_by_name[name].get("signals", [])}
        for cp in properties:
            member_names |= {cp.verse_name, f"{cp.verse_name}Getter", f"{cp.verse_name}Setter"}
        locals_for_accessors = accessor_locals(member_names)

        candidates = []
        for m in methods:
            if m["name"] in superseded:
                replacement = next(cp.verse_name for cp in properties
                                   if m["name"] in (cp.getter, cp.setter))
                coverage.skip("superseded_by_property", SkippedMember(
                    verse_class_name(name), verse_method_name(m["name"]), name, m["name"],
                    "superseded_by_property", f"`{replacement}`"))
                continue
            cm = classify_method(m, resolver, coverage, member_names, name)
            if cm is not None:
                candidates.append(cm)
        candidates.sort(key=lambda cm: (cm.verse_name, cm.godot_name))

        used = set(base_names)
        emitted_lines = []
        for cp in properties:
            names = {cp.verse_name, f"{cp.verse_name}Getter", f"{cp.verse_name}Setter"}
            if names & used:
                coverage.skip("shadow")
                continue
            used |= names
            all_member_names |= names
            emitted_lines.extend(emit_property(cp, locals_for_accessors))
            method_map.append((name, verse_class_name(name), cp.godot_name, cp.verse_name, False))
            coverage.properties_emitted += 1

        for cm in candidates:
            if cm.verse_name in used:
                coverage.skip("shadow", SkippedMember(
                    verse_class_name(name), cm.verse_name, name, cm.godot_name, "shadow", ""))
                continue
            used.add(cm.verse_name)
            all_member_names.add(cm.verse_name)
            emitted_lines.append(emit_method(cm))
            method_map.append((name, verse_class_name(name), cm.godot_name, cm.verse_name,
                               cm.default_body is not None))
            coverage.methods_emitted += 1
            # Recorded as it is emitted rather than recomputed afterwards, so the list cannot
            # disagree with the mirror -- the same reason verse_api_skipped.h is generated.
            if not cm.is_const and not cm.is_void and cm.default_body is None:
                coverage.nonatomic.append((name, cm.godot_name, verse_class_name(name),
                                           cm.verse_name, cm.godot_return))

        # Godot's own signals, last, so a name a method or property already took wins: an accessor
        # is the convenience and the member is the API. Every collision of the kind that would have
        # mattered -- `Node.ready`, `CanvasItem.draw`, `Control.gui_input`, `BaseButton.pressed` --
        # was with a *virtual*, and those keep Godot's leading underscore, so none of them meet.
        for sig in classes_by_name[name].get("signals", []):
            emitted = emit_signal_accessor(name, sig, resolver, coverage)
            if emitted is None:
                continue
            signal_name, line = emitted
            if signal_name in used:
                coverage.skip("shadow", SkippedMember(
                    verse_class_name(name), signal_name, name, sig["name"], "shadow", ""))
                continue
            used.add(signal_name)
            all_member_names.add(signal_name)
            emitted_lines.append(line)
            coverage.signals_emitted += 1

        inherited_names[name] = used
        coverage.classes_emitted += 1

        header = f"{verse_class_name(name)}<public> := class({base_verse}):"
        if emitted_lines:
            class_blocks.append(header + "\n\n" + "\n".join(emitted_lines))
        else:
            class_blocks.append(header)

    return (class_blocks, emit_order, method_map, all_member_names, resolver.typed_arrays,
            resolver.typed_dictionaries)


HEADER_TEMPLATE = """using {{/Verse.org/Native}}

# Generated by tools/gen_verse_api.py from godot-cpp/gdextension/extension_api-4-7.json
# ({version}). Do not edit by hand.
#
# Godot's Object is mirrored like every other class, and derives from the hand-written native
# `vh_object` (see Godot.native.verse) -- which is what gives a script's class a UObject
# representation the host can instantiate and call into. Nothing a script writes should name
# `vh_object`; `object` is the base to derive from, and it carries Godot's own Object API.
"""


ENUMS_TEMPLATE = """
# --- Godot's enums -----------------------------------------------------------
#
# R-SCN-5: every Godot enum as a real Verse enum, so `SetProcessMode(node_process_mode.Always)`
# compiles and `SetProcessMode(2)` does not. The type is class-qualified because 96 bare enum names
# repeat across Godot's classes and the whole project shares one flat scope.
#
# Enumerators drop the prefix their own names share -- `PROCESS_MODE_ALWAYS` in Node.ProcessMode is
# `node_process_mode.Always` -- all or nothing per enum, and not at all when a stripped name would be
# illegal, duplicated, reserved, or ambiguous with a Verse stdlib function. `_MAX` sentinels and
# duplicate-valued aliases are dropped: the first is a count rather than a value, and the second would
# put two `case` arms on one number.
#
# A bitfield's parameters stay `int`, because a combination of flags is not an enumerator. Its enum is
# declared anyway so the flags have names, and Verse's own bitwise intrinsics combine them:
# `BitOr(ToInt(key_modifier_mask.Ctrl), ToInt(key_modifier_mask.Shift))`.

{enums}
"""


VARIANT_TEMPLATE = """
# --- Godot's Variant ---------------------------------------------------------
#
# A Godot value whose type is not known until it arrives. The type is nameable so it can appear in a
# signature; its lanes are not, so a script reads one through the failable `As<GodotType>` readers
# below and builds one through the matching `VariantFrom<GodotType>` (R-TYPE-7, amended in Phase 2).
# Neither family is overloaded -- the type is in the name, for the reason gen_verse_api.py records
# where it emits them.
#
#     if (Health := AsInt[V]):
#         Print("hp {{Health}}")
#
#     case (VariantKind(V)):          # when the type is not known at all
#         variant_type.TypeInt => Print("an int")
#         _ => Print("something else")
#
#     Node.SetMeta("score", VariantFromInt(42))
#
# Reading is a <decides> free function rather than a cast, because Verse's own cast rejects a struct
# on both sides -- see docs/phase-2-design.md 1 and 4.1. It reads the same at the call site and it
# boxes nothing.

{readers}

# --- the packed arrays of a math type ----------------------------------------
#
# These ride as a reference like every other container, but their elements are structs rather than
# scalars, so each crosses as its own variant instead of flattening into a lane sequence.

{packed}
"""


CONTAINERS_TEMPLATE = """
# --- Godot's containers ------------------------------------------------------
#
# A typed accessor pair per element type, because a script cannot spell a `variant` -- the packers
# are module-scoped so that a user cannot hold a raw reference that outlives what it names
# (R-TYPE-7). Every read is failable: a key that is absent, an index out of range, and an element
# of another type are all the same answer, and it is "no" rather than an error.

{classes}
"""


MATH_TEMPLATE = """
# --- Godot's math types ------------------------------------------------------
#
# Each mirrors its own member tree, so `Transform.Basis.X.X` reads the way `transform.basis.x.x`
# does in GDScript. Data only: the methods Godot gives these are R-SCN-3 and arrive in Phase 2.

{structs}

# --- and their packers -------------------------------------------------------
#
# The float lanes carry a type's components in Godot's own order, depth first over the member tree
# above -- which is the order src/verse_value.cpp writes and reads them in. The two have to agree
# component for component, so both are derived from the same tree rather than written twice.

{packers}
"""


TYPED_ARRAYS_TEMPLATE = """
# --- typed containers --------------------------------------------------------
#
# What `typedarray::Node` becomes: a typed_array(node) (see GodotApi.native.verse), built from the
# same reference id a godot_array holds and carrying the element conversion as a function value.
#
# Module-scoped and Vh-prefixed, none of it in a script's completion. The element converter is the
# `As<GodotType>` reader wherever one exists -- it already checks the tag and is already <decides>,
# which is the contract -- and its own function only where the element is a *class*, because
# building one has to name it.

{converters}
"""


UTILITIES_TEMPLATE = """
# --- @GlobalScope utilities --------------------------------------------------
#
# In a module rather than at module scope, and not for tidiness: a module-level `Randf` is ambiguous
# with `random_number_generator.Randf`, because Verse refuses a definition that resolves
# ambiguously against anything visible. `GodotStatics.Randf()` reaches one and
# `MyRng.Randf()` still reaches the other.
#
# The one exception to R-AUD-2, and only this family: Verse has `GetRandomFloat`, but a Verse-side
# RNG would silently ignore `Seed()` and `Randomize()`, so a project that seeds for a replay would
# get a different game. These steer Godot's own stream. C# makes the same exception for the same
# reason.

GodotStatics<public> := module:
{functions}
"""


STATICS_TEMPLATE = """
# --- constants ---------------------------------------------------------------
#
# Godot's class constants and its math types', as one inline module per owner: `NodeStatics`,
# `Vector2Statics`. Verse has no constant *on* a type -- Epic hit the same wall and wrote `Zero2()`
# with a TODO wishing for `vector2.Zero` -- and an inline module needs no `using`, so
# `NodeStatics.NotificationReady` reaches one from any script that imports this package.
#
# The `...Statics` suffix is load-bearing rather than decoration. Verse refuses a *local* that
# resolves ambiguously against a visible definition, so a module named `Tree` would break
# `if (Tree := GetTree[])` -- which the yardstick writes today -- and one named `Node` would break
# any local of that name.

{modules}
"""


SINGLETONS_TEMPLATE = """
# Godot hands a singleton out by name rather than through the scene, so a mirrored `input` or
# `engine` would otherwise be a class no script can obtain an instance of. <decides> because
# Engine::get_singleton answers nothing for a name this build did not register -- an editor-only
# singleton asked for in an exported game, say.

{accessors}
"""


def singleton_accessor_name(godot_name: str, member_names: set) -> str:
    """`GetInput`, unless a mirrored class already carries a member of that name.

    Within one package a module-level function and a class member of the same name are ambiguous
    **whatever their signatures** -- `XRController3D.GetInput(:[]char)` against the accessor for the
    Input singleton, which takes nothing. (Across packages a signature does disambiguate, which is
    why `godot_array.Length()` coexists with /Verse.org/Verse's `Length`.) The accessor is this
    generator's invention and the member is Godot's, so the accessor is the one that moves.
    """
    base = f"Get{godot_name}"
    return f"{base}Singleton" if base in member_names else base


def emit_singleton_accessors(api: dict, emit_order: list, member_names: set) -> list:
    """One module-level accessor per emitted class that Godot registers as a singleton."""
    singletons = {s["name"] for s in api.get("singletons", [])}
    return [
        # A cast over what the host built, not a construction -- the same road every object-returning
        # method takes, and R-SCN-6's rule that the class an object crosses as is the class Godot
        # says it is rather than the one the signature named. It used to construct, and Phase 4.5 had
        # to change it: an archetype instantiation carries the constructing class's own effect, a
        # mirrored class descends from the native `vh_object` and so is `<transacts>` to construct,
        # and that made a `<reads>` accessor impossible. Casting has no such effect and was the more
        # correct spelling anyway.
        f'{singleton_accessor_name(name, member_names)}<public>()<decides><reads>'
        f':{verse_class_name(name)}'
        f' = {verse_class_name(name)}[VhObjectOf(VhSingleton["{name}"])]'
        for name in sorted(n for n in emit_order if n in singletons)
    ]


def render(api: dict, class_blocks: list, singleton_accessors: list, typed_arrays: dict,
           typed_dictionaries: dict, enums: dict, statics_modules: list, utilities: list) -> str:
    version = api["header"]["version_full_name"]
    text = HEADER_TEMPLATE.format(version=version)
    text += MATH_TEMPLATE.format(
        structs="\n\n".join(emit_math_structs()),
        packers="\n".join(emit_math_packers()),
    )
    text += ENUMS_TEMPLATE.format(enums="\n\n".join(emit_enums(enums)))
    text += VARIANT_TEMPLATE.format(
        readers="\n\n".join(emit_variant_readers(api, enums)),
        packed="\n".join(emit_math_packed_converters()),
    )
    text += CONTAINERS_TEMPLATE.format(classes="\n\n".join(emit_container_classes()))
    if typed_arrays or typed_dictionaries:
        text += TYPED_ARRAYS_TEMPLATE.format(
            converters="\n\n".join(emit_typed_array_converters(typed_arrays, typed_dictionaries)))
    text += "\n" + "\n\n".join(class_blocks) + "\n"
    if utilities:
        text += UTILITIES_TEMPLATE.format(functions="\n".join(utilities))
    if statics_modules:
        text += STATICS_TEMPLATE.format(modules="\n\n".join(statics_modules))
    if singleton_accessors:
        text += SINGLETONS_TEMPLATE.format(accessors="\n".join(singleton_accessors))
    return text


MATH_LAYOUT_HEADER_TEMPLATE = """// Copyright Epic Games, Inc. All Rights Reserved.
#pragma once

// Generated by tools/gen_verse_api.py from godot-cpp/gdextension/extension_api-4-7.json
// ({version}). Do not edit by hand.
//
// The shape of each Godot math type as Verse declares it, for the host's marshalling: which fields
// a struct has, in order, and which of them are structs of their own. Generated from the same
// MATH_LAYOUT that emits the Verse structs and their packers, so the two cannot drift -- a value
// built here with the fields of a type it is not would be read back as plausible nonsense.
//
// Scalar fields carry NestedTag 0. A nested one carries the tag of the struct it holds, which is
// its entry in MathLayouts below.

namespace verse_math {{

struct field
{{
	const char *name;
	// 0 for a scalar; otherwise the vh_variant_tag of the struct this field holds.
	int nested_tag;
	// A scalar field holding an integer rather than a float -- the integer vectors, whose
	// components stop being exact as doubles above 2^53.
	bool is_int;
}};

struct layout
{{
	const char *verse_name;
	int variant_tag;
	// The packed array Godot has for this struct, which an array of them crosses as. 0 for none.
	int packed_array_tag;
	const field *fields;
	int field_count;
}};

{field_arrays}

inline constexpr layout layouts[] = {{
{entries}
}};

// The Verse classes that wrap a reference id rather than carrying a value, and the Godot type each
// names. The host needs this to report a parameter's or a result's type across the ABI: without
// it Godot is told the argument is Nil and refuses to pass one.
struct reference_type
{{
	const char *verse_name;
	int variant_tag;
}};

inline constexpr reference_type reference_types[] = {{
{reference_entries}
}};

}} // namespace verse_math
"""


CLASSES_HEADER_TEMPLATE = """#pragma once

// Generated by tools/gen_verse_api.py from godot-cpp/gdextension/extension_api-4-7.json
// ({version}). Do not edit by hand.
//
// Maps each Godot class this bridge emits a Verse class for to that class's name, so the
// Godot side (VerseScriptLanguage::_make_template) can turn a node's Godot class into the
// Verse class it should subclass without re-deriving gen_verse_api.py's name transform.
// A Godot class that was not emitted (see GodotClasses.native.verse's coverage report) is
// absent here; the caller walks ClassDB::get_parent_class until it finds one that is.

namespace verse_api {{

struct class_mapping {{
\tconst char *godot_name;
\tconst char *verse_name;
}};

inline constexpr class_mapping classes[] = {{
{entries}
}};

// Maps each emitted method back to the Godot method it mirrors, so a symbol the editor resolved
// to a Verse name can be looked up in Godot's own class documentation. The Verse name alone is
// not enough to invert: the transform drops the underscores that separated the words.
//
// Keyed by the class the method is *declared* on, which is what the compiler reports as a
// resolved definition's enclosing scope -- an inherited call resolves to the declaring class,
// not the one it was called through.
// `is_virtual` is what tells the two halves of this table apart, and the editor needs it: every
// mirrored method is a class member the compiler would accept an `<override>` of, but overriding a
// concrete one -- `GetName` -- compiles and changes nothing, because the body forwards through the
// handle either way. Only a virtual is a method Godot itself will dispatch to.
struct method_mapping {{
	const char *verse_class;
	const char *verse_method;
	const char *godot_class;
	const char *godot_method;
	bool is_virtual;
}};

inline constexpr method_mapping methods[] = {{
{method_entries}
}};

}} // namespace verse_api
"""


SKIPPED_HEADER_PATH = "src/verse_api_skipped.h"

SKIPPED_HEADER_TEMPLATE = """#pragma once

// Generated by tools/gen_verse_api.py from godot-cpp/gdextension/extension_api-4-7.json
// ({version}). Do not edit by hand.
//
// Every Godot member the mirror does not carry under its own name, and why. R-SCN-2's second half:
// "the method I need isn't there and I can't tell why" is the failure mode that ends adoption, and a
// report file nobody opens does not prevent it. VerseScriptLanguage reads this when the compiler
// says `Unknown member X in Y`, and answers with where the member went.
//
// Keyed by the Verse spellings, because those are what the diagnostic carries -- the Verse name
// cannot be inverted on its own, since the transform to PascalCase drops the underscores that
// separated the words.
//
// `detail` is the reason's own payload and reads differently per reason: the property that replaced
// a getter, the accessors that survived a property, or the Godot types nothing could carry.

namespace verse_api {{

struct skipped_member {{
\tconst char *verse_class;
\tconst char *verse_name;
\tconst char *godot_class;
\tconst char *godot_name;
\tconst char *reason;
\tconst char *detail;
}};

inline constexpr skipped_member skipped[] = {{
{entries}
}};

}} // namespace verse_api
"""



NONATOMIC_PATH = "docs/nonatomic-methods.md"

NONATOMIC_HEADER = """<!-- Generated by tools/gen_verse_api.py from {version}. Do not hand-edit. -->

# The methods whose `<transacts>` is not kept

**Generated.** R-AUD-3 asks for a list rather than an assurance, and this is it, written by the same
pass that writes the mirror so it cannot drift from the code. `docs/spec.md` R-AUD-1 is the rule
this is the appendix to, and `docs/phase-4.5-design.md` 4 is why it exists.

## What the label promises, and where it is not kept

`<transacts>` says a call takes part in the enclosing transaction: if the transaction fails, the
call is undone. The bridge keeps that three ways and breaks it one way.

| what | how the promise is kept |
| --- | --- |
| a method that mutates and returns **nothing** | deferred to `AutoRTFM::OnCommit`. A failed expression never performs it, which `tests/integration` measures in both directions |
| a method that is **const and answers a value** | `<reads>` since Phase 4.5. Nothing to undo, and the label no longer forces `<transacts>` onto the caller |
| `signal.Subscribe` | **compensated**: the host registers an `AutoRTFM::OnAbort<SameAsClosed>` that disconnects |
| a method that **mutates and answers a value** | **not kept.** The answer is needed now, so the call cannot be deferred, and the bridge forwards it to Godot rather than performing it, so it has no inverse to register |

Signal **emission** joins them by decision rather than by shape: `signal.Signal` runs its
handlers immediately, the way GDScript does, so a transaction that later aborts has already run
them. `docs/phase-4-design.md` 6.4 is the argument.

**Why none of these is compensated.** Compensation needs an inverse, and Godot publishes none: there
is no `un-load`, no `un-create_shape_owner`, no way to take back a `Tween.tween_property`. A
per-method inverse table would be {count} rows of invented semantics, each of which would be wrong
for some caller, and a compensation that half works is worse than a documented sharp edge --
GDScript offers exactly this and says nothing at all.

**Subscription is the exception, and there is now a compensated spelling for both halves of it.**
`signal.Subscribe` covers a signal the mirror knows about, and since Phase 5
`MakeSignal(Owner, Name).Subscribe(...)` covers one it does not -- a signal a GDScript or C# script
declared, or one made with `add_user_signal`. Both register an `AutoRTFM::OnAbort` that disconnects.
`Object.Connect` is the unforgiving general form under them (R-SIG-6) and is in the list below; it
stays what it is, and a script that cares reaches for one of the two above instead.

## The shapes

| shape | count | what the mutation is |
| --- | --- | --- |
| answers a **value** | {value} | a method Godot did not mark `const`. Some of these do not visibly mutate anything -- `Tween.is_running` is not `const` and reads like a query -- but `is_const` is Godot's own annotation and the mirror believes it rather than second-guessing 708 of them |
| answers an **Error** | {error} | an operation that reports whether it worked: `load`, `save`, `send`, `connect_node`. Undoing one would mean undoing I/O |
| answers an **object** | {object} | an allocation the caller now owns -- `create_shape_owner`, `Tween.tween_property`. Undoing it means freeing something the caller may still hold |
| answers its **receiver** | {receiver} | a builder chaining, where the mutation is the point: `PropertyTweener.SetDelay(...).SetEase(...)` |

## The list

{count} methods, by Godot class.

"""


def render_nonatomic(api: dict, coverage: "Coverage", parent_map: dict, class_names: set) -> str:
    """The R-AUD-3 appendix: every emitted method that mutates Godot and answers a value."""
    def ancestors(name):
        seen = []
        while name:
            seen.append(name)
            name = parent_map.get(name)
        return seen

    shapes = {"value": 0, "error": 0, "object": 0, "receiver": 0}
    by_class = {}
    for godot_class, godot_name, verse_class, verse_name, raw in coverage.nonatomic:
        if raw == "enum::Error":
            shape = "error"
        elif raw in ancestors(godot_class):
            shape = "receiver"
        elif raw in class_names or raw.startswith(TYPED_ARRAY_PREFIX):
            shape = "object"
        else:
            shape = "value"
        shapes[shape] += 1
        by_class.setdefault(godot_class, []).append((verse_class, verse_name, godot_name, shape))

    text = NONATOMIC_HEADER.format(version=api["header"]["version_full_name"],
                                   count=len(coverage.nonatomic), **shapes)
    for godot_class in sorted(by_class):
        rows = sorted(by_class[godot_class], key=lambda r: r[1])
        text += f"### {godot_class} ({rows[0][0]})\n\n"
        text += "| Verse | Godot | shape |\n| --- | --- | --- |\n"
        for _verse_class, verse_name, godot_name, shape in rows:
            text += f"| `{verse_name}` | `{godot_name}` | {shape} |\n"
        text += "\n"
    return text


def render_skipped_header(api: dict, skipped: list) -> str:
    """One row per skipped member, sorted so a diff of the header reads as a diff of the API."""
    rows = sorted({(s.verse_class, s.verse_name, s.godot_class, s.godot_name, s.reason, s.detail)
                   for s in skipped})
    entries = "\n".join(
        '\t{ "%s", "%s", "%s", "%s", "%s", "%s" },' % row for row in rows)
    return SKIPPED_HEADER_TEMPLATE.format(
        version=api["header"]["version_full_name"], entries=entries)


# Godot builtins rather than mirrored classes, so they are hand-written in GodotApi.native.verse
# and never reach emit_order -- but they are Godot types with Godot documentation, and without
# them the editor calls `vector2` a local constant.
VALUE_TYPE_CLASSES = {"Vector2": "vector2", "Vector3": "vector3", "Color": "color"}

# object's three lifecycle methods are hand-written in Godot.native.verse rather than mirrored --
# the generator skips virtuals -- but they exist to be the Verse spelling of Godot's, and a script
# overriding one wants Godot's documentation for it. Listed as (verse class, verse method, godot
# class, godot method), the shape the method map already carries.
# The one virtual that is hand-written rather than generated, so a hover on it still finds Godot's
# documentation. Everything else Godot calls on a script is in extension_api.json and is generated
# onto the class that declares it; `_notification` is in no part of it (docs/phase-4-design.md 7.3).
LIFECYCLE_METHODS = [
    (NATIVE_ROOT, "_Notification", "Object", "_notification", True),
]

# The fields of those hand-written value types, in the same shape. Listed rather than read out of
# the API's builtin_classes because the structs are hand-written too: a field the JSON has and
# GodotApi.native.verse does not would be a mapping to a name no Verse code can spell. Without
# these a click on the `X` of `Position.X` reaches a definition in the engine tree, which has no
# res:// file to jump to and no Godot doc page to fall back on, so it does nothing at all.
VALUE_TYPE_MEMBERS = [
    ("vector2", "X", "Vector2", "x", False),
    ("vector2", "Y", "Vector2", "y", False),
    ("vector3", "X", "Vector3", "x", False),
    ("vector3", "Y", "Vector3", "y", False),
    ("vector3", "Z", "Vector3", "z", False),
    ("color", "R", "Color", "r", False),
    ("color", "G", "Color", "g", False),
    ("color", "B", "Color", "b", False),
    ("color", "A", "Color", "a", False),
]


# Where the math types' methods and operators actually live. Hand-written Verse, not generated --
# OQ-11's answer is that a vector2 has no handle and needs no ABI -- which is exactly why the
# *gap* has to be computed rather than maintained: a list of "what is missing" written by hand goes
# stale the moment someone adds a method, and silently.
MATH_SOURCE_PATH = "host/Verse/GodotMath.native.verse"

# `(V:vector2).LengthSquared<public>(` -- an extension method, which is a module-level definition of
# `operator'.LengthSquared'` and is why these names are unusable as parameter names anywhere.
MATH_METHOD_RE = re.compile(r"^\(\s*\w+\s*:\s*(\w+)\s*\)\.(\w+)")

# `operator'*'<public>(L:vector2, S:float)` and `prefix'-'<public>(V:vector2)`. The left operand's
# type is what files the definition under a math type; the right operand's is what tells two
# overloads of one symbol apart.
MATH_OPERATOR_RE = re.compile(
    r"^(operator|prefix)'([^']+)'\s*(?:<[^>]*>\s*)*\(\s*\w+\s*:\s*(\w+)\s*(?:,\s*\w+\s*:\s*(\w+)\s*)?\)")


def read_math_written(path: Path) -> tuple[dict, set]:
    """What GodotMath.native.verse defines, as (methods by verse type, operator keys).

    Read out of the file rather than listed here on purpose (G12): adding a method to GodotMath has
    to make its skip disappear on the next generation, or the record drifts from the code and the
    editor starts explaining the absence of something that is present.

    Operator keys are `(symbol, left verse type, right verse type or None)`, because Godot lists one
    operator entry per right-hand type and Verse spells each as its own overload.
    """
    methods = defaultdict(set)
    operators = set()
    if not path.exists():
        return methods, operators
    for line in path.read_text(encoding="utf-8").splitlines():
        method = MATH_METHOD_RE.match(line)
        if method:
            methods[method.group(1)].add(method.group(2))
            continue
        operator = MATH_OPERATOR_RE.match(line)
        if operator:
            kind, symbol, left, right = operator.groups()
            # `prefix'-'(V:vector2)` is Godot's `unary-`; a binary operator keys on both sides.
            operators.add((symbol, left, None) if kind == "prefix" else (symbol, left, right))
            # Godot files `float * Vector2` under Vector2 as well, and Verse needs the mirrored
            # overload to be written for it to be reachable -- so record it under whichever side is
            # a math type.
            if kind == "operator" and right is not None:
                operators.add((symbol, right, left))
    return methods, operators


# Godot's spelling of a unary operator, against Verse's `prefix'-'`.
GODOT_UNARY_PREFIX = "unary"


def record_math_skips(api: dict, coverage: Coverage, math_source: Path):
    """Records every builtin method and operator GodotMath does not define, as `math_not_written`.

    R-SCN-2 promises that every Godot member is reachable *or the reason it is not is reported*, and
    this is the largest surface where the second half was missing: 367 methods and 261 operators
    across sixteen types, absent with nothing said. The skip is what turns "that name does not
    exist" into a sentence in the editor.
    """
    written_methods, written_operators = read_math_written(math_source)

    for builtin in api.get("builtin_classes", []):
        godot_class = builtin["name"]
        if godot_class not in MATH_TYPES:
            continue
        verse_class = verse_class_name(godot_class)

        for method in builtin.get("methods") or []:
            verse_name = verse_method_name(method["name"])
            if verse_name in written_methods.get(verse_class, ()):
                continue
            coverage.skip("math_not_written", SkippedMember(
                verse_class, verse_name, godot_class, method["name"], "math_not_written", ""))

        for operator in builtin.get("operators") or []:
            symbol = operator["name"]
            right = operator.get("right_type")
            if symbol.startswith(GODOT_UNARY_PREFIX):
                key = (symbol[len(GODOT_UNARY_PREFIX):], verse_class, None)
            else:
                key = (symbol, verse_class, verse_builtin_type_name(right))
            if key in written_operators:
                continue
            # The name an author would have written is the operator itself; there is no member name
            # to look up, so the detail carries the operand that identifies which overload.
            detail = "" if right is None else right
            coverage.skip("math_not_written", SkippedMember(
                verse_class, f"operator'{symbol}'", godot_class, symbol, "math_operator_not_written",
                detail))


def verse_builtin_type_name(godot_type: str | None) -> str | None:
    """`Vector2` -> `vector2`, `float` -> `float`. None stays None, for a unary operator."""
    if godot_type is None:
        return None
    if godot_type in ("int", "float", "bool", "String", "Variant"):
        return {"bool": "logic", "String": "string", "Variant": "variant"}.get(godot_type, godot_type)
    return verse_class_name(godot_type)


def render_math_layout_header(api: dict) -> str:
    field_arrays = []
    entries = []
    for godot_name in MATH_TYPES:
        verse_name = verse_class_name(godot_name)
        ident = verse_name.replace("_", "")
        rows = []
        for member, member_type in MATH_LAYOUT[godot_name]:
            field = verse_method_name(member)
            if member_type in ("float", "int"):
                rows.append(f'\t{{"{field}", 0, {"true" if member_type == "int" else "false"}}},')
            else:
                rows.append(f'\t{{"{field}", {math_variant_tag(member_type)}, false}},')
        field_arrays.append(
            f"inline constexpr field {ident}_fields[] = {{\n" + "\n".join(rows) + "\n};")
        packed = MATH_PACKED_ARRAYS.get(godot_name, "0")
        entries.append(
            f'\t{{"{verse_name}", {math_variant_tag(godot_name)}, {packed}, '
            f"{ident}_fields, {len(MATH_LAYOUT[godot_name])}}},")
    reference_entries = "\n".join(
        f'\t{{"{verse_name}", {tag}}},' for verse_name, tag in REFERENCE_TYPES)
    return MATH_LAYOUT_HEADER_TEMPLATE.format(
        version=api["header"]["version_full_name"],
        field_arrays="\n\n".join(field_arrays),
        entries="\n".join(entries),
        reference_entries=reference_entries,
    )


CLASS_NAMES_HEADER_TEMPLATE = """// Copyright Epic Games, Inc. All Rights Reserved.
#pragma once

// Generated by tools/gen_verse_api.py from godot-cpp/gdextension/extension_api-4-7.json
// ({version}). Do not edit by hand.
//
// Every Godot class, and the mirrored Verse class an object of it crosses as (R-SCN-6). The host
// asks Godot for a handle's class name and needs the Verse class to build the object with; that
// object has to be of the *most derived* mirrored class, or a downcast can never succeed.
//
// Every Godot class is here, not only the mirrored ones: with --classes-file a subset is emitted,
// and a class outside it must still cross as something. Each row names its nearest emitted
// ancestor, so the lookup is one step and always answers.
//
// Sorted by Godot name, for binary search.

namespace verse_classes {{

struct class_name
{{
	const char *godot_name;
	const char *verse_name;
}};

inline constexpr class_name class_names[] = {{
{entries}
}};

// The other direction, for R-NODE-3: a Verse class the host is about to instantiate, and the Godot
// class whose object is its peer. `helper{{}}` on a `class(ref_counted)` walks up to `ref_counted`
// and this is what turns that into the `RefCounted` that ClassDB::instantiate is given.
//
// **The emitted classes only**, which is what makes it the inverse of the table above rather than
// a second copy of it. That one maps every Godot class onto its nearest *emitted* ancestor, so
// several rows share a Verse name in a --classes-file build and reversing it would be ambiguous;
// here each Verse class appears once, naming the Godot class it was generated from.
//
// Sorted by Verse name, for binary search.

struct mirrored_class
{{
	const char *verse_name;
	const char *godot_name;
}};

inline constexpr mirrored_class mirrored_classes[] = {{
{mirrored_entries}
}};

}} // namespace verse_classes
"""


def render_class_names_header(api: dict, emit_order: list) -> str:
    emitted = set(emit_order)
    parents = {c["name"]: c.get("inherits") for c in api["classes"]}
    rows = []
    for c in api["classes"]:
        cur = c["name"]
        while cur and cur not in emitted:
            cur = parents.get(cur)
        rows.append((c["name"], verse_class_name(cur) if cur else "object"))
    rows.sort()
    mirrored = sorted((verse_class_name(name), name) for name in emitted)
    return CLASS_NAMES_HEADER_TEMPLATE.format(
        version=api["header"]["version_full_name"],
        entries="\n".join(f'\t{{ "{godot}", "{verse}" }},' for godot, verse in rows),
        mirrored_entries="\n".join(f'\t{{ "{verse}", "{godot}" }},' for verse, godot in mirrored),
    )


def render_classes_header(api: dict, emit_order: list, method_map: list) -> str:
    version = api["header"]["version_full_name"]
    pairs = sorted(
        [(name, verse_class_name(name)) for name in emit_order] + list(VALUE_TYPE_CLASSES.items())
    )
    entries = "\n".join(f'\t{{ "{godot_name}", "{verse_name}" }},' for godot_name, verse_name in pairs)
    rows = [(m[1], m[3], m[0], m[2], m[4]) for m in method_map] + LIFECYCLE_METHODS + VALUE_TYPE_MEMBERS
    method_entries = "\n".join(
        f'\t{{ "{verse_class}", "{verse_method}", "{godot_class}", "{godot_method}", '
        f'{"true" if is_virtual else "false"} }},'
        for verse_class, verse_method, godot_class, godot_method, is_virtual in sorted(rows)
    )
    return CLASSES_HEADER_TEMPLATE.format(
        version=version, entries=entries, method_entries=method_entries
    )


def format_report(coverage: Coverage, class_count_requested: int) -> str:
    lines = []
    lines.append("Verse API generation coverage report")
    lines.append("=" * 40)
    lines.append("")
    lines.append(f"Classes requested (incl. ancestors pulled in): {class_count_requested}")
    lines.append(f"Classes emitted: {coverage.classes_emitted}")
    lines.append(f"Methods emitted: {coverage.methods_emitted}")
    lines.append(f"Properties emitted: {coverage.properties_emitted}")
    lines.append(f"Signal accessors emitted: {coverage.signals_emitted}")
    lines.append(f"Constants emitted: {coverage.constants_emitted}")
    lines.append(f"Static methods emitted: {coverage.statics_emitted}")
    lines.append(f"Utility functions dispatched: {coverage.utilities_emitted}")
    lines.append("")
    lines.append("Methods skipped, by reason:")
    total_skipped = sum(coverage.skip_reasons.values())
    for reason, count in coverage.skip_reasons.most_common():
        lines.append(f"  {reason:<18} {count}")
    lines.append(f"  {'TOTAL':<18} {total_skipped}")
    lines.append("")
    lines.append("Top 20 unsupported types (by methods they cost):")
    for t, count in coverage.unsupported_types.most_common(20):
        lines.append(f"  {t:<30} {count}")
    return "\n".join(lines) + "\n"


RESERVED_WORDS = set()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--api", default=EXTENSION_API, help="Path to extension_api.json")
    # The whole API by default: a class the mirror lacks can only be added by rebuilding
    # verse_host.dll, which needs a UE source checkout, so a subset is a wall rather than a
    # setting for anyone but the host's own author (docs/phase-2-design.md 3).
    parser.add_argument("--classes-file", default=None,
                        help=f"Emit only these classes and their ancestors, e.g. {DEFAULT_CLASSES_FILE}")
    parser.add_argument("--out", default=GENERATED_PATH)
    parser.add_argument("--math-layout-header", default=MATH_LAYOUT_HEADER_PATH)
    parser.add_argument("--class-names-header", default=CLASS_NAMES_HEADER_PATH)
    parser.add_argument("--classes-header", default=CLASSES_HEADER_PATH)
    parser.add_argument("--skipped-header", default=SKIPPED_HEADER_PATH)
    parser.add_argument("--nonatomic", default=NONATOMIC_PATH,
                        help="Where to write the R-AUD-3 appendix of non-atomic methods")
    parser.add_argument("--math-source", default=MATH_SOURCE_PATH,
                        help="The hand-written math file whose definitions decide what is *not* skipped")
    parser.add_argument("--report", default=None, help="Write the coverage report here instead of stdout")
    parser.add_argument("--keywords", default=KEYWORDS_HEADER)
    args = parser.parse_args()

    root = repo_root()
    api_path = resolve(root, args.api)
    keywords_path = resolve(root, args.keywords)
    out_path = resolve(root, args.out)
    classes_header_path = resolve(root, args.classes_header)

    global RESERVED_WORDS
    RESERVED_WORDS = load_reserved_words(keywords_path)

    api = load_api(api_path)

    # Before generation rather than during rendering: a typed array of objects is spelled with a
    # Variant::Type *number*, so resolving one needs the numbers, and TypeResolver runs first.
    check_variant_lanes(api)
    enums = collect_enums(api)
    check_enum_names(enums, api)

    if args.classes_file is None:
        requested = [c["name"] for c in api["classes"]]
    else:
        classes_file = resolve(root, args.classes_file)
        requested = read_classes_file(classes_file)

    coverage = Coverage()
    (class_blocks, emit_order, method_map, member_names, typed_arrays,
     typed_dictionaries) = generate(api, requested, coverage, enums)
    resolver_for_statics = TypeResolver(set(emit_order), build_parent_map(api["classes"]),
                                       {c["name"] for c in api["classes"]}, enums)
    statics_modules = emit_statics_modules(api, emit_order, resolver_for_statics, coverage, enums)
    utilities = emit_utility_functions(api, resolver_for_statics, coverage)
    record_math_skips(api, coverage, resolve(root, args.math_source))
    # The converters are module-level, so the two resolvers' demands are one set. A typed array
    # only a static or a utility mentions -- ImporterMesh.merge_importer_meshes is the first, and
    # is new in 4.7 -- otherwise names a VhFrom...Array nothing defines, which is an unknown
    # identifier at the first compile of the mirror and nothing earlier.
    typed_arrays.update(resolver_for_statics.typed_arrays)
    typed_dictionaries.update(resolver_for_statics.typed_dictionaries)
    text = render(api, class_blocks, emit_singleton_accessors(api, emit_order, member_names),
                  typed_arrays, typed_dictionaries, enums, statics_modules, utilities)
    classes_header_text = render_classes_header(api, emit_order, method_map)

    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(text, encoding="utf-8", newline="\n")

    classes_header_path.parent.mkdir(parents=True, exist_ok=True)
    classes_header_path.write_text(classes_header_text, encoding="utf-8", newline="\n")

    math_layout_path = resolve(root, args.math_layout_header)
    math_layout_path.parent.mkdir(parents=True, exist_ok=True)
    math_layout_path.write_text(render_math_layout_header(api), encoding="utf-8", newline="\n")

    class_names_path = resolve(root, args.class_names_header)
    class_names_path.parent.mkdir(parents=True, exist_ok=True)
    class_names_path.write_text(render_class_names_header(api, emit_order), encoding="utf-8", newline="\n")

    skipped_path = resolve(root, args.skipped_header)
    skipped_path.parent.mkdir(parents=True, exist_ok=True)
    skipped_path.write_text(render_skipped_header(api, coverage.skipped_members),
                            encoding="utf-8", newline="\n")

    nonatomic_path = resolve(root, args.nonatomic)
    nonatomic_path.parent.mkdir(parents=True, exist_ok=True)
    nonatomic_path.write_text(
        render_nonatomic(api, coverage, build_parent_map(api["classes"]),
                         {c["name"] for c in api["classes"]}),
        encoding="utf-8", newline="\n")

    report = format_report(coverage, len(class_blocks))
    if args.report:
        report_path = resolve(root, args.report)
        report_path.parent.mkdir(parents=True, exist_ok=True)
        report_path.write_text(report, encoding="utf-8", newline="\n")
        print(f"[gen_verse_api] wrote {out_path}, {classes_header_path} and {report_path}")
    else:
        print(f"[gen_verse_api] wrote {out_path} and {classes_header_path}")
        print()
        print(report)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
