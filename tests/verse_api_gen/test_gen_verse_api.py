#!/usr/bin/env python3
"""Plain-python tests for tools/gen_verse_api.py. No pytest dependency: prints one line per
case and exits non-zero if any case fails.
"""

import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(REPO_ROOT / "tools"))

import gen_verse_api as g  # noqa: E402

failures = []


def check(name, got, want):
    ok = got == want
    print(f"{'ok' if ok else 'FAIL'} - {name}" + ("" if ok else f" (got {got!r}, want {want!r})"))
    if not ok:
        failures.append(name)


def check_true(name, condition, detail=""):
    print(f"{'ok' if condition else 'FAIL'} - {name}" + ("" if condition else f" ({detail})"))
    if not condition:
        failures.append(name)


def test_class_names():
    cases = {
        "Node": "godot_node",
        "CanvasItem": "godot_canvas_item",
        "Node2D": "godot_node2d",
        "AnimatedSprite2D": "godot_animated_sprite2d",
        "RigidBody3D": "godot_rigid_body3d",
        "HTTPRequest": "godot_http_request",
        "XRServer": "godot_xr_server",
        "AABB": "godot_aabb",
        "CPUParticles2D": "godot_cpu_particles2d",
    }
    for godot_name, want in cases.items():
        check(f"class name {godot_name}", g.verse_class_name(godot_name), want)


def test_method_names():
    cases = {
        "get_position": "GetPosition",
        "set_v_size_flags": "SetVSizeFlags",
        "is_inside_tree": "IsInsideTree",
    }
    for godot_name, want in cases.items():
        check(f"method name {godot_name}", g.verse_method_name(godot_name), want)


def test_param_names():
    reserved = g.load_reserved_words(REPO_ROOT / "src" / "verse_keywords.h")
    check("param name 'position'", g.verse_param_name("position", 0, reserved, set(), set()), "Position")
    check("param name 'to_position' splits like a method name", g.verse_param_name("to_position", 0, reserved, set(), set()), "ToPosition")
    check("param name 'self' collides with reserved word", g.verse_param_name("self", 2, reserved, set(), set()), "Arg2")
    check("param name '' falls back on index", g.verse_param_name("", 3, reserved, set(), set()), "Arg3")
    check(
        "param name colliding with an already-used name in this method",
        g.verse_param_name("Foo", 1, reserved, {"Foo"}, set()),
        "Arg1",
    )
    check(
        "param name colliding with a member of the enclosing class",
        g.verse_param_name("parallel", 0, reserved, set(), {"Parallel"}),
        "Arg0",
    )
    check(
        "param name colliding with an inherited lifecycle method",
        g.verse_param_name("update", 1, reserved, set(), g.BASE_MEMBER_NAMES),
        "Arg1",
    )


def test_default_literals():
    cases = [
        ("logic", "false", "false"),
        ("int", "-1", "-1"),
        ("float", "-1", "-1.0"),
        ("float", "0.0", "0.0"),
        ("string", '&""', '""'),
        ("string", 'NodePath("")', '""'),
        ("color", "Color(1, 1, 1, 1)", "color{R := 1.0, G := 1.0, B := 1.0, A := 1.0}"),
        ("vector2", "Vector2(0, -1)", "vector2{X := 0.0, Y := -1.0}"),
        ("int", "null", None),
        ("godot_node", "null", None),
        ("[]string", "[]", None),
    ]
    for verse_type, default, want in cases:
        check(f"default {verse_type} {default!r}", g.verse_default_literal(verse_type, default), want)

    cm = g.ClassifiedMethod(
        godot_name="get_child",
        verse_name="GetChild",
        params=[g.Param("Idx", g.SCALAR_TYPES["int"], None),
                g.Param("IncludeInternal", g.SCALAR_TYPES["bool"], "false")],
        return_type=None,
        is_void=True,
    )
    check(
        "a defaulted parameter is emitted as an optional named one",
        "?IncludeInternal:logic = false" in g.emit_method(cm),
        True,
    )


def test_emit_void_method():
    cm = g.ClassifiedMethod(
        godot_name="set_position",
        verse_name="SetPosition",
        params=[g.Param("Position", g.SCALAR_TYPES["float"], None)],
        return_type=None,
        is_void=True,
    )
    # Use the real Vector2 TypeInfo (SCALAR_TYPES keys on the Godot type spelling).
    cm = cm._replace(params=[g.Param("Position", g.SCALAR_TYPES["Vector2"], None)])
    want = '    SetPosition<public>(Position:vector2)<transacts>:void = VhCallVoid(Handle, "set_position", array{VhFromVector2(Position)})'
    check("emit void method (SetPosition)", g.emit_method(cm), want)


def test_emit_value_method_scalar():
    cm = g.ClassifiedMethod(
        godot_name="get_position",
        verse_name="GetPosition",
        params=[],
        return_type=g.SCALAR_TYPES["Vector2"],
        is_void=False,
    )
    want = '    GetPosition<public>()<decides><transacts>:vector2 = VhToVector2[VhCallValue[Handle, "get_position", array{}]]'
    check("emit value method, scalar return (GetPosition)", g.emit_method(cm), want)


def test_emit_value_method_class_return():
    ti = g.TypeInfo("godot_node", "VhFromObject", False, "VhToHandle", True)
    cm = g.ClassifiedMethod(
        godot_name="get_child",
        verse_name="GetChild",
        params=[g.Param("Index", g.SCALAR_TYPES["int"], None)],
        return_type=ti,
        is_void=False,
    )
    want = (
        '    GetChild<public>(Index:int)<decides><transacts>:godot_node = '
        'godot_node{Handle := VhToHandle[VhCallValue[Handle, "get_child", array{VhFromInt(Index)}]]}'
    )
    check("emit value method, class return (GetChild)", g.emit_method(cm), want)


def test_emit_packed_string_array_return():
    ti = g.SCALAR_TYPES["PackedStringArray"]
    cm = g.ClassifiedMethod(
        godot_name="get_meta_list",
        verse_name="GetMetaList",
        params=[],
        return_type=ti,
        is_void=False,
    )
    want = '    GetMetaList<public>()<decides><transacts>:[]string = VhToStrings(VhCallValue[Handle, "get_meta_list", array{}])'
    check("emit value method, PackedStringArray return uses non-decides unpacker", g.emit_method(cm), want)


def test_ancestor_pull_in():
    parent_map = {
        "Object": None,
        "Node": "Object",
        "CanvasItem": "Node",
        "Node2D": "CanvasItem",
        "Sprite2D": "Node2D",
    }
    order = g.compute_emit_set(["Sprite2D"], parent_map)
    check("ancestor pull-in order", order, ["Node", "CanvasItem", "Node2D", "Sprite2D"])
    check_true("Object never pulled in", "Object" not in order)


def _method(name, ret_type=None, args=None):
    m = {"name": name, "is_virtual": False, "is_static": False, "is_vararg": False}
    m["return_value"] = {"type": ret_type} if ret_type else None
    m["arguments"] = args or []
    return m


def test_shadow_suppression_across_inheritance():
    api = {
        "classes": [
            {"name": "Object", "inherits": None, "methods": []},
            {"name": "Base", "inherits": "Object", "methods": [_method("get_value", "int")]},
            {
                "name": "Derived",
                "inherits": "Base",
                "methods": [_method("get_value", "int"), _method("get_other", "int")],
            },
        ]
    }
    coverage = g.Coverage()
    blocks, _emit_order = g.generate(api, ["Derived"], coverage)
    check("shadowed method skipped once", coverage.skip_reasons["shadow"], 1)
    check("only the non-colliding method emitted on Derived", coverage.methods_emitted, 2)
    derived_block = next(b for b in blocks if b.startswith("godot_derived"))
    check_true("Derived does not redeclare GetValue", "GetValue" not in derived_block, derived_block)
    check_true("Derived declares GetOther", "GetOther" in derived_block, derived_block)


def test_base_member_shadow():
    api = {
        "classes": [
            {"name": "Object", "inherits": None, "methods": []},
            {"name": "Thing", "inherits": "Object", "methods": [_method("ready", "bool")]},
        ]
    }
    coverage = g.Coverage()
    g.generate(api, ["Thing"], coverage)
    check(
        "method name colliding with godot_object's own Ready is shadowed",
        coverage.skip_reasons["shadow"],
        1,
    )


def test_unsupported_type_skipping():
    api = {
        "classes": [
            {"name": "Object", "inherits": None, "methods": []},
            {
                "name": "Thing",
                "inherits": "Object",
                "methods": [
                    _method("get_data", "Dictionary"),
                    _method("get_value", "int"),
                ],
            },
        ]
    }
    coverage = g.Coverage()
    blocks, _emit_order = g.generate(api, ["Thing"], coverage)
    check("unsupported return type skips its method", coverage.skip_reasons["unsupported_type"], 1)
    check("unsupported type recorded by name", coverage.unsupported_types["Dictionary"], 1)
    check("the supported sibling method still emits", coverage.methods_emitted, 1)
    thing_block = blocks[-1]
    check_true("GetData (Dictionary) absent", "GetData" not in thing_block, thing_block)
    check_true("GetValue (int) present", "GetValue" in thing_block, thing_block)


def test_packed_string_array_unsupported_as_parameter():
    api = {
        "classes": [
            {"name": "Object", "inherits": None, "methods": []},
            {
                "name": "Thing",
                "inherits": "Object",
                "methods": [_method("set_names", None, [{"name": "names", "type": "PackedStringArray"}])],
            },
        ]
    }
    coverage = g.Coverage()
    g.generate(api, ["Thing"], coverage)
    check("PackedStringArray parameter is unsupported", coverage.skip_reasons["unsupported_type"], 1)
    check("PackedStringArray recorded as the offending type", coverage.unsupported_types["PackedStringArray"], 1)


def test_class_type_falls_back_to_nearest_emitted_ancestor():
    api = {
        "classes": [
            {"name": "Object", "inherits": None, "methods": []},
            {"name": "Base", "inherits": "Object", "methods": []},
            {"name": "Mid", "inherits": "Base", "methods": []},
            {"name": "Leaf", "inherits": "Mid", "methods": []},
            {
                "name": "Other",
                "inherits": "Object",
                "methods": [_method("get_leaf", "Leaf")],
            },
        ]
    }
    coverage = g.Coverage()
    # Base is emitted, but Mid/Leaf are not requested -- Other.GetLeaf must fall back to Base.
    blocks, _emit_order = g.generate(api, ["Base", "Other"], coverage)
    other_block = next(b for b in blocks if b.startswith("godot_other"))
    check_true(
        "unresolved class type falls back to nearest emitted ancestor (godot_base)",
        "godot_base{Handle := VhToHandle[" in other_block,
        other_block,
    )


def test_render_classes_header():
    api = {"header": {"version_full_name": "Godot Engine v4.6.stable.official"}}
    text = g.render_classes_header(api, ["Node2D", "Node"])
    check_true("classes header has a #pragma once", text.startswith("#pragma once"))
    check_true("classes header opens verse_api namespace", "namespace verse_api {" in text)
    check_true(
        "classes header sorts entries by Godot class name (Node before Node2D)",
        text.index('{ "Node", "godot_node" }') < text.index('{ "Node2D", "godot_node2d" }'),
    )


def test_classes_header_file_matches_generated_verse_file():
    header = REPO_ROOT / "src" / "verse_api_classes.h"
    check_true("verse_api_classes.h exists", header.is_file())
    header_text = header.read_text(encoding="utf-8")
    import re

    verse_text = (REPO_ROOT / "host" / "Verse" / "GodotClasses.native.verse").read_text(encoding="utf-8")
    verse_classes = set(re.findall(r"^(godot_\w+)<public> := class\(", verse_text, re.MULTILINE))
    header_classes = set(re.findall(r'"([^"]+)" \}', header_text))
    check(
        "verse_api_classes.h lists exactly the classes GodotClasses.native.verse emits",
        header_classes,
        verse_classes,
    )


def test_generated_file_matches_hand_written_slice():
    generated = REPO_ROOT / "host" / "Verse" / "GodotClasses.native.verse"
    text = generated.read_text(encoding="utf-8")
    check_true("GodotClasses.native.verse exists", generated.is_file())
    hand_written_lines = [
        '    GetPosition<public>()<decides><transacts>:vector2 = VhToVector2[VhCallValue[Handle, "get_position", array{}]]',
        '    SetPosition<public>(Position:vector2)<transacts>:void = VhCallVoid(Handle, "set_position", array{VhFromVector2(Position)})',
        '    GetRotation<public>()<decides><transacts>:float = VhToFloat[VhCallValue[Handle, "get_rotation", array{}]]',
        '    SetRotation<public>(Radians:float)<transacts>:void = VhCallVoid(Handle, "set_rotation", array{VhFromFloat(Radians)})',
        '    GetScale<public>()<decides><transacts>:vector2 = VhToVector2[VhCallValue[Handle, "get_scale", array{}]]',
        '    SetScale<public>(Scale:vector2)<transacts>:void = VhCallVoid(Handle, "set_scale", array{VhFromVector2(Scale)})',
    ]
    node2d_start = text.find("godot_node2d<public> := class(")
    check_true("godot_node2d is present", node2d_start != -1)
    next_class = text.find("\n\ngodot_", node2d_start + 1)
    node2d_block = text[node2d_start: next_class if next_class != -1 else len(text)]
    for line in hand_written_lines:
        check_true(f"godot_node2d contains: {line.strip()[:40]}...", line in node2d_block)

    import re

    class_positions = [
        (mm.start(), mm.group(1), mm.group(2))
        for mm in re.finditer(r"^(\w+)<public> := class\((\w+)\):", text, re.MULTILINE)
    ]
    method_re = re.compile(r"^\s{4}(\w+)<public>", re.MULTILINE)
    blocks = {}
    for i, (pos, name, base) in enumerate(class_positions):
        end = class_positions[i + 1][0] if i + 1 < len(class_positions) else len(text)
        body = text[pos:end]
        blocks[name] = {"base": base, "names": method_re.findall(body)}

    base_members = {"Handle", "Ready", "Update", "PhysicsUpdate"}

    def inherited(name):
        if name == "godot_object":
            return set(base_members)
        info = blocks[name]
        return inherited(info["base"]) | set(info["names"])

    no_redeclare = True
    for name, info in blocks.items():
        anc = inherited(info["base"])
        if set(info["names"]) & anc:
            no_redeclare = False
            break
    check_true("no generated class redeclares an inherited method name", no_redeclare)


def main():
    test_class_names()
    test_method_names()
    test_param_names()
    test_default_literals()
    test_emit_void_method()
    test_emit_value_method_scalar()
    test_emit_value_method_class_return()
    test_emit_packed_string_array_return()
    test_ancestor_pull_in()
    test_shadow_suppression_across_inheritance()
    test_base_member_shadow()
    test_unsupported_type_skipping()
    test_packed_string_array_unsupported_as_parameter()
    test_class_type_falls_back_to_nearest_emitted_ancestor()
    test_render_classes_header()
    test_classes_header_file_matches_generated_verse_file()
    test_generated_file_matches_hand_written_slice()

    print()
    if failures:
        print(f"{len(failures)} check(s) FAILED: {failures}")
        return 1
    print("all checks passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
