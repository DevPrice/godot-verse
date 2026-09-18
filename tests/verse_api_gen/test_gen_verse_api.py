#!/usr/bin/env python3
"""Plain-python tests for tools/gen_verse_api.py. No pytest dependency: prints one line per
case and exits non-zero if any case fails.
"""

import sys
import tempfile
import textwrap
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
        "param name colliding with an inherited member of the native root",
        g.verse_param_name("handle", 1, reserved, set(), g.BASE_MEMBER_NAMES),
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


def test_a_const_method_reads_and_a_mutating_one_transacts():
    """R-AUD-1 / Phase 4.5 3. `is_const` is the only thing that decides this, and the effect and
    the native it dispatches through have to move together: a `<reads>` body may not call
    `VhCallValue`."""
    cm = g.ClassifiedMethod(
        godot_name="get_position",
        verse_name="GetPosition",
        params=[],
        return_type=g.SCALAR_TYPES["Vector2"],
        is_void=False,
        is_const=True,
    )
    want = ('    GetPosition<public>()<reads>:vector2 = '
            'VhToVector2(VhCallValueConst(Handle, "get_position", array{}))')
    check("a const method is <reads> and dispatches through the const native", g.emit_method(cm), want)

    # The same method with Godot's flag off, which is the only difference between the two lines.
    check("and without the flag it is <transacts> again",
          g.emit_method(cm._replace(is_const=False)),
          '    GetPosition<public>()<transacts>:vector2 = '
          'VhToVector2(VhCallValue(Handle, "get_position", array{}))')

    # Godot's `const` means "does not mutate the C++ object", not "has no effect": the 38 methods
    # that are const and return nothing are OS.set_environment, CanvasItem.draw_string and 36 more
    # of that shape. classify_method is where the conjunction is applied.
    coverage = g.Coverage()
    resolver = g.TypeResolver({}, set(), {})
    const_void = _method("draw_string")
    const_void["is_const"] = True
    classified = g.classify_method(const_void, resolver, coverage, set(), "CanvasItem")
    check("a const method that returns nothing is not <reads>", classified.is_const, False)

    const_value = _method("get_position", "Vector2")
    const_value["is_const"] = True
    check("a const method that answers is",
          g.classify_method(const_value, resolver, coverage, set(), "Node2D").is_const, True)

    # CONST_OVERRIDES, the other direction: Godot's flag missing rather than too broad. Every row
    # was read out of Godot's source by tools/audit_const_overrides.py, and `Tween::is_running` is
    # `bool is_running() { return running; }`.
    check_true("the override table names a class and a method",
               ("Tween", "is_running") in g.CONST_OVERRIDES)
    check("an overridden method is <reads> despite Godot's flag",
          g.classify_method(_method("is_running", "bool"), resolver, coverage, set(), "Tween").is_const,
          True)
    check("and the same name on a class the audit did not accept is not",
          g.classify_method(_method("is_running", "bool"), resolver, coverage, set(), "AudioStreamPlayer").is_const,
          False)
    # The override cannot resurrect a void method: the conjunction above still applies, and a
    # const-and-void method is `OS.delay_msec`-shaped whatever any table says.
    check("an override does not make a void method <reads>",
          g.classify_method(_method("is_running"), resolver, coverage, set(), "Tween").is_const,
          False)


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
        'node[VhObjectFrom[VhCallValue(Handle, "get_child", array{VhFromInt(Index)})]]'
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
    # Object is mirrored like any other class since Phase 2, and comes first because it is the root.
    # What it derives from is the hand-written native `vh_object`, which is not in the API at all.
    check("ancestor pull-in order", order, ["Object", "Node", "CanvasItem", "Node2D", "Sprite2D"])


def test_singleton_accessors_cover_only_emitted_classes():
    api = {"singletons": [{"name": "Input"}, {"name": "RenderingServer"}]}
    lines = g.emit_singleton_accessors(api, ["Node", "Input"], set())
    check(
        "an accessor for the emitted singleton and nothing else",
        lines,
        ['GetInput<public>()<reads>:input = if (S := input[VhSingletonObject("Input")]) then S'
         ' else Err("Godot has no Input singleton; it is registered before any scene loads,'
         ' so this is a bridge failure rather than a script error")'],
    )


def test_only_the_editor_singletons_are_failable():
    """R-TYPE-4: a singleton Godot registers before any scene loads is not a nullable type.

    `api_type` is the whole test, so a Godot release that moves a singleton into or out of the
    editor moves its accessor's failability with it and nothing here has to be revised by hand.
    """
    api = {
        "singletons": [{"name": "Input"}, {"name": "EditorInterface"}],
        "classes": [
            {"name": "Input", "api_type": "core"},
            {"name": "EditorInterface", "api_type": "editor"},
        ],
    }
    lines = g.emit_singleton_accessors(api, ["EditorInterface", "Input"], set())
    check(
        "the editor-only singleton is the one that keeps <decides>",
        [line for line in lines if "<decides>" in line],
        ['GetEditorInterface<public>()<decides><reads>:editor_interface'
         ' = editor_interface[VhSingletonObject("EditorInterface")]'],
    )
    check_true(
        "and a singleton that cannot be absent raises instead of failing",
        all('else Err("Godot has no Input singleton' in line
            for line in lines if "<decides>" not in line),
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
    blocks, _emit_order, _method_map, _members, _arrays, _dicts = g.generate(api, ["Derived"], coverage, {})
    check("shadowed method skipped once", coverage.skip_reasons["shadow"], 1)
    check("only the non-colliding method emitted on Derived", coverage.methods_emitted, 2)
    derived_block = next(b for b in blocks if b.startswith("derived"))
    check_true("Derived does not redeclare GetValue", "GetValue" not in derived_block, derived_block)
    check_true("Derived declares GetOther", "GetOther" in derived_block, derived_block)


def test_base_member_shadow():
    api = {
        "classes": [
            {"name": "Object", "inherits": None, "methods": []},
            {"name": "Thing", "inherits": "Object",
             "methods": [dict(_method("_notification", "bool"), is_virtual=True)]},
        ]
    }
    coverage = g.Coverage()
    g.generate(api, ["Thing"], coverage, {})
    check(
        "a method colliding with the native root's own _Notification is shadowed",
        coverage.skip_reasons["shadow"],
        1,
    )


def test_get_node_takes_the_or_null_spelling():
    """Godot's two lookups are one signature in Verse, and the silent one keeps the plain name.

    R-TYPE-4 makes every object return `<decides>`, so `get_node` and `get_node_or_null` cross
    identically; all that is left of the difference is the `ERR_FAIL_V_MSG` in Godot's `get_node`,
    which prints on the branch the compiler has already forced the author to write.
    """
    api = {
        "classes": [
            {"name": "Object", "inherits": None, "methods": []},
            {
                "name": "Node",
                "inherits": "Object",
                "methods": [
                    dict(_method("get_node", "Node", [{"name": "path", "type": "NodePath"}]),
                         is_const=True),
                    dict(_method("get_node_or_null", "Node", [{"name": "path", "type": "NodePath"}]),
                         is_const=True),
                ],
            },
        ]
    }
    coverage = g.Coverage()
    blocks, _emit_order, _method_map, _members, _arrays, _dicts = g.generate(api, ["Node"], coverage, {})
    node_block = next(b for b in blocks if b.startswith("node"))
    check_true("GetNode dispatches get_node_or_null",
               "GetNode<public>(Path:string)<decides><reads>:node" in node_block
               and '"get_node_or_null"' in node_block, node_block)
    check_true("and Godot's own get_node is not emitted", '"get_node"' not in node_block, node_block)
    check("both spellings accounted for", coverage.skip_reasons["method_renamed"], 2)
    rows = {(sm.verse_name, sm.reason) for sm in coverage.skipped_members}
    check_true("only the lost spelling gets a lookup row, since GetNode itself resolves",
               ("GetNodeOrNull", "method_renamed") in rows
               and ("GetNode", "method_renamed") not in rows, sorted(rows))


def test_a_predicate_decides_and_an_accessor_answers_logic():
    """Godot's `bool` is two things, and Verse spells the test and the value differently.

    The rule is PREDICATE_NAME_RE plus a `set_` twin check; the shape it copies is Epic's own
    `.native.verse` bindings, where `IsInScene()<decides><reads>:void` sits beside
    `GetCollidable()<reads>:logic`. `GodotMath.native.verse` already spelled every hand-written
    predicate this way, so this is the mirror catching up with its own math types.
    """
    api = {
        "classes": [
            {"name": "Object", "inherits": None, "methods": []},
            {
                "name": "ZIPReader",
                "inherits": "Object",
                "methods": [
                    # Predicate-named, no twin: the common case.
                    dict(_method("is_playing", "bool"), is_const=True),
                    # An accessor, told apart by its twin rather than by its name.
                    dict(_method("is_point_disabled", "bool",
                                 [{"name": "id", "type": "int"}]), is_const=True),
                    _method("set_point_disabled", None, [{"name": "id", "type": "int"},
                                                         {"name": "disabled", "type": "bool"}]),
                    # A server's flattened pair, where the twin is not a prefix.
                    dict(_method("font_is_force_autohinter", "bool"), is_const=True),
                    _method("font_set_force_autohinter", None, [{"name": "on", "type": "bool"}]),
                    # An outcome rather than a success, and unprefixed: stays a value.
                    _method("move_and_slide", "bool"),
                    # Unprefixed, and a predicate anyway -- PREDICATE_EXTRA keys this one by class.
                    _method("file_exists", "bool", [{"name": "path", "type": "String"}]),
                    # A virtual, which is a predicate whatever its name.
                    dict(_method("_has_point", "bool"), is_virtual=True),
                ],
            },
        ]
    }
    coverage = g.Coverage()
    blocks, _emit_order, _method_map, _members, _arrays, _dicts = g.generate(api, ["ZIPReader"], coverage, {})
    block = next(b for b in blocks if b.startswith("zip_reader"))
    for name, want in [
        ("IsPlaying", "IsPlaying<public>()<decides><reads>:void"),
        ("IsPointDisabled", "IsPointDisabled<public>(Id:int)<reads>:logic"),
        ("FontIsForceAutohinter", "FontIsForceAutohinter<public>()<reads>:logic"),
        ("MoveAndSlide", "MoveAndSlide<public>()<transacts>:logic"),
        ("FileExists", "FileExists<public>(Path:string)<decides><transacts>:void"),
        ("_HasPoint", "_HasPoint<public>()<decides>:void"),
    ]:
        check_true(f"{name} is spelled {want.split(chr(40))[0]}...", want in block, block)
    check_true("a predicate's body is the query operator over VhToLogic",
               "VhToLogic(VhCallValueConst(Handle, \"is_playing\", array{}))?" in block, block)


def test_a_bool_virtual_decides_with_nothing_but_decides():
    """A bool virtual is `<decides>:void`, and the specifier is alone on purpose.

    `<decides>` does not narrow: its effect descriptor rescinds only `decides` and excludes
    nothing, so the declaration keeps the wide default set and an override may still call a
    specifier-less helper. `<decides><transacts>` would rescind `no_rollback` and start wall 8's
    cascade at every such call. Both halves are measured in
    `tests/verse_probe/decides_virtual_{probe,reject}.verse`; this asserts the generator emits the
    one that was chosen.

    The default body has to fail, because an unoverridden Godot virtual answers "not handled".
    """
    api = {
        "classes": [
            {"name": "Object", "inherits": None, "methods": []},
            {
                "name": "Control",
                "inherits": "Object",
                "methods": [
                    dict(_method("_has_point", "bool"), is_virtual=True),
                    # Not predicate-named and with no `set_` twin, so only being a virtual puts it
                    # here -- which is the rule this case exists for.
                    dict(_method("_process", "bool"), is_virtual=True),
                    # A void virtual is untouched, and so is one that answers a value.
                    dict(_method("_draw", None), is_virtual=True),
                    dict(_method("_get_minimum_size", "int"), is_virtual=True),
                ],
            },
        ]
    }
    coverage = g.Coverage()
    blocks, _emit_order, _method_map, _members, _arrays, _dicts = g.generate(api, ["Control"], coverage, {})
    block = next(b for b in blocks if b.startswith("control"))
    check_true("a bool virtual is <decides>:void with a failing default",
               "_HasPoint<public>()<decides>:void = false?" in block, block)
    check_true("and it is so whatever the name says",
               "_Process<public>()<decides>:void = false?" in block, block)
    check_true("the specifier is <decides> and nothing else",
               "<decides><transacts>:void = false?" not in block
               and "<decides><reads>:void = false?" not in block, block)
    check_true("a void virtual is untouched", "_Draw<public>():void = {}" in block, block)
    check_true("and so is one that answers a value",
               "_GetMinimumSize<public>():int = 0" in block, block)


def test_virtual_names_keep_godots_underscore():
    """A virtual is `_Ready`, not `Ready`, and it is measured rather than preferred.

    Eight virtuals collide with a *signal* of the same PascalCase name on the classes an ordinary
    script derives from -- Node.ready, CanvasItem.draw, Control.gui_input, BaseButton.pressed and
    four more. The underscore Godot already uses is the disambiguation, and throwing it away is
    what made those collide.
    """
    for godot_name, want in (("_ready", "_Ready"), ("_physics_process", "_PhysicsProcess"),
                             ("_get_minimum_size", "_GetMinimumSize"), ("get_child", "GetChild")):
        check(f"virtual name {godot_name}", g.verse_virtual_name(godot_name), want)


def test_virtual_emits_a_default_body():
    api = {
        "classes": [
            {"name": "Object", "inherits": None, "methods": []},
            {"name": "Thing", "inherits": "Object", "methods": [
                dict(_method("_ready", None), is_virtual=True),
                dict(_method("_has_point", "bool"), is_virtual=True),
                dict(_method("_get_minimum_size", "int"), is_virtual=True),
                # No default can be written for an object return, so it is a recorded skip rather
                # than a silent absence.
                dict(_method("_get_owner", "Object"), is_virtual=True),
            ]},
        ]
    }
    coverage = g.Coverage()
    blocks, _order, _map, _members, _arrays, _dicts = g.generate(api, ["Thing"], coverage, {})
    block = next(b for b in blocks if b.startswith("thing"))
    check_true("a void virtual is declared with an empty body",
               "    _Ready<public>():void = {}" in block, block)
    check_true("a value-returning one answers Godot's own default",
               "    _GetMinimumSize<public>():int = 0" in block, block)
    check_true("and a bool one declines instead, because it is <decides>",
               "    _HasPoint<public>()<decides>:void = false?" in block, block)
    check_true("a virtual with no writable default is skipped, not emitted",
               "_GetOwner" not in block, block)
    check("and the skip says why", coverage.skip_reasons.get("virtual_no_default"), 1)


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
    blocks, _emit_order, _method_map, _members, _arrays, _dicts = g.generate(api, ["Thing"], coverage, {})
    check("unsupported return type skips its method", coverage.skip_reasons["unsupported_type"], 1)
    check("unsupported type recorded by name", coverage.unsupported_types["typedarray::Node2D"], 1)
    check("the supported sibling method still emits", coverage.methods_emitted, 1)
    thing_block = blocks[-1]
    check_true("GetData (typed array) absent", "GetData" not in thing_block, thing_block)
    check_true("GetValue (int) present", "GetValue" in thing_block, thing_block)


def test_typed_array_parameter_takes_the_parametric_class():
    api = {
        "classes": [
            {"name": "Object", "inherits": None, "methods": []},
            {
                "name": "Thing",
                "inherits": "Object",
                "methods": [
                    _method("set_names", None, [{"name": "names", "type": "typedarray::StringName"}]),
                    _method("get_kids", "typedarray::Thing"),
                ],
            },
        ]
    }
    coverage = g.Coverage()
    blocks, _order, _map, _members, arrays, _dicts = g.generate(api, ["Thing"], coverage, {})
    check("no typed array is unsupported any more", coverage.skip_reasons["unsupported_type"], 0)
    check_true(
        "a typed-array parameter takes the parametric class",
        "SetNames<public>(Names:typed_array(string))" in blocks[-1],
    )
    check_true(
        "and a typed-array return hands one back",
        "GetKids<public>()<transacts>:typed_array(thing)" in blocks[-1],
    )
    check("both element types were recorded", sorted(arrays), ["StringName", "Thing"])

    converters = "\n".join(g.emit_typed_array_converters(arrays, {}))
    check_true(
        "a lane element reuses its own reader rather than getting a new one",
        "Unpack := VhUnpackStringName" in converters and "VhToStringNameElement" not in converters,
    )
    check_true(
        "a class element gets one, because the converter has to name the class",
        "VhToThingElement(Value:variant)<decides><reads>:thing" in converters,
    )


def test_union_parameter_widens_to_the_common_ancestor():
    api = {
        "classes": [
            {"name": "Object", "inherits": None, "methods": []},
            {"name": "Material", "inherits": "Object", "methods": []},
            {"name": "BaseMaterial3D", "inherits": "Material", "methods": []},
            {"name": "ShaderMaterial", "inherits": "Material", "methods": []},
            {
                "name": "Thing",
                "inherits": "Object",
                "methods": [
                    _method("set_material", None,
                            [{"name": "material", "type": "BaseMaterial3D,ShaderMaterial"}]),
                    # Godot's exclusion form: a Texture2D that is not one of these.
                    _method("set_texture", None,
                            [{"name": "texture", "type": "Material,-ShaderMaterial"}]),
                ],
            },
        ]
    }
    coverage = g.Coverage()
    blocks, order, _map, _members, _arrays, _dicts = g.generate(
        api, ["Thing", "BaseMaterial3D", "ShaderMaterial"], coverage, {})
    body = blocks[order.index("Thing")]
    # Optional because neither argument carries `"meta": "required"`, which is the next test's
    # subject rather than this one's -- what these two assert is the class the union widened to.
    check_true(
        "a union parameter widens to the class every member derives from",
        "SetMaterial<public>(Material:?material)" in body,
    )
    check_true(
        "and an exclusion is dropped rather than narrowing anything",
        "SetTexture<public>(Texture:?material)" in body,
    )
    check("neither is recorded as unsupported", coverage.skip_reasons["unsupported_type"], 0)


def test_an_object_parameter_is_optional_unless_godot_marks_it_required():
    """R-TYPE-4 over godotengine/godot#86079's metadata, which is the only thing that says.

    Godot's dump marks an object argument that may *not* be null and says nothing about one that
    may, so the absence is the signal and the mirror reads it the way Godot does. 112 of 1020 are
    marked, and they are the calls an author makes most -- which is why the common one is unchanged.
    """
    api = {
        "classes": [
            {"name": "Object", "inherits": None, "methods": []},
            {"name": "Node", "inherits": "Object", "methods": []},
            {
                "name": "Thing",
                "inherits": "Object",
                "methods": [
                    _method("adopt", None,
                            [{"name": "child", "type": "Node", "meta": "required"}]),
                    _method("watch", None, [{"name": "target", "type": "Node"}]),
                ],
            },
        ]
    }
    coverage = g.Coverage()
    blocks, order, _map, _members, _arrays, _dicts = g.generate(
        api, ["Thing", "Node"], coverage, {})
    body = blocks[order.index("Thing")]
    check_true(
        "a required object parameter keeps the class itself",
        "Adopt<public>(Child:node)" in body,
    )
    check_true(
        "and packs through the plain builder",
        "VhFromObject(Child)" in body,
    )
    check_true(
        "one Godot does not mark is optional, because null is a value it accepts",
        "Watch<public>(Target:?node)" in body,
    )
    check_true(
        "and packs through the one that has a Nil to answer with",
        "VhFromMaybeObject(Target)" in body,
    )


def test_a_raw_pointer_is_its_own_permitted_skip():
    api = {
        "classes": [
            {"name": "Object", "inherits": None, "methods": []},
            {
                "name": "Thing",
                "inherits": "Object",
                "methods": [_method("poke", None, [{"name": "p", "type": "const void*"}])],
            },
        ]
    }
    coverage = g.Coverage()
    g.generate(api, ["Thing"], coverage, {})
    check("a pointer parameter is skipped as a pointer", coverage.skip_reasons["unmarshallable_pointer"], 1)
    check("and not as an unsupported type", coverage.skip_reasons["unsupported_type"], 0)


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
    blocks, _emit_order, _method_map, _members, _arrays, _dicts = g.generate(api, ["Base", "Other"], coverage, {})
    other_block = next(b for b in blocks if b.startswith("other"))
    check_true(
        "unresolved class type falls back to nearest emitted ancestor (base)",
        "base[VhObjectFrom[" in other_block,
        other_block,
    )


def test_render_classes_header():
    api = {"header": {"version_full_name": "Godot Engine v4.6.stable.official"}}
    method_map = [("Timer", "timer", "start", "Start", False, "method")]
    doc_map = [("timer", "Timeout", "Timer", "timeout", "signal"),
               ("Vector2Statics", "Zero", "Vector2", "ZERO", "constant")]
    enums = {"Node.InternalMode": g.GodotEnum(
        key="Node.InternalMode", verse_name="node_internal_mode", is_bitfield=False,
        values=[("Disabled", 0)], stripped="INTERNAL_MODE_")}
    global_rows = [("LerpAngle", "@GlobalScope", "lerp_angle"), ("ToString", "Object", "to_string")]
    text = g.render_classes_header(api, ["Node2D", "Node"], method_map, doc_map, enums, global_rows)
    check_true("classes header has a #pragma once", text.startswith("#pragma once"))
    check_true("classes header opens verse_api namespace", "namespace verse_api {" in text)
    check_true(
        "classes header sorts entries by Godot class name (Node before Node2D)",
        text.index('{ "Node", "node" }') < text.index('{ "Node2D", "node2d" }'),
    )
    # The kind is the whole point of the table for the editor: a signal accessor and a static are
    # both Verse functions, so nothing on the Verse side could tell a consumer which page to open.
    check_true(
        "a class method carries member_kind::method",
        '{ "timer", "Start", "Timer", "start", false, member_kind::method },' in text,
        text,
    )
    check_true(
        "a signal accessor carries member_kind::signal",
        '{ "timer", "Timeout", "Timer", "timeout", false, member_kind::signal },' in text,
        text,
    )
    check_true(
        "a statics module's constant carries member_kind::constant",
        '{ "Vector2Statics", "Zero", "Vector2", "ZERO", false, member_kind::constant },' in text,
        text,
    )
    check_true(
        "an enum maps back to the Godot class and enum it was spelled from",
        '{ "node_internal_mode", "Node", "InternalMode" },' in text,
        text,
    )
    # A global carries the page it is documented on as well as the name, because one of them is not
    # on @GlobalScope: Godot's Object.to_string is what Verse's own `ToString` stands for.
    check_true(
        "a global names the page it is documented on",
        '{ "LerpAngle", "@GlobalScope", "lerp_angle" },' in text,
        text,
    )
    check_true(
        "and one documented on a class names the class",
        '{ "ToString", "Object", "to_string" },' in text,
        text,
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


def test_math_written_is_read_from_the_source(tmp_source=None):
    """G12: the record of what is missing is computed from GodotMath, not maintained beside it.

    The property that matters is the *negative* one -- a method that is written must not be recorded
    as absent -- because that is the one that rots. A list maintained by hand goes stale silently the
    first time someone adds a body.
    """
    source = textwrap.dedent("""\
        using { /Verse.org/Native }

        operator'+'<public>(L:vector2, R:vector2)<computes>:vector2 = vector2{}
        operator'*'<public>(S:float, R:vector2)<computes>:vector2 = vector2{}
        prefix'-'<public>(V:vector2)<computes>:vector2 = vector2{}
        (V:vector2).Length<public>()<reads>:float = 0.0
        """)
    with tempfile.TemporaryDirectory() as folder:
        path = Path(folder) / "GodotMath.native.verse"
        path.write_text(source, encoding="utf-8")
        methods, operators = g.read_math_written(path)

    check("the extension method is seen", sorted(methods["vector2"]), ["Length"])
    check_true("a binary operator is keyed on both operands", ("+", "vector2", "vector2") in operators)
    check_true("a prefix operator is keyed as unary", ("-", "vector2", None) in operators)
    # Godot files `float * Vector2` under Vector2, and the Verse overload that serves it is written
    # with the float on the left -- so it has to be found under either side or it reads as missing.
    check_true("a reversed-operand overload counts for the math type",
               ("*", "vector2", "float") in operators)

    api = {"builtin_classes": [{
        "name": "Vector2",
        "methods": [{"name": "length"}, {"name": "snapped"}],
        "operators": [
            {"name": "+", "right_type": "Vector2"},
            {"name": "unary-"},
            {"name": "/", "right_type": "float"},
        ],
    }]}
    coverage = g.Coverage()
    with tempfile.TemporaryDirectory() as folder:
        path = Path(folder) / "GodotMath.native.verse"
        path.write_text(source, encoding="utf-8")
        g.record_math_skips(api, coverage, path)

    skipped = {(s.verse_name, s.reason) for s in coverage.skipped_members}
    check_true("a method with no body is recorded", ("Snapped", "math_not_written") in skipped)
    check_true("a method that is written is not",
               not any(s.verse_name == "Length" for s in coverage.skipped_members))
    check_true("an operator with no overload is recorded",
               ("operator'/'", "math_operator_not_written") in skipped)
    check_true("an operator that is written is not",
               not any(s.verse_name == "operator'+'" for s in coverage.skipped_members))
    check_true("nor a unary one that is written",
               not any(s.godot_name == "unary-" for s in coverage.skipped_members))


def test_property_skips_have_reasons():
    resolver = g.TypeResolver({"Node"}, {"Node": "Object"}, {"Node", "Texture2D"}, {})
    coverage = g.Coverage()
    check_true(
        "a read-only property is skipped",
        g.classify_property({"name": "a", "type": "float", "getter": "get_a"}, resolver, coverage, {}) is None,
    )
    check_true(
        "a string property is skipped",
        g.classify_property(
            {"name": "b", "type": "String", "getter": "get_b", "setter": "set_b"}, resolver, coverage, {}
        ) is None,
    )
    check_true(
        "an object property is skipped",
        g.classify_property(
            {"name": "c", "type": "Node", "getter": "get_c", "setter": "set_c"}, resolver, coverage, {}
        ) is None,
    )
    check_true(
        "a property named after a Verse function keeps its value under another name",
        g.classify_property(
            {"name": "max", "type": "float", "getter": "get_max", "setter": "set_max"}, resolver, coverage, {}
        ) is not None,
    )
    check_true(
        "a float property is not",
        g.classify_property(
            {"name": "d", "type": "float", "getter": "get_d", "setter": "set_d"}, resolver, coverage, {}
        ) is not None,
    )
    check(
        "each skip recorded its own reason",
        sorted(coverage.skip_reasons),
        ["property_container_type", "property_no_accessor_pair", "property_object_type",
         "property_renamed"],
    )


def test_method_map_names_the_godot_original():
    api = {"header": {"version_full_name": "Godot Engine v4.6.stable.official"}}
    method_map = [
        ("Node2D", "node2d", "set_position", "SetPosition", False, "method"),
        ("Node2D", "node2d", "get_position", "GetPosition", False, "method"),
        ("Node2D", "node2d", "_draw", "_Draw", True, "method"),
    ]
    text = g.render_classes_header(api, ["Node2D"], method_map, [], {}, [])
    check_true(
        "method map carries the Godot spelling the Verse name cannot be inverted to",
        '{ "node2d", "GetPosition", "Node2D", "get_position", false, member_kind::method },' in text,
    )
    check_true(
        "method map sorts by Verse class then Verse method",
        text.index('"GetPosition"') < text.index('"SetPosition"'),
    )
    # The column the editor's override completion reads. A concrete method is a member the
    # compiler would take an <override> of and Godot would never dispatch to.
    check_true(
        "method map marks a virtual as one",
        '{ "node2d", "_Draw", "Node2D", "_draw", true, member_kind::method },' in text,
    )


def test_generated_method_map_covers_a_known_method():
    header = (REPO_ROOT / "src" / "verse_api_classes.h").read_text(encoding="utf-8")
    # A property lands in the same table as a method: the editor routes both to Godot's own docs,
    # and neither Verse name can be inverted back to the Godot one.
    check_true(
        "the checked-in header maps node2d.Position to Node2D.position",
        '{ "node2d", "Position", "Node2D", "position", false, member_kind::property },' in header,
    )
    check_true(
        "the checked-in header still maps a surviving method",
        '{ "node", "GetChild", "Node", "get_child", false, member_kind::method },' in header,
    )
    # A virtual is generated onto the class Godot declares it on, so its documentation is found the
    # same way every other member's is.
    check_true(
        "the checked-in header maps node._Ready to Node._ready",
        '{ "node", "_Ready", "Node", "_ready", true, member_kind::method },' in header,
    )
    # None of the five script-level hooks is in any part of extension_api.json, so all five are
    # hand-written on the native root -- and a row here is what makes the editor offer the override
    # at all. `completes_as_override` admits a member of `vh_object` only when the method map finds
    # it and the row says virtual, so a hook missing from this table is one nobody can complete and
    # nobody can hover. Four of them were missing for exactly that reason (by-hand-findings.md B1's
    # mechanism, rediscovered on `_Set`), which is why this checks the set rather than one row.
    for verse_name, godot_name in [
        ("_Notification", "_notification"),
        ("_Get", "_get"),
        ("_Set", "_set"),
        ("_GetPropertyList", "_get_property_list"),
        ("_ValidateProperty", "_validate_property"),
    ]:
        check_true(
            f"and vh_object.{verse_name} to Object.{godot_name}, marked virtual",
            f'{{ "vh_object", "{verse_name}", "Object", "{godot_name}", true, member_kind::method }},'
            in header,
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

    # A mirrored method on a *Godot class* may report exactly three absences, and all are real: a
    # null object return (R-TYPE-4 puts nullability in the type), a predicate answering no, and a
    # bool virtual's own declaration, whose unoverridden body declines because "not handled" is
    # what it has to mean. Anything else claiming failure is a method whose caller would have to
    # write an `if` around a case that never arrives.
    #
    # Two kinds of definition fail for their own real reasons and are excluded: the singleton
    # accessors, which fail when the name is not registered in this build and are told apart by
    # being at module scope; and a container's element accessors, where an absent key, an index out
    # of range and an element of another type are all genuine misses.
    failable = [
        line for line in text.splitlines()
        if "<decides>" in line and not line.lstrip().startswith("#") and "VhRefGet[" not in line
    ]
    def failable_shape_is_known(line):
        return ("VhObjectFrom[" in line
                or (":void = VhToLogic(" in line and line.rstrip().endswith("?"))
                or line.rstrip().endswith("<decides>:void = false?"))

    members = [line for line in failable if line.startswith("    ")]
    check_true(
        "every failable method on a mirrored Godot class returns an object, tests, or is a virtual",
        any(members) and all(failable_shape_is_known(line) for line in members),
        [line for line in members if not failable_shape_is_known(line)][:3],
    )
    check_true(
        "and a bool virtual carries <decides> and nothing else",
        not any("<decides><" in line or "><decides>" in line
                for line in members if line.rstrip().endswith("= false?")),
        [line for line in members
         if line.rstrip().endswith("= false?")
         and ("<decides><" in line or "><decides>" in line)][:3],
    )
    container_reads = [
        line for line in text.splitlines()
        if "<decides>" in line and "VhRefGet[" in line
    ]
    check_true(
        "a container's element accessors are the other failable methods",
        container_reads and all(line.lstrip().startswith("Get") for line in container_reads),
    )

    # At module scope there are three kinds of failable definition, and each fails for a reason a
    # caller has to handle: a singleton this build does not register -- the accessor's cast over the
    # bare vh_object the host answers for one -- a variant that is not the type being asked for, and
    # a null object.
    free = [line for line in failable if not line.startswith("    ")]
    unexplained = [
        line for line in free
        if "VhSingletonObject(" not in line
        and not line.startswith("(Value:variant).As")
        and not line.startswith("VhUnpack")
        and not line.startswith("VhToObject(")
        # A typed container's element converter, which fails on a null object like any other object
        # read. Module-scoped and never in a script's completion.
        and not (line.startswith("VhTo") and "Element(Value:variant)" in line)
    ]
    check("no failable free function fails for an unexplained reason", unexplained, [])
    check_true(
        "a mirrored singleton gets an accessor",
        'GetInput' in text and 'VhSingletonObject("Input")' in text,
    )
    check_true(
        "an accessor a mirrored method already names is the one that moves",
        'GetInputSingleton<public>()<reads>:input' in text
        and "\nGetInput<public>()" not in text,
    )

    # The two whose class is `"api_type": "editor"`, named here rather than counted, because what
    # the rule protects is that a script can still handle the one absence a game can really meet.
    # Every other accessor raises, and 4.7 has 41 of them.
    accessors = [line for line in text.splitlines() if "VhSingletonObject(" in line]
    check(
        "only the editor singletons are failable",
        sorted(line.split("<")[0] for line in accessors if "<decides>" in line),
        ["GetEditorInterfaceSingleton", "GetGDScriptLanguageProtocol"],
    )
    check_true(
        "and every other accessor raises rather than answering nothing",
        all("else Err(" in line for line in accessors if "<decides>" not in line),
    )

    # Every Variant::Type the mirror can read has a reader and a builder, and they are named after
    # Godot's own type rather than after the Verse one.
    readers = [line for line in free if line.startswith("(Value:variant).As")]
    check_true("a reader per variant lane, spelled on the receiver", len(readers) == len(g.VARIANT_LANES))
    check_true(
        "and a builder to match each",
        all(f"Variant{lane.reader}<public>(" in text for lane in g.VARIANT_LANES),
    )
    # The plain half is what a typed container's `Unpack` member names; an extension method is
    # receiver-plus-tuple and cannot be a function value. Non-public, so it is not API.
    check_true(
        "each reader forwarding to a plain function a container can name as a value",
        all(f"VhUnpack{lane.reader}(Value:variant)<decides><reads>:" in text
            for lane in g.VARIANT_LANES),
    )
    check_true(
        "the kind enum keeps Godot's TYPE_ prefix, because Int and Float are taken",
        "    TypeInt\n" in text and "    TypeFloat\n" in text and "    Int\n" not in text,
    )

    base_members = {"Handle", "_Notification"}

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


def test_enumerator_names_strip_their_shared_prefix():
    check(
        "the enumerators' own shared prefix comes off",
        g.enumerator_names(["PROCESS_MODE_INHERIT", "PROCESS_MODE_ALWAYS", "PROCESS_MODE_DISABLED"]),
        (["Inherit", "Always", "Disabled"], True),
    )
    check(
        "no shared prefix means no stripping",
        g.enumerator_names(["OK", "FAILED", "ERR_UNAVAILABLE"]),
        (["Ok", "Failed", "ErrUnavailable"], False),
    )
    check(
        "a stripped name that is not an identifier abandons stripping for the whole enum",
        g.enumerator_names(["SOURCE_TEXTURE", "SOURCE_2D_TEXTURE"]),
        (["SourceTexture", "Source2dTexture"], False),
    )
    check(
        "and so does one ambiguous with a Verse stdlib function",
        g.enumerator_names(["TYPE_NIL", "TYPE_INT", "TYPE_FLOAT"]),
        (["TypeNil", "TypeInt", "TypeFloat"], False),
    )
    check(
        "the last word is never consumed, so a one-word remainder survives",
        g.enumerator_names(["AXIS_X", "AXIS_Y", "AXIS_Z"]),
        (["X", "Y", "Z"], True),
    )


def test_enums_drop_sentinels_and_aliases():
    api = {
        "classes": [],
        "global_enums": [
            {
                "name": "Thing",
                "values": [
                    {"name": "THING_FIRST", "value": 0},
                    {"name": "THING_SECOND", "value": 1},
                    # An alias: a second name for a value another enumerator already has.
                    {"name": "THING_ALSO_SECOND", "value": 1},
                    # A sentinel, and the one thing Godot renumbers between releases.
                    {"name": "THING_MAX", "value": 2},
                ],
            }
        ],
    }
    enums = g.collect_enums(api)
    info = enums["Thing"]
    check("the sentinel and the alias are both gone", info.values, [("First", 0), ("Second", 1)])
    check("and the type is named after the enum", info.verse_name, "thing")

    emitted = "\n".join(g.emit_enums(enums))
    check_true("the enum is declared public", "thing<public> := enum:" in emitted)
    check_true("int -> enum reads the variant it was handed",
               "VhToThing(Value:variant)<reads>:thing" in emitted)
    check_true("a number no enumerator has raises rather than guessing",
               'VhTypeMismatch("thing", Value)' in emitted)
    check_true("enum -> int is the one public name", "ToInt<public>(Value:thing)<reads>:int" in emitted)
    check_true("and the packer goes through it",
               "VhFromThing(Value:thing)<reads>:variant = VhFromInt(ToInt(Value))" in emitted)


def test_a_property_takes_its_enum_from_the_getter():
    # Godot reports process_mode as an int and only get_process_mode says which enum it is, which is
    # true of 515 of its 994 int properties.
    check(
        "the getter's enum wins over the property's int",
        g.property_godot_type(
            {"name": "process_mode", "type": "int", "getter": "get_process_mode", "setter": "set_process_mode"},
            {"get_process_mode": {"return_value": {"type": "enum::Node.ProcessMode"}}},
        ),
        "enum::Node.ProcessMode",
    )
    check(
        "a bitfield does not, because a combination is not an enumerator",
        g.property_godot_type(
            {"name": "flags", "type": "int", "getter": "get_flags", "setter": "set_flags"},
            {"get_flags": {"return_value": {"type": "bitfield::Node.Flags"}}},
        ),
        "int",
    )
    check(
        "and a type the property states outright is left alone",
        g.property_godot_type({"name": "x", "type": "float", "getter": "get_x", "setter": "set_x"}, {}),
        "float",
    )


def test_a_member_ambiguous_with_a_verse_name():
    # Confirmed by the compiler rather than guessed: generating the whole API with no guard reports
    # exactly Min, Max and ToString. Length, Reverse, Sign and Shuffle are not among them, which an
    # earlier conservative guess had assumed they were.
    check("the confirmed set is small and closed",
          sorted(g.VERSE_AMBIGUOUS_MEMBER_NAMES), ["Max", "Min", "ToString"])

    resolver = g.TypeResolver({"Thing"}, {"Thing": None}, {"Thing"}, {})
    coverage = g.Coverage()
    renamed = g.classify_property(
        {"name": "max", "type": "float", "getter": "get_max", "setter": "set_max"},
        resolver, coverage, {}, "Thing")
    check_true("a property named Max is still a property", renamed is not None)
    check("under the name beside it in PROPERTY_RENAMES", renamed.verse_name, "Maximum")
    check("and the name it did not get is recorded",
          coverage.skip_reasons["property_renamed"], 1)
    check("pointing at the one it did", coverage.skipped_members[0].detail, "`Maximum`")
    check("keyed by the spelling an author would have tried",
          coverage.skipped_members[0].verse_name, "Max")

    # A method in that position has no rename rule any more -- there is none left to need one -- so
    # the generator refuses rather than letting the collision reach the compiler far from its cause.
    raised = False
    try:
        g.classify_method(_method("max", "float"), resolver, g.Coverage(), set(), "Thing")
    except ValueError:
        raised = True
    check_true("a *method* in that position fails generation instead", raised)


def test_to_string_is_reachable_as_verses_own():
    resolver = g.TypeResolver({"Object"}, {"Object": None}, {"Object"}, {})
    coverage = g.Coverage()
    check_true(
        "Object.to_string is not emitted as a method",
        g.classify_method(_method("to_string", "String"), resolver, coverage, set(), "Object") is None,
    )
    check("it is superseded by a free function",
          coverage.skip_reasons["superseded_by_free_function"], 1)
    check("and the reason names the spelling that replaced it",
          coverage.skipped_members[0].detail, "`ToString(Value)`")


# The three the type table leaves out on purpose: a script has no business writing any of them, so
# completion must not offer them, and the syntax highlighter names them itself.
PLUMBING_TYPES = {"vh_object", "vh_signal", "godot_ref"}


def test_every_exported_type_has_a_row():
    """A public type in the hand-written mirror that no table knows is invisible to the editor.

    This is the check `variant` needed and did not have. It is public -- typed_array's constructor
    forces it to be -- and a script writes it in `_Get` and `_Set`; the editor drew it as plain
    text and hovered it as a local constant for a phase, because the one list that carried it was
    dropped when it briefly stopped being public and nothing noticed when it came back.

    The generated file is not scanned: those names come from the generator's own tables and are
    checked above. These two are hand-written, so the list of them is hand-written too.
    """
    import re

    declared = set()
    for name in ("Godot.native.verse", "GodotApi.native.verse"):
        source = (REPO_ROOT / "host" / "Verse" / name).read_text(encoding="utf-8")
        # Specifiers and parameters arrive in either order and any number -- `variant<public>
        # <native>`, `signal<public>(t:type)` -- so they are matched as one run and read after.
        for name, decoration in re.findall(
                r"^(\w+)((?:<\w+>|\([^)]*\))*) := (?:class|struct|interface)\b", source, re.MULTILINE):
            if "<public>" in decoration:
                declared.add(name)

    check_true("the hand-written mirror declares public types at all", len(declared) > 5)
    check(
        "every public type in the hand-written mirror is a row or deliberate plumbing",
        declared - set(g.EXPORTED_TYPES) - PLUMBING_TYPES,
        set(),
    )
    # The wrappers are the rows that come from the generated file instead of from those two, so
    # they are named rather than scanned for -- and a new one has to arrive here as well, since the
    # editor would otherwise learn nothing about it either.
    check(
        "every reference wrapper has a row",
        {name for name, _ in g.REFERENCE_TYPES} - set(g.EXPORTED_TYPES),
        set(),
    )
    # The other direction: a row for a name nothing declares would colour a word that is not a
    # type.
    check(
        "and every row is a type something declares",
        set(g.EXPORTED_TYPES) - declared - {name for name, _ in g.REFERENCE_TYPES},
        set(),
    )


def test_the_value_types_are_all_of_them():
    """All sixteen and RID, which is what `vector2i` hovering into nothing was.

    Three of the sixteen were named here, so `vector2` reached Godot's documentation, appeared in
    the completion list and coloured as a type, and `vector2i` beside it did none of the three.
    """
    check("every math type is a value type row", set(g.VALUE_TYPE_CLASSES) - set(g.MATH_TYPES), {"RID"})
    check("and every math type has one", set(g.MATH_TYPES) - set(g.VALUE_TYPE_CLASSES), set())
    header = (REPO_ROOT / "src" / "verse_api_classes.h").read_text(encoding="utf-8")
    for godot_name, verse_name in (("Vector2i", "vector2i"), ("RID", "rid"), ("Transform3D", "transform3d")):
        check_true(f"the checked-in header maps {godot_name} to {verse_name}",
                   f'{{ "{godot_name}", "{verse_name}" }},' in header)
    # `variant` is not a Godot class and has no row in that table; the type table is where it is.
    check_true("and carries variant in the type table",
               '{ "variant", "Variant" },' in header)
    check_true("with no page for the declaration types Godot does not document",
               '{ "signal", "" },' in header)


def test_a_written_math_method_is_documented():
    """A method GodotMath writes gets a row; the same walk records a skip for one it does not.

    `(V:vector2).Length()` is `Vector2.length` and is documented as such, but an extension method is
    a module-level definition, so the compiler reports its owner as the file. Nothing keyed by owner
    could find it and all 159 hovered as local constants with their signature -- the consumer reads
    the receiver off the declared type, and this is the row it reads with.
    """
    header = (REPO_ROOT / "src" / "verse_api_classes.h").read_text(encoding="utf-8")
    for verse_class, verse_name, godot_class, godot_name, kind in (
            ("vector2", "Length", "Vector2", "length", "method"),
            ("vector2", "Snapped", "Vector2", "snapped", "method"),
            ("color", "Darkened", "Color", "darkened", "method"),
            # Godot's `end` is a member rather than a method, so the method walk cannot see it and
            # the member walk beside it is what gives `GetEnd` a page.
            ("rect2", "GetEnd", "Rect2", "end", "property"),
    ):
        check_true(
            f"the checked-in header maps {verse_class}.{verse_name} to {godot_class}.{godot_name}",
            f'{{ "{verse_class}", "{verse_name}", "{godot_class}", "{godot_name}", false, '
            f'member_kind::{kind} }},' in header,
        )
    # The two written methods Godot has no such method for. `Xform` is its `operator *` and
    # `InverseOrthonormal` is a helper of the mirror's own, so both keep their own comment.
    for verse_class, verse_name in (("quaternion", "Xform"), ("transform3d", "InverseOrthonormal")):
        check_true(f"and gives {verse_class}.{verse_name} no row, having nothing to point at",
                   f'{{ "{verse_class}", "{verse_name}", ' not in header)


def test_a_hand_written_global_is_documented():
    """The globals table, which was two rows in verse_script_language.cpp and is now derived.

    `Print` and `IsInstanceValid` had rows and the other thirty-eight globals the mirror hand-writes
    did not, so every one of GodotMath's scalar functions drew "Local Constant".
    """
    import json

    api = json.loads((REPO_ROOT / "godot-cpp" / "gdextension" / "extension_api-4-7.json")
                     .read_text(encoding="utf-8"))
    rows = dict((name, (godot_class, godot_function))
                for name, godot_class, godot_function in g.global_doc_rows(api, REPO_ROOT / "host" / "Verse"))

    check("a global named exactly as Godot names it", rows.get("LerpAngle"),
          ("@GlobalScope", "lerp_angle"))
    # Godot spells eight of them without the word boundary the mirror keeps.
    check("one Godot spells without the boundary", rows.get("WrapF"), ("@GlobalScope", "wrapf"))
    # And one that shares no letters with the Godot name, which only UTILITY_VERSE_SPELLINGS knows.
    check("one that only the spellings table can reach", rows.get("VariantTypeName"),
          ("@GlobalScope", "type_string"))
    # `Object.to_string` is the one global documented on a class rather than on @GlobalScope.
    check("and ToString, which is a class method", rows.get("ToString"), ("Object", "to_string"))

    check_true("a function the bridge invented gets no row",
               not any(name in rows for name in ("MakeVariant", "TruncatedQuotient", "MakeSignal")))
    check_true("and an extension method is not a global",
               "Length" not in rows and "Normalized" not in rows)


def test_fmod_keeps_its_own_spelling():
    """`fmod` was bound twice in UTILITY_VERSE_SPELLINGS and the second binding won.

    Both sentences were true, which is why it read as correct: Verse's `Mod[X, Y]` exists and so
    does GodotMath's `FMod(A, B)`. Only one of them is the float modulo Godot's `fmod` is, and the
    editor was naming the other.
    """
    check("fmod is spelled FMod", g.UTILITY_VERSE_SPELLINGS["fmod"], "FMod(A, B)")
    check("posmod keeps Verse's own", g.UTILITY_VERSE_SPELLINGS["posmod"], "Mod[X, Y]")


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
    test_a_member_ambiguous_with_a_verse_name()
    test_to_string_is_reachable_as_verses_own()
    test_every_exported_type_has_a_row()
    test_the_value_types_are_all_of_them()
    test_a_written_math_method_is_documented()
    test_a_hand_written_global_is_documented()
    test_fmod_keeps_its_own_spelling()
    test_enumerator_names_strip_their_shared_prefix()
    test_enums_drop_sentinels_and_aliases()
    test_a_property_takes_its_enum_from_the_getter()
    test_singleton_accessors_cover_only_emitted_classes()
    test_singleton_accessor_yields_to_a_method_of_the_same_name()
    test_shadow_suppression_across_inheritance()
    test_base_member_shadow()
    test_get_node_takes_the_or_null_spelling()
    test_a_predicate_decides_and_an_accessor_answers_logic()
    test_a_bool_virtual_decides_with_nothing_but_decides()
    test_virtual_names_keep_godots_underscore()
    test_virtual_emits_a_default_body()
    test_unsupported_type_skipping()
    test_typed_array_parameter_takes_the_parametric_class()
    test_union_parameter_widens_to_the_common_ancestor()
    test_a_raw_pointer_is_its_own_permitted_skip()
    test_class_type_falls_back_to_nearest_emitted_ancestor()
    test_render_classes_header()
    test_emit_scalar_property()
    test_emit_indexed_property_passes_its_index()
    test_struct_property_gets_the_field_overloads()
    test_accessor_locals_dodge_a_colliding_member()
    test_math_written_is_read_from_the_source()
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
