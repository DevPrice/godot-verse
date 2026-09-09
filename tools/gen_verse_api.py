#!/usr/bin/env python3
"""Generates host/Verse/GodotClasses.native.verse from godot-cpp/gdextension/extension_api.json.

Mirrors a subset of the Godot class hierarchy as ordinary Verse classes deriving from the
hand-written native godot_object (see host/Verse/Godot.native.verse) and calling the packing
helpers in host/Verse/GodotApi.native.verse. See host/Verse/GodotApi.native.verse's own header
comment for why that keeps a thousand mirrored classes from needing a thousand C++ shadows.
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

BASE_MEMBER_NAMES = {"Handle", "Ready", "Update", "PhysicsUpdate"}

TypeInfo = namedtuple("TypeInfo", ["verse_type", "pack_fn", "pack_decides", "unpack_fn", "unpack_decides"])

# Godot type -> (verse type, packer, packer<decides>, unpacker, unpacker<decides>). Packer/unpacker
# names and their <decides>-ness come straight from GodotApi.native.verse; a method that calls a
# <decides> function must itself be <decides>, but only a <decides> callee is invoked with [...]
# rather than (...) -- see GodotApi.native.verse's bracket-discipline comment.
SCALAR_TYPES = {
    "bool": TypeInfo("logic", "VhFromLogic", False, "VhToLogic", True),
    "int": TypeInfo("int", "VhFromInt", False, "VhToInt", True),
    "int32": TypeInfo("int", "VhFromInt", False, "VhToInt", True),
    "int64": TypeInfo("int", "VhFromInt", False, "VhToInt", True),
    "uint32": TypeInfo("int", "VhFromInt", False, "VhToInt", True),
    "float": TypeInfo("float", "VhFromFloat", False, "VhToFloat", True),
    "String": TypeInfo("string", "VhFromString", False, "VhToString", True),
    "StringName": TypeInfo("string", "VhFromStringName", False, "VhToString", True),
    "NodePath": TypeInfo("string", "VhFromNodePath", False, "VhToString", True),
    "Vector2": TypeInfo("vector2", "VhFromVector2", False, "VhToVector2", True),
    "Vector3": TypeInfo("vector3", "VhFromVector3", False, "VhToVector3", True),
    "Color": TypeInfo("color", "VhFromColor", False, "VhToColor", True),
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
    return "godot_" + "_".join(t.lower() for t in split_pascal(godot_name))


def verse_method_name(godot_name: str) -> str:
    parts = [p for p in godot_name.split("_") if p]
    return "".join(p[0].upper() + p[1:] for p in parts)


def verse_param_name(godot_name: str, index: int, reserved_words: set, used: set, members: set) -> str:
    # A parameter that matches a member of the enclosing class is ambiguous, not shadowing:
    # Tween.set_parallel(parallel) and Tween.parallel() collide, as does any argument named
    # `update` against godot_object's Update.
    candidate = verse_method_name(godot_name)
    if not candidate or candidate in reserved_words or candidate in used or candidate in members:
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
Param = namedtuple("Param", ["verse_name", "type_info"])


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
        params.append(Param(pname, info))

    if unsupported_seen:
        coverage.unsupported(unsupported_seen)
        return None

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
    param_decl = ", ".join(f"{p.verse_name}:{p.type_info.verse_type}" for p in cm.params)
    args = emit_call_args(cm.params)
    call = f'VhCallValue[Handle, "{cm.godot_name}", array{{{args}}}]' if not cm.is_void else None

    if cm.is_void:
        body = f'VhCallVoid(Handle, "{cm.godot_name}", array{{{args}}})'
        return f"    {cm.verse_name}<public>({param_decl})<transacts>:void = {body}"

    ti = cm.return_type
    if ti.pack_fn == "VhFromObject":
        unpack = f"VhToHandle[{call}]"
        body = f"{ti.verse_type}{{Handle := {unpack}}}"
    elif ti.unpack_decides:
        body = f"{ti.unpack_fn}[{call}]"
    else:
        body = f"{ti.unpack_fn}({call})"

    return f"    {cm.verse_name}<public>({param_decl})<decides><transacts>:{ti.verse_type} = {body}"


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

    for name in emit_order:
        parent = parent_map[name]
        base_names = set(BASE_MEMBER_NAMES) if parent == "Object" else set(inherited_names[parent])
        base_verse = "godot_object" if parent == "Object" else verse_class_name(parent)

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
            coverage.methods_emitted += 1

        inherited_names[name] = used
        coverage.classes_emitted += 1

        header = f"{verse_class_name(name)}<public> := class({base_verse}):"
        if emitted_lines:
            class_blocks.append(header + "\n\n" + "\n".join(emitted_lines))
        else:
            class_blocks.append(header)

    return class_blocks, emit_order


HEADER_TEMPLATE = """using {{/Verse.org/Native}}

# Generated by tools/gen_verse_api.py from godot-cpp/gdextension/extension_api.json
# ({version}). Do not edit by hand.
#
# Godot's Object class is skipped entirely: its own API is almost all Callable- and
# Variant-typed reflection that this bridge cannot marshal (see the type table in
# tools/gen_verse_api.py), so a class whose Godot parent is Object derives directly from
# the hand-written native godot_object (see Godot.native.verse) instead of a generated one.
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

}} // namespace verse_api
"""


def render_classes_header(api: dict, emit_order: list) -> str:
    version = api["header"]["version_full_name"]
    pairs = sorted((name, verse_class_name(name)) for name in emit_order)
    entries = "\n".join(f'\t{{ "{godot_name}", "{verse_name}" }},' for godot_name, verse_name in pairs)
    return CLASSES_HEADER_TEMPLATE.format(version=version, entries=entries)


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
    lines.append("whose Godot parent is Object derives from the native godot_object instead.")
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
    class_blocks, emit_order = generate(api, requested, coverage)
    text = render(api, class_blocks)
    classes_header_text = render_classes_header(api, emit_order)

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
