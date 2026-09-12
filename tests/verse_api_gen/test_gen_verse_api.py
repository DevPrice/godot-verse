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
        "Node": "node",
        "CanvasItem": "canvas_item",
        "Node2D": "node2d",
        "AnimatedSprite2D": "animated_sprite2d",
        "RigidBody3D": "rigid_body3d",
        "HTTPRequest": "http_request",
        "XRServer": "xr_server",
        "AABB": "aabb",
        "CPUParticles2D": "cpu_particles2d",
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
        g.verse_param_name("process", 1, reserved, set(), g.BASE_MEMBER_NAMES),
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
        ("node", "null", None),
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
    want = '    GetPosition<public>()<transacts>:vector2 = VhToVector2(VhCallValue(Handle, "get_position", array{}))'
    check("emit value method, scalar return does not claim it can fail", g.emit_method(cm), want)


def test_emit_value_method_class_return():
    ti = g.TypeInfo("node", "VhFromObject", False, "VhToHandle", True)
    cm = g.ClassifiedMethod(
        godot_name="get_child",
        verse_name="GetChild",
        params=[g.Param("Index", g.SCALAR_TYPES["int"], None)],
        return_type=ti,
        is_void=False,
    )
    want = (
        '    GetChild<public>(Index:int)<decides><transacts>:node = '
        'node{Handle := VhToHandle[VhCallValue(Handle, "get_child", array{VhFromInt(Index)})]}'
    )
    check("emit value method, class return stays failable (GetChild)", g.emit_method(cm), want)


def test_packed_arrays_marshal_as_verse_arrays():
    """A packed array rides as a reference id, but the author sees a Verse array either way.

    Two of Godot's packed types can share one Verse type -- PackedFloat32Array and
    PackedFloat64Array are both `[]float` -- so what matters is that the *packer* differs, since
    that is what carries the tag the Godot side rebuilds from.
    """
    check("PackedStringArray is []string", g.SCALAR_TYPES["PackedStringArray"].verse_type, "[]string")
    check("PackedFloat32Array is []float", g.SCALAR_TYPES["PackedFloat32Array"].verse_type, "[]float")
    check("and PackedFloat64Array too", g.SCALAR_TYPES["PackedFloat64Array"].verse_type, "[]float")
    check("but they pack differently",
          g.SCALAR_TYPES["PackedFloat32Array"].pack_fn != g.SCALAR_TYPES["PackedFloat64Array"].pack_fn, True)


def test_reference_types_are_wrappers():
    """Array, Dictionary, Callable and Signal cross as ids, so they map to wrapper classes rather
    than to a Verse value type. `array` is a reserved word, hence `godot_array`."""
    check("Array is godot_array", g.SCALAR_TYPES["Array"].verse_type, "godot_array")
    check("Dictionary", g.SCALAR_TYPES["Dictionary"].verse_type, "dictionary")
    check("Callable", g.SCALAR_TYPES["Callable"].verse_type, "callable")
    names = {name for name, _ in g.REFERENCE_TYPES}
    check("and each is in the host's reference table", names,
          {"godot_array", "dictionary", "callable", "signal_ref"})


def test_container_properties_stay_methods():
    """A `var` whose type is a container cannot work -- Verse asks for accessor overloads no single
    signature satisfies -- so Godot's getter and setter are emitted as ordinary methods."""
    check("a container type is not a var", "godot_array" in g.CONTAINER_PROPERTY_TYPES, True)


def test_math_layout_matches_the_wire():
    """Every math type's flattened leaves must be the lane count src/verse_value.cpp writes.

    The two are separate pieces of code that have to agree component for component; a type written
    with three components and read with two truncates silently, which no other test would catch.
    """
    for name in g.MATH_TYPES:
        lanes = g.math_leaf_lanes(name)
        got = (
            sum(1 for _, kind in lanes if kind == "int"),
            sum(1 for _, kind in lanes if kind == "float"),
        )
        check(f"{name} occupies the lanes the wire gives it", got, g.MATH_LANES[name])


def test_integer_vector_defaults_stay_integers():
    """Verse will not take 0.0 for an int, so Vector2i(0, 0) must not spell its components as
    floats the way Vector2(0, 0) does."""
    check("Vector2i default", g.verse_default_literal("vector2i", "Vector2i(0, 0)"),
          "vector2i{X := 0, Y := 0}")
    check("Vector2 default", g.verse_default_literal("vector2", "Vector2(0, 0)"),
          "vector2{X := 0.0, Y := 0.0}")


def test_nested_math_structs_are_not_vars():
    """A struct whose members are structs cannot be a `var` property: Verse asks for a field-named
    accessor per nesting level, and transform3d's two members have different types, so no single
    getter signature satisfies it. Those stay ordinary methods."""
    check("a flat struct can be a var", "vector3" in g.FLAT_MATH_STRUCTS, True)
    check("a nested one cannot", "transform3d" in g.FLAT_MATH_STRUCTS, False)
    check("nor can plane, whose Normal is a vector3", "plane" in g.FLAT_MATH_STRUCTS, False)


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


def test_singleton_accessors_cover_only_emitted_classes():
    api = {"singletons": [{"name": "Input"}, {"name": "RenderingServer"}]}
    lines = g.emit_singleton_accessors(api, ["Node", "Input"], set())
    check(
        "an accessor for the emitted singleton and nothing else",
        lines,
        ['GetInput<public>()<decides><transacts>:input = input{Handle := VhSingleton["Input"]}'],
    )


def test_singleton_accessor_yields_to_a_method_of_the_same_name():
    api = {"singletons": [{"name": "EditorInterface"}, {"name": "Input"}]}
    lines = g.emit_singleton_accessors(api, ["EditorInterface", "Input"], {"GetEditorInterface"})
    check_true(
        "the accessor a nullary method already claims is renamed",
        any(line.startswith("GetEditorInterfaceSingleton<public>()") for line in lines),
    )
    check_true(
        "and no accessor keeps the colliding name",
        not any(line.startswith("GetEditorInterface<public>()") for line in lines),
    )
    check_true(
        "an accessor nothing collides with is left alone",
        any(line.startswith("GetInput<public>()") for line in lines),
    )


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
    blocks, _emit_order, _method_map, _nullary = g.generate(api, ["Derived"], coverage)
    check("shadowed method skipped once", coverage.skip_reasons["shadow"], 1)
    check("only the non-colliding method emitted on Derived", coverage.methods_emitted, 2)
    derived_block = next(b for b in blocks if b.startswith("derived"))
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
        "method name colliding with `object`'s own Ready is shadowed",
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
                    _method("get_data", "typedarray::Node2D"),
                    _method("get_value", "int"),
                ],
            },
        ]
    }
    coverage = g.Coverage()
    blocks, _emit_order, _method_map, _nullary = g.generate(api, ["Thing"], coverage)
    check("unsupported return type skips its method", coverage.skip_reasons["unsupported_type"], 1)
    check("unsupported type recorded by name", coverage.unsupported_types["typedarray::Node2D"], 1)
    check("the supported sibling method still emits", coverage.methods_emitted, 1)
    thing_block = blocks[-1]
    check_true("GetData (typed array) absent", "GetData" not in thing_block, thing_block)
    check_true("GetValue (int) present", "GetValue" in thing_block, thing_block)


def test_packed_string_array_unsupported_as_parameter():
    api = {
        "classes": [
            {"name": "Object", "inherits": None, "methods": []},
            {
                "name": "Thing",
                "inherits": "Object",
                "methods": [_method("set_names", None, [{"name": "names", "type": "typedarray::StringName"}])],
            },
        ]
    }
    coverage = g.Coverage()
    g.generate(api, ["Thing"], coverage)
    check("a typed-array parameter is unsupported", coverage.skip_reasons["unsupported_type"], 1)
    check("and is recorded as the offending type", coverage.unsupported_types["typedarray::StringName"], 1)


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
    blocks, _emit_order, _method_map, _nullary = g.generate(api, ["Base", "Other"], coverage)
    other_block = next(b for b in blocks if b.startswith("other"))
    check_true(
        "unresolved class type falls back to nearest emitted ancestor (base)",
        "base{Handle := VhToHandle[" in other_block,
        other_block,
    )


def test_render_classes_header():
    api = {"header": {"version_full_name": "Godot Engine v4.6.stable.official"}}
    text = g.render_classes_header(api, ["Node2D", "Node"], [])
    check_true("classes header has a #pragma once", text.startswith("#pragma once"))
    check_true("classes header opens verse_api namespace", "namespace verse_api {" in text)
    check_true(
        "classes header sorts entries by Godot class name (Node before Node2D)",
        text.index('{ "Node", "node" }') < text.index('{ "Node2D", "node2d" }'),
    )


def test_emit_scalar_property():
    cp = g.ClassifiedProperty(
        godot_name="rotation",
        verse_name="Rotation",
        type_info=g.SCALAR_TYPES["float"],
        getter="get_rotation",
        setter="set_rotation",
        index=None,
    )
    lines = g.emit_property(cp, g.accessor_locals(set()))
    check("scalar property emits a var and one accessor pair", len(lines), 3)
    check(
        "the var names its accessors",
        lines[0],
        "    var Rotation<public><getter(RotationGetter)><setter(RotationSetter)>:float = external {}",
    )
    check(
        "the setter writes through the Godot setter",
        lines[2],
        '    RotationSetter<epic_internal>(Accessor:accessor, Value:float)<transacts>:void'
        ' = VhCallVoid(Handle, "set_rotation", array{VhFromFloat(Value)})',
    )


def test_emit_indexed_property_passes_its_index():
    cp = g.ClassifiedProperty(
        godot_name="stretch_margin_top",
        verse_name="StretchMarginTop",
        type_info=g.SCALAR_TYPES["int"],
        getter="get_stretch_margin",
        setter="set_stretch_margin",
        index=1,
    )
    lines = g.emit_property(cp, g.accessor_locals(set()))
    check_true(
        "an indexed property passes its index ahead of the value",
        'array{VhFromInt(1), VhFromInt(Value)}' in lines[2],
    )
    check_true("and ahead of nothing on the read", 'array{VhFromInt(1)}' in lines[1])


def test_struct_property_gets_the_field_overloads():
    cp = g.ClassifiedProperty(
        godot_name="position",
        verse_name="Position",
        type_info=g.SCALAR_TYPES["Vector2"],
        getter="get_position",
        setter="set_position",
        index=None,
    )
    lines = g.emit_property(cp, g.accessor_locals(set()))
    arities = [line for line in lines if "Field:string" in line]
    check("a struct property carries the field-named overload of each accessor", len(arities), 2)


def test_accessor_locals_dodge_a_colliding_member():
    # Range.value would make a parameter named Value ambiguous where the body mentions it.
    names = g.accessor_locals({"Value"})
    check_true("a colliding accessor parameter is renamed", names["Value"].startswith("Arg"))
    check("an uncontested one is not", names["Accessor"], "Accessor")


def test_property_skips_have_reasons():
    resolver = g.TypeResolver({"Node"}, {"Node": "Object"}, {"Node", "Texture2D"})
    coverage = g.Coverage()
    check_true(
        "a read-only property is skipped",
        g.classify_property({"name": "a", "type": "float", "getter": "get_a"}, resolver, coverage) is None,
    )
    check_true(
        "a string property is skipped",
        g.classify_property(
            {"name": "b", "type": "String", "getter": "get_b", "setter": "set_b"}, resolver, coverage
        ) is None,
    )
    check_true(
        "an object property is skipped",
        g.classify_property(
            {"name": "c", "type": "Node", "getter": "get_c", "setter": "set_c"}, resolver, coverage
        ) is None,
    )
    check_true(
        "a property named after a Verse stdlib function is skipped",
        g.classify_property(
            {"name": "max", "type": "float", "getter": "get_max", "setter": "set_max"}, resolver, coverage
        ) is None,
    )
    check_true(
        "a float property is not",
        g.classify_property(
            {"name": "d", "type": "float", "getter": "get_d", "setter": "set_d"}, resolver, coverage
        ) is not None,
    )
    check(
        "each skip recorded its own reason",
        sorted(coverage.skip_reasons),
        ["property_ambiguous_name", "property_container_type", "property_no_accessor_pair",
         "property_object_type"],
    )


def test_method_map_names_the_godot_original():
    api = {"header": {"version_full_name": "Godot Engine v4.6.stable.official"}}
    method_map = [
        ("Node2D", "node2d", "set_position", "SetPosition"),
        ("Node2D", "node2d", "get_position", "GetPosition"),
    ]
    text = g.render_classes_header(api, ["Node2D"], method_map)
    check_true(
        "method map carries the Godot spelling the Verse name cannot be inverted to",
        '{ "node2d", "GetPosition", "Node2D", "get_position" },' in text,
    )
    check_true(
        "method map sorts by Verse class then Verse method",
        text.index('"GetPosition"') < text.index('"SetPosition"'),
    )


def test_generated_method_map_covers_a_known_method():
    header = (REPO_ROOT / "src" / "verse_api_classes.h").read_text(encoding="utf-8")
    # A property lands in the same table as a method: the editor routes both to Godot's own docs,
    # and neither Verse name can be inverted back to the Godot one.
    check_true(
        "the checked-in header maps node2d.Position to Node2D.position",
        '{ "node2d", "Position", "Node2D", "position" },' in header,
    )
    check_true(
        "the checked-in header still maps a surviving method",
        '{ "node", "GetChild", "Node", "get_child" },' in header,
    )
    # object's lifecycle methods are hand-written rather than mirrored -- the generator skips
    # virtuals -- but a script overriding one wants Godot's documentation for it.
    check_true(
        "the checked-in header maps object.Ready to Node._ready",
        '{ "object", "Ready", "Node", "_ready" },' in header,
    )


def test_classes_header_file_matches_generated_verse_file():
    header = REPO_ROOT / "src" / "verse_api_classes.h"
    check_true("verse_api_classes.h exists", header.is_file())
    header_text = header.read_text(encoding="utf-8")
    import re

    verse_text = (REPO_ROOT / "host" / "Verse" / "GodotClasses.native.verse").read_text(encoding="utf-8")
    verse_classes = set(re.findall(r"^(\w+)<public> := class\(", verse_text, re.MULTILINE))
    # Only the class table: the method table below it has rows of the same shape, and its last
    # column is a Godot method name.
    class_table = header_text.split("class_mapping classes[] = {", 1)[1].split("};", 1)[0]
    header_classes = set(re.findall(r'"([^"]+)" \}', class_table))
    # The builtin value types are hand-written in GodotApi.native.verse rather than generated, so
    # they are in the table but never in the mirrored class file.
    check_true(
        "the header also carries the builtin value types",
        set(g.VALUE_TYPE_CLASSES.values()) <= header_classes,
    )
    header_classes -= set(g.VALUE_TYPE_CLASSES.values())
    # The container wrappers are generated into the same file but are not mirrored Godot classes:
    # nothing resolves a node's class to one, so they have no row in the header's table.
    verse_classes -= {name for name, _ in g.REFERENCE_TYPES}
    check(
        "verse_api_classes.h lists exactly the classes GodotClasses.native.verse emits",
        header_classes,
        verse_classes,
    )


def test_generated_file_matches_hand_written_slice():
    generated = REPO_ROOT / "host" / "Verse" / "GodotClasses.native.verse"
    text = generated.read_text(encoding="utf-8")
    check_true("GodotClasses.native.verse exists", generated.is_file())
    # position, rotation and scale are Godot properties, so node2d carries them as writable vars
    # and the get/set pairs they were built from are gone.
    hand_written_lines = [
        '    var Position<public><getter(PositionGetter)><setter(PositionSetter)>:vector2 = external {}',
        '    PositionGetter<epic_internal>(Accessor:accessor)<transacts>:vector2 = VhToVector2(VhCallValue(Handle, "get_position", array{}))',
        '    PositionSetter<epic_internal>(Accessor:accessor, Value:vector2)<transacts>:void = VhCallVoid(Handle, "set_position", array{VhFromVector2(Value)})',
        '    var Rotation<public><getter(RotationGetter)><setter(RotationSetter)>:float = external {}',
        '    RotationGetter<epic_internal>(Accessor:accessor)<transacts>:float = VhToFloat(VhCallValue(Handle, "get_rotation", array{}))',
        '    RotationSetter<epic_internal>(Accessor:accessor, Value:float)<transacts>:void = VhCallVoid(Handle, "set_rotation", array{VhFromFloat(Value)})',
    ]
    import re

    node2d_start = text.find("node2d<public> := class(")
    check_true("node2d is present", node2d_start != -1)
    # A blank line separates a class header from its own methods as well as from the next class,
    # so the block ends at the next header rather than at the next blank line.
    next_class = re.compile(r"^\w+<public> := class\(", re.MULTILINE).search(text, node2d_start + 1)
    node2d_block = text[node2d_start: next_class.start() if next_class else len(text)]
    for line in hand_written_lines:
        check_true(f"node2d contains: {line.strip()[:40]}...", line in node2d_block)

    class_positions = [
        (mm.start(), mm.group(1), mm.group(2))
        for mm in re.finditer(r"^(\w+)<public> := class\((\w+)\):", text, re.MULTILINE)
    ]
    method_re = re.compile(r"^\s{4}(?:var )?(\w+)<public>", re.MULTILINE)
    blocks = {}
    for i, (pos, name, base) in enumerate(class_positions):
        end = class_positions[i + 1][0] if i + 1 < len(class_positions) else len(text)
        body = text[pos:end]
        blocks[name] = {"base": base, "names": method_re.findall(body)}

    # A null Godot object is the only absence a mirrored method on a *Godot class* can report, so
    # every remaining <decides> there must be an object return. Anything else claiming failure is a
    # method whose caller would have to write an `if` around a case that never arrives.
    #
    # Two kinds of definition fail for their own real reasons and are excluded: the singleton
    # accessors, which fail when the name is not registered in this build and are told apart by
    # being at module scope; and a container's element accessors, where an absent key, an index out
    # of range and an element of another type are all genuine misses.
    failable = [
        line for line in text.splitlines()
        if "<decides>" in line and not line.lstrip().startswith("#") and "VhRefGet[" not in line
    ]
    check_true(
        "every failable method on a mirrored Godot class returns an object",
        any(line.startswith("    ") for line in failable)
        and all("VhToHandle[" in line for line in failable if line.startswith("    ")),
    )
    container_reads = [
        line for line in text.splitlines()
        if "<decides>" in line and "VhRefGet[" in line
    ]
    check_true(
        "a container's element accessors are the other failable methods",
        container_reads and all(line.lstrip().startswith("Get") for line in container_reads),
    )

    accessors = [line for line in failable if not line.startswith("    ")]
    check_true(
        "the only failable free functions are the singleton accessors",
        accessors and all("VhSingleton[" in line for line in accessors),
    )
    check_true(
        "a mirrored singleton gets an accessor",
        'GetInput<public>()<decides><transacts>:input = input{Handle := VhSingleton["Input"]}' in text,
    )

    base_members = {"Handle", "Ready", "Process", "PhysicsProcess"}

    def inherited(name):
        if name == "object":
            return set(base_members)
        # A base this file does not declare is hand-written in Godot.native.verse -- `godot_ref`,
        # which the container wrappers derive from. Its members are not the generator's to check.
        info = blocks.get(name)
        if info is None:
            return set(base_members)
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
    test_packed_arrays_marshal_as_verse_arrays()
    test_reference_types_are_wrappers()
    test_container_properties_stay_methods()
    test_math_layout_matches_the_wire()
    test_integer_vector_defaults_stay_integers()
    test_nested_math_structs_are_not_vars()
    test_ancestor_pull_in()
    test_singleton_accessors_cover_only_emitted_classes()
    test_singleton_accessor_yields_to_a_method_of_the_same_name()
    test_shadow_suppression_across_inheritance()
    test_base_member_shadow()
    test_unsupported_type_skipping()
    test_packed_string_array_unsupported_as_parameter()
    test_class_type_falls_back_to_nearest_emitted_ancestor()
    test_render_classes_header()
    test_emit_scalar_property()
    test_emit_indexed_property_passes_its_index()
    test_struct_property_gets_the_field_overloads()
    test_accessor_locals_dodge_a_colliding_member()
    test_property_skips_have_reasons()
    test_method_map_names_the_godot_original()
    test_generated_method_map_covers_a_known_method()
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
