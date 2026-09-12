#!/usr/bin/env python3
"""Generates host/Verse/GodotClasses.native.verse from godot-cpp/gdextension/extension_api.json.

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
from collections import Counter, namedtuple
from pathlib import Path

GENERATED_PATH = "host/Verse/GodotClasses.native.verse"
CLASSES_HEADER_PATH = "src/verse_api_classes.h"
MATH_LAYOUT_HEADER_PATH = "host/Private/GodotMathLayout.gen.h"
DEFAULT_CLASSES_FILE = "tools/verse_api_classes.txt"
KEYWORDS_HEADER = "src/verse_keywords.h"
EXTENSION_API = "godot-cpp/gdextension/extension_api.json"

BASE_MEMBER_NAMES = {"Handle", "Ready", "Process", "PhysicsProcess"}

# /Verse.org/Verse is in scope in every generated body, and Verse reports an ambiguity rather
# than shadowing, so a parameter named Min breaks any method that mentions it. The standard
# library's names are compiler intrinsics rather than a .verse digest, so there is nothing to
# enumerate; this list is what the full-API generation actually collided with, plus the obvious
# siblings. Over-listing costs nothing but an ArgN parameter name.
#
# This package's own module-level names are in scope for the same reason and belong here too --
# Print and IsInstanceValid, the two functions GodotApi.native.verse exports.
VERSE_STDLIB_NAMES = {
    "Abs", "Ceil", "Floor", "Round", "Sqrt", "Min", "Max", "Sign", "Clamp", "Lerp", "Mod",
    "Sin", "Cos", "Tan", "ArcSin", "ArcCos", "ArcTan", "Pow", "Exp", "Ln",
    "Print", "Err", "Sleep", "Length", "Slice", "Reverse", "Shuffle", "Concatenate", "Fits",
    "ToString", "ToDiagnostic", "ToInt", "ToFloat", "ToChar", "ToRational",
    "IsInstanceValid",
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


def verse_method_name(godot_name: str) -> str:
    parts = [p for p in godot_name.split("_") if p]
    return "".join(p[0].upper() + p[1:] for p in parts)


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
    def __init__(self, emitted: set, parent_map: dict, class_names: set):
        self.emitted = emitted
        self.parent_map = parent_map
        self.class_names = class_names
        self._ancestor_cache = {}

    def nearest_emitted_ancestor(self, godot_class: str):
        if godot_class in self._ancestor_cache:
            return self._ancestor_cache[godot_class]
        cur = self.parent_map.get(godot_class)
        result = None
        while cur and cur != "Object":
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
        if godot_type.startswith("enum::") or godot_type.startswith("bitfield::"):
            return SCALAR_TYPES["int"]
        if godot_type in self.class_names:
            target = godot_type if godot_type in self.emitted else self.nearest_emitted_ancestor(godot_type)
            if target is None:
                return None
            return TypeInfo(verse_class_name(target), "VhFromObject", False, "VhToHandle", True)
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


# What a script can read out of, or write into, a Godot container.
#
# One entry per element type, because a script cannot spell a `variant`: the packers are
# module-scoped so that a user cannot hold a raw reference (R-TYPE-7), which means every way into
# and out of a container has to be typed. Keyed by the suffix the accessor takes.
CONTAINER_ELEMENTS = [
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
        lines = [f"{verse_name}<public> := class(godot_ref):", ""]
        lines.append("    # How many elements it holds.")
        lines.append("    Length<public>()<transacts>:int = VhRefSize(Ref)")
        lines.append("")
        if verse_name == "godot_array":
            lines.append("    # The whole thing as a Verse array of its elements' own type, which is a copy:")
            lines.append("    # Verse's arrays are values, so mutating what this returns reaches nothing.")
            for suffix, verse_type, _, unpack in CONTAINER_ELEMENTS:
                if verse_type in ("godot_array", "dictionary"):
                    continue
                lines.append(
                    f"    To{suffix}s<public>()<transacts>:[]{verse_type} ="
                    f" for (V : VhRefValues(Ref)) {{ {unpack}(V) }}")
            lines.append("")

        for suffix, verse_type, pack, unpack in CONTAINER_ELEMENTS:
            for key_name, key_type, key_pack in keys:
                # Failable: an absent key and an index out of range are ordinary misses, and a
                # value of another type is a miss too rather than a raise -- asking a container for
                # an int and getting a string back is the caller's question answered "no".
                lines.append(
                    f"    Get{suffix}<public>({key_name}:{key_type})<decides><transacts>:{verse_type} ="
                    f" {unpack}(VhRefGet[Ref, {key_pack}({key_name})])")
            for key_name, key_type, key_pack in keys:
                lines.append(
                    f"    Set{suffix}<public>({key_name}:{key_type}, Value:{verse_type})<transacts>:void ="
                    f" VhRefSet(Ref, {key_pack}({key_name}), {pack}(Value))")
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
        lines = [f"{math_struct_name(godot_name)}<public> := struct:"]
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
            f"VhFrom{godot_name}(Value:{name})<transacts>:variant = variant{{{', '.join(assigns)}}}")

        reads = {}
        for i, path in enumerate(ints):
            reads[path] = f"Value.I{i}"
        for i, path in enumerate(floats):
            reads[path] = f"Value.F{i}"
        blocks.append(
            f"VhTo{godot_name}(Value:variant)<transacts>:{name} =\n"
            f"    VhExpect(Value, {tag}, \"{name}\")\n"
            f"    {build_math_literal(godot_name, reads, '')}")
    return blocks


ClassifiedMethod = namedtuple(
    "ClassifiedMethod", ["godot_name", "verse_name", "params", "return_type", "is_void"]
)
ClassifiedProperty = namedtuple(
    "ClassifiedProperty", ["godot_name", "verse_name", "type_info", "getter", "setter", "index"]
)
Param = namedtuple("Param", ["verse_name", "type_info", "default"])


def build_parent_map(classes: list) -> dict:
    return {c["name"]: c.get("inherits") for c in classes}


def compute_emit_set(requested: list, parent_map: dict) -> list:
    """Requested classes plus all their ancestors (Object excluded), in first-seen order."""
    emit_order = []
    emit_set = set()
    for name in requested:
        if name == "Object" or name not in parent_map:
            continue
        chain = []
        cur = name
        while cur is not None and cur != "Object" and cur not in emit_set:
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
    depth = 0 if parent is None or parent == "Object" else 1 + class_depth(parent, parent_map, memo)
    memo[name] = depth
    return depth


def read_classes_file(path: Path) -> list:
    names = []
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.split("#", 1)[0].strip()
        if line:
            names.append(line)
    return names


class Coverage:
    def __init__(self):
        self.classes_emitted = 0
        self.methods_emitted = 0
        self.properties_emitted = 0
        self.skip_reasons = Counter()
        self.unsupported_types = Counter()

    def skip(self, reason: str):
        self.skip_reasons[reason] += 1

    def unsupported(self, types_seen):
        self.skip_reasons["unsupported_type"] += 1
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


def classify_method(m: dict, resolver: TypeResolver, coverage: Coverage, members: set):
    """Returns a ClassifiedMethod, or None (and records why in coverage) if the method is skipped."""
    if m.get("is_virtual"):
        coverage.skip("virtual")
        return None
    if m.get("is_static"):
        coverage.skip("static")
        return None
    if m.get("is_vararg"):
        coverage.skip("vararg")
        return None

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
        coverage.unsupported(unsupported_seen)
        return None

    # An optional parameter may not be followed by a required one, and a Godot default that had
    # no Verse spelling leaves its parameter required, so trailing defaults only.
    for i in range(len(params)):
        if params[i].default is not None and any(p.default is None for p in params[i + 1:]):
            params[i] = params[i]._replace(default=None)

    return ClassifiedMethod(
        godot_name=m["name"],
        verse_name=verse_method_name(m["name"]),
        params=params,
        return_type=return_info,
        is_void=is_void,
    )


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
    args = emit_call_args(cm.params)
    call = f'VhCallValue(Handle, "{cm.godot_name}", array{{{args}}})' if not cm.is_void else None

    if cm.is_void:
        body = f'VhCallVoid(Handle, "{cm.godot_name}", array{{{args}}})'
        return f"    {cm.verse_name}<public>({param_decl})<transacts>:void = {body}"

    ti = cm.return_type
    if ti.pack_fn == "VhFromObject":
        body = f"{ti.verse_type}{{Handle := VhToHandle[{call}]}}"
    elif ti.unpack_decides:
        body = f"{ti.unpack_fn}[{call}]"
    else:
        body = f"{ti.unpack_fn}({call})"

    effects = "<decides><transacts>" if ti.unpack_decides else "<transacts>"
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


def classify_property(p: dict, resolver: TypeResolver, coverage: Coverage):
    """Returns a ClassifiedProperty, or None (and records why) if the property is skipped."""
    if not p.get("getter") or not p.get("setter"):
        coverage.skip("property_no_accessor_pair")
        return None

    info = resolver.classify(p["type"])
    if info is None:
        coverage.unsupported([p["type"]])
        return None
    # A `var` is *data*, and data cannot overload: Verse rejects a member named Max outright
    # because /Verse.org/Verse's Max is in scope at its declaration, where a zero-argument *method*
    # of the same name would have been distinguished by its signature. So the property is dropped
    # and Godot's own getter and setter survive as methods -- GetMax() still reads it; what is lost
    # is only `set Node.Max = ...`.
    if verse_method_name(p["name"]) in VERSE_STDLIB_NAMES:
        coverage.skip("property_ambiguous_name")
        return None
    if info.verse_type in CONTAINER_PROPERTY_TYPES or info.verse_type.startswith("[]"):
        coverage.skip("property_container_type")
        return None
    # An object-typed property would need a getter that cannot fail, and a null Godot object is
    # exactly the absence VhToHandle reports as failure.
    if info.pack_fn == "VhFromObject":
        coverage.skip("property_object_type")
        return None
    # A nested math struct: see FLAT_MATH_STRUCTS. Skipping it here leaves Godot's own getter and
    # setter to be emitted as ordinary methods, so `GetGlobalTransform()` still reaches it -- what
    # is lost is only the `set Node.GlobalTransform = ...` spelling.
    if info.verse_type in MATH_STRUCT_NAMES and info.verse_type not in FLAT_MATH_STRUCTS:
        coverage.skip("property_nested_struct")
        return None

    return ClassifiedProperty(
        godot_name=p["name"],
        verse_name=verse_method_name(p["name"]),
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


def generate(api: dict, requested: list, coverage: Coverage):
    classes = api["classes"]
    parent_map = build_parent_map(classes)
    class_names = set(parent_map.keys())
    classes_by_name = {c["name"]: c for c in classes}

    emit_order = compute_emit_set(requested, parent_map)
    depth_memo = {}
    emit_order.sort(key=lambda n: (class_depth(n, parent_map, depth_memo), verse_class_name(n)))

    emit_set = set(emit_order)
    resolver = TypeResolver(emit_set, parent_map, class_names)

    inherited_names = {}  # godot class name -> set of Verse names visible to its subclasses
    class_blocks = []
    method_map = []  # (godot class, verse class, godot method, verse method) per emitted method
    # Every emitted method a module-level function could be ambiguous with: see
    # singleton_accessor_name, where arity is the whole question.
    nullary_methods = set()

    for name in emit_order:
        parent = parent_map[name]
        base_names = set(BASE_MEMBER_NAMES) if parent == "Object" else set(inherited_names[parent])
        base_verse = "object" if parent == "Object" else verse_class_name(parent)

        methods = classes_by_name[name].get("methods", [])

        properties = []
        for p in classes_by_name[name].get("properties", []):
            cp = classify_property(p, resolver, coverage)
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
        for cp in properties:
            member_names |= {cp.verse_name, f"{cp.verse_name}Getter", f"{cp.verse_name}Setter"}
        locals_for_accessors = accessor_locals(member_names)

        candidates = []
        for m in methods:
            if m["name"] in superseded:
                coverage.skip("superseded_by_property")
                continue
            cm = classify_method(m, resolver, coverage, member_names)
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
            emitted_lines.extend(emit_property(cp, locals_for_accessors))
            method_map.append((name, verse_class_name(name), cp.godot_name, cp.verse_name))
            coverage.properties_emitted += 1

        for cm in candidates:
            if cm.verse_name in used:
                coverage.skip("shadow")
                continue
            used.add(cm.verse_name)
            if not cm.params:
                nullary_methods.add(cm.verse_name)
            emitted_lines.append(emit_method(cm))
            method_map.append((name, verse_class_name(name), cm.godot_name, cm.verse_name))
            coverage.methods_emitted += 1

        inherited_names[name] = used
        coverage.classes_emitted += 1

        header = f"{verse_class_name(name)}<public> := class({base_verse}):"
        if emitted_lines:
            class_blocks.append(header + "\n\n" + "\n".join(emitted_lines))
        else:
            class_blocks.append(header)

    return class_blocks, emit_order, method_map, nullary_methods


HEADER_TEMPLATE = """using {{/Verse.org/Native}}

# Generated by tools/gen_verse_api.py from godot-cpp/gdextension/extension_api.json
# ({version}). Do not edit by hand.
#
# Godot's Object class is skipped entirely: its own API is almost all Callable- and
# Variant-typed reflection that this bridge cannot marshal (see the type table in
# tools/gen_verse_api.py), so a class whose Godot parent is Object derives directly from
# the hand-written native `object` (see Godot.native.verse) instead of a generated one.
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


SINGLETONS_TEMPLATE = """
# Godot hands a singleton out by name rather than through the scene, so a mirrored `input` or
# `engine` would otherwise be a class no script can obtain an instance of. <decides> because
# Engine::get_singleton answers nothing for a name this build did not register -- an editor-only
# singleton asked for in an exported game, say.

{accessors}
"""


def singleton_accessor_name(godot_name: str, nullary_methods: set) -> str:
    """`GetInput`, unless a mirrored method already answers to that with no arguments.

    A module-level function and a class method of the same name *and* signature are ambiguous
    where the class' own body can see both -- EditorPlugin.get_editor_interface() against the
    accessor for the EditorInterface singleton, which hand back the same object. The accessor is
    this generator's invention and the method is Godot's, so the accessor is the one that moves.
    Arity is what decides it: XRController3D.get_input(int) coexists with GetInput() untouched.
    """
    base = f"Get{godot_name}"
    return f"{base}Singleton" if base in nullary_methods else base


def emit_singleton_accessors(api: dict, emit_order: list, nullary_methods: set) -> list:
    """One module-level accessor per emitted class that Godot registers as a singleton."""
    singletons = {s["name"] for s in api.get("singletons", [])}
    return [
        f'{singleton_accessor_name(name, nullary_methods)}<public>()<decides><transacts>'
        f':{verse_class_name(name)}'
        f' = {verse_class_name(name)}{{Handle := VhSingleton["{name}"]}}'
        for name in sorted(n for n in emit_order if n in singletons)
    ]


def render(api: dict, class_blocks: list, singleton_accessors: list) -> str:
    version = api["header"]["version_full_name"]
    text = HEADER_TEMPLATE.format(version=version)
    text += MATH_TEMPLATE.format(
        structs="\n\n".join(emit_math_structs()),
        packers="\n".join(emit_math_packers()),
    )
    text += CONTAINERS_TEMPLATE.format(classes="\n\n".join(emit_container_classes()))
    text += "\n" + "\n\n".join(class_blocks) + "\n"
    if singleton_accessors:
        text += SINGLETONS_TEMPLATE.format(accessors="\n".join(singleton_accessors))
    return text


MATH_LAYOUT_HEADER_TEMPLATE = """// Copyright Epic Games, Inc. All Rights Reserved.
#pragma once

// Generated by tools/gen_verse_api.py from godot-cpp/gdextension/extension_api.json
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

// Generated by tools/gen_verse_api.py from godot-cpp/gdextension/extension_api.json
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
struct method_mapping {{
	const char *verse_class;
	const char *verse_method;
	const char *godot_class;
	const char *godot_method;
}};

inline constexpr method_mapping methods[] = {{
{method_entries}
}};

}} // namespace verse_api
"""


# Godot builtins rather than mirrored classes, so they are hand-written in GodotApi.native.verse
# and never reach emit_order -- but they are Godot types with Godot documentation, and without
# them the editor calls `vector2` a local constant.
VALUE_TYPE_CLASSES = {"Vector2": "vector2", "Vector3": "vector3", "Color": "color"}

# object's three lifecycle methods are hand-written in Godot.native.verse rather than mirrored --
# the generator skips virtuals -- but they exist to be the Verse spelling of Godot's, and a script
# overriding one wants Godot's documentation for it. Listed as (verse class, verse method, godot
# class, godot method), the shape the method map already carries.
LIFECYCLE_METHODS = [
    ("object", "Ready", "Node", "_ready"),
    ("object", "Process", "Node", "_process"),
    ("object", "PhysicsProcess", "Node", "_physics_process"),
]

# The fields of those hand-written value types, in the same shape. Listed rather than read out of
# the API's builtin_classes because the structs are hand-written too: a field the JSON has and
# GodotApi.native.verse does not would be a mapping to a name no Verse code can spell. Without
# these a click on the `X` of `Position.X` reaches a definition in the engine tree, which has no
# res:// file to jump to and no Godot doc page to fall back on, so it does nothing at all.
VALUE_TYPE_MEMBERS = [
    ("vector2", "X", "Vector2", "x"),
    ("vector2", "Y", "Vector2", "y"),
    ("vector3", "X", "Vector3", "x"),
    ("vector3", "Y", "Vector3", "y"),
    ("vector3", "Z", "Vector3", "z"),
    ("color", "R", "Color", "r"),
    ("color", "G", "Color", "g"),
    ("color", "B", "Color", "b"),
    ("color", "A", "Color", "a"),
]


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


def render_classes_header(api: dict, emit_order: list, method_map: list) -> str:
    version = api["header"]["version_full_name"]
    pairs = sorted(
        [(name, verse_class_name(name)) for name in emit_order] + list(VALUE_TYPE_CLASSES.items())
    )
    entries = "\n".join(f'\t{{ "{godot_name}", "{verse_name}" }},' for godot_name, verse_name in pairs)
    rows = [(m[1], m[3], m[0], m[2]) for m in method_map] + LIFECYCLE_METHODS + VALUE_TYPE_MEMBERS
    method_entries = "\n".join(
        f'\t{{ "{verse_class}", "{verse_method}", "{godot_class}", "{godot_method}" }},'
        for verse_class, verse_method, godot_class, godot_method in sorted(rows)
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
    lines.append("")
    lines.append("Note: Godot's Object class is always skipped -- its API is mostly")
    lines.append("Callable/Variant-typed reflection this bridge cannot marshal. Any class")
    lines.append("whose Godot parent is Object derives from the native `object` instead.")
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
    parser.add_argument("--classes-header", default=CLASSES_HEADER_PATH)
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

    if args.classes_file is None:
        requested = [c["name"] for c in api["classes"] if c["name"] != "Object"]
    else:
        classes_file = resolve(root, args.classes_file)
        requested = read_classes_file(classes_file)

    coverage = Coverage()
    class_blocks, emit_order, method_map, nullary_methods = generate(api, requested, coverage)
    text = render(api, class_blocks, emit_singleton_accessors(api, emit_order, nullary_methods))
    classes_header_text = render_classes_header(api, emit_order, method_map)

    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(text, encoding="utf-8", newline="\n")

    classes_header_path.parent.mkdir(parents=True, exist_ok=True)
    classes_header_path.write_text(classes_header_text, encoding="utf-8", newline="\n")

    math_layout_path = resolve(root, args.math_layout_header)
    math_layout_path.parent.mkdir(parents=True, exist_ok=True)
    math_layout_path.write_text(render_math_layout_header(api), encoding="utf-8", newline="\n")

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
