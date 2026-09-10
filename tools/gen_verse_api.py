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
DEFAULT_CLASSES_FILE = "tools/verse_api_classes.txt"
KEYWORDS_HEADER = "src/verse_keywords.h"
EXTENSION_API = "godot-cpp/gdextension/extension_api.json"

BASE_MEMBER_NAMES = {"Handle", "IsInstanceValid", "Ready", "Process", "PhysicsProcess"}

# /Verse.org/Verse is in scope in every generated body, and Verse reports an ambiguity rather
# than shadowing, so a parameter named Min breaks any method that mentions it. The standard
# library's names are compiler intrinsics rather than a .verse digest, so there is nothing to
# enumerate; this list is what the full-API generation actually collided with, plus the obvious
# siblings. Over-listing costs nothing but an ArgN parameter name.
VERSE_STDLIB_NAMES = {
    "Abs", "Ceil", "Floor", "Round", "Sqrt", "Min", "Max", "Sign", "Clamp", "Lerp", "Mod",
    "Sin", "Cos", "Tan", "ArcSin", "ArcCos", "ArcTan", "Pow", "Exp", "Ln",
    "Print", "Err", "Sleep", "Length", "Slice", "Reverse", "Shuffle", "Concatenate", "Fits",
    "ToString", "ToDiagnostic", "ToInt", "ToFloat", "ToChar", "ToRational",
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
    "Vector2": TypeInfo("vector2", "VhFromVector2", False, "VhToVector2", False),
    "Vector3": TypeInfo("vector3", "VhFromVector3", False, "VhToVector3", False),
    "Color": TypeInfo("color", "VhFromColor", False, "VhToColor", False),
    "PackedStringArray": TypeInfo("[]string", None, False, "VhToStrings", False),
}


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


VECTOR_FIELDS = {"vector2": "XY", "vector3": "XYZ", "color": "RGBA"}


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

    fields = VECTOR_FIELDS.get(verse_type)
    if fields:
        m = re.fullmatch(rf"{verse_type.capitalize()}\(([^)]*)\)", default)
        if not m:
            return None
        parts = [p.strip() for p in m.group(1).split(",")]
        if len(parts) != len(fields):
            return None
        numbers = [verse_default_literal("float", p) for p in parts]
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
CONTAINER_PROPERTY_TYPES = {"string", "[]string"}

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
    if info.verse_type in CONTAINER_PROPERTY_TYPES:
        coverage.skip("property_container_type")
        return None
    # An object-typed property would need a getter that cannot fail, and a null Godot object is
    # exactly the absence VhToHandle reports as failure.
    if info.pack_fn == "VhFromObject":
        coverage.skip("property_object_type")
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
    fields = VECTOR_FIELDS.get(ti.verse_type)
    if not fields:
        return lines

    selects = " else ".join(
        f'if ({field} = "{f}") then {current}.{f}' for f in fields[:-1]
    )
    lines += [
        f"    {get_name}<epic_internal>({accessor}:accessor, {field}:string)<transacts>:float =",
        f"        {current} := {read}",
        f"        {selects} else {current}.{fields[-1]}",
        f"    {set_name}<epic_internal>({accessor}:accessor, {field}:string, {value}:float)<transacts>:void =",
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

    return class_blocks, emit_order, method_map


HEADER_TEMPLATE = """using {{/Verse.org/Native}}

# Generated by tools/gen_verse_api.py from godot-cpp/gdextension/extension_api.json
# ({version}). Do not edit by hand.
#
# Godot's Object class is skipped entirely: its own API is almost all Callable- and
# Variant-typed reflection that this bridge cannot marshal (see the type table in
# tools/gen_verse_api.py), so a class whose Godot parent is Object derives directly from
# the hand-written native `object` (see Godot.native.verse) instead of a generated one.
"""


def render(api: dict, class_blocks: list) -> str:
    version = api["header"]["version_full_name"]
    text = HEADER_TEMPLATE.format(version=version)
    text += "\n" + "\n\n".join(class_blocks) + "\n"
    return text


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
    parser.add_argument("--classes-file", default=DEFAULT_CLASSES_FILE)
    parser.add_argument("--all", action="store_true", help="Emit every class in the API")
    parser.add_argument("--out", default=GENERATED_PATH)
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

    if args.all:
        requested = [c["name"] for c in api["classes"] if c["name"] != "Object"]
    else:
        classes_file = resolve(root, args.classes_file)
        requested = read_classes_file(classes_file)

    coverage = Coverage()
    class_blocks, emit_order, method_map = generate(api, requested, coverage)
    text = render(api, class_blocks)
    classes_header_text = render_classes_header(api, emit_order, method_map)

    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(text, encoding="utf-8", newline="\n")

    classes_header_path.parent.mkdir(parents=True, exist_ok=True)
    classes_header_path.write_text(classes_header_text, encoding="utf-8", newline="\n")

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
