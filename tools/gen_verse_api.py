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

BASE_MEMBER_NAMES = {"Handle", "IsInstanceValid", "Ready", "Update", "PhysicsUpdate"}

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
    # `update` against `object`'s Update.
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
        # Every name the class will carry, so a parameter can be checked against members that
        # have not been classified yet as well as inherited ones.
        member_names = base_names | {verse_method_name(m["name"]) for m in methods}

        candidates = []
        for m in methods:
            cm = classify_method(m, resolver, coverage, member_names)
            if cm is not None:
                candidates.append(cm)
        candidates.sort(key=lambda cm: (cm.verse_name, cm.godot_name))

        used = set(base_names)
        emitted_lines = []
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


def render_classes_header(api: dict, emit_order: list, method_map: list) -> str:
    version = api["header"]["version_full_name"]
    pairs = sorted((name, verse_class_name(name)) for name in emit_order)
    entries = "\n".join(f'\t{{ "{godot_name}", "{verse_name}" }},' for godot_name, verse_name in pairs)
    method_entries = "\n".join(
        f'\t{{ "{verse_class}", "{verse_method}", "{godot_class}", "{godot_method}" }},'
        for godot_class, verse_class, godot_method, verse_method in sorted(
            method_map, key=lambda m: (m[1], m[3])
        )
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
