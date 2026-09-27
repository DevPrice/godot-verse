extends SceneTree

# docs/architecture-review.md item 4 step 3: a headless Godot project that asserts the Godot facts
# the bridge relies on, run on every api_version bump. This project loads no GDExtension -- no
# addons/, no extension_list.cfg -- so it never touches verse_host.dll and needs no host token,
# UE checkout or build. tools/run_godot_contract.py runs this file with:
#
#     godot --headless --path tests/godot_contract --script res://test_main.gd
#
# and adds the two facts only a Godot *source* checkout can answer (the real ProfilingInfo struct's
# field count, and the dictionary key lookup_code() reads for a script path). Everything below is
# what GDScript alone can observe.

var _passed := 0
var _failed := 0
var _skipped := 0


func _init() -> void:
	_check_booleanize_empty_variant()
	_check_singleton_driver_classes()
	_check_cyclic_resource_load()
	print("[godot_contract] %d passed, %d failed, %d skipped" % [_passed, _failed, _skipped])
	quit(1 if _failed > 0 else 0)


func _ok(name: String) -> void:
	_passed += 1
	print("[godot_contract] %s: ok" % name)


func _fail(name: String, detail: String = "") -> void:
	_failed += 1
	if detail.is_empty():
		print("[godot_contract] %s: FAIL" % name)
	else:
		print("[godot_contract] %s: FAIL (%s)" % [name, detail])


func _skip(name: String, why: String) -> void:
	_skipped += 1
	print("[godot_contract] %s: skip -- %s" % [name, why])


func _check(name: String, ok: bool, detail: String = "") -> void:
	if ok:
		_ok(name)
	else:
		_fail(name, detail)


func _check_eq(name: String, got: Variant, expected: Variant) -> void:
	if got == expected:
		_ok(name)
	else:
		_fail(name, "got %s, expected %s" % [str(got), str(expected)])


# CLAUDE.md, "The status is the whole answer for one of these, in both directions": Godot reads a
# script virtual's `<decides>:void` result through `Variant::booleanize()` -- `!is_zero()` -- so an
# empty Variant reads as false whether the Verse side declined or never ran at all. `if v:` compiles
# to the same booleanize() GDScript's own conditional-jump opcode uses, so it is the closest a
# script gets to observing `Variant::booleanize(Variant())` directly.
func _check_booleanize_empty_variant() -> void:
	var v: Variant = null
	var truthy := false
	if v:
		truthy = true
	_check_eq("booleanize_empty_variant_is_false", truthy, false)


# CLAUDE.md, "Object::get_class() can answer a class extension_api.json has never heard of": a
# GDCLASS driver's get_class() answers the driver's own name (IPWindows, GodotNavigationServer2D),
# which GodotClassNames.gen.h's exact-name lookup misses -- and by-hand-findings.md B21 is why this
# is the *IP* singleton specifically: godot-verse's own instance-binding callback is what crashed at
# shutdown, not IP itself, and this project never installs one, so calling it here has nothing to
# crash. A GDSOFTCLASS driver (DisplayServer's platform implementation) answers the registered base
# name instead, which is why CLAUDE.md says it "needs nothing" -- and that answer is the same
# headless as windowed, because GDSOFTCLASS leaves get_class() to the nearest GDCLASS ancestor
# regardless of which concrete driver was actually constructed.
func _check_singleton_driver_classes() -> void:
	if OS.get_name() != "Windows":
		# CLAUDE.md CI note: only Windows x86_64 and web are supported and runtime-tested: these
		# two driver class names are Windows-specific measurements.
		_skip("singleton_class_ip", "measured on Windows only; this run is on " + OS.get_name())
		_skip("singleton_class_navigation_server_2d", "measured on Windows only; this run is on " + OS.get_name())
	else:
		_check_eq("singleton_class_ip", IP.get_class(), "IPWindows")
		_check_eq("singleton_class_navigation_server_2d", NavigationServer2D.get_class(), "GodotNavigationServer2D")
	_check_eq("singleton_class_display_server_is_gdsoftclass", DisplayServer.get_class(), "DisplayServer")


# by-hand-findings.md B30: a cyclic resource load answers ERR_BUSY internally and a null Ref
# externally, silently -- the only thing that reaches the log is the caller's own generic sentence,
# which names the resource *asked for* and never the one it collided with. res_a.gd/res_b.gd and
# a.tres/b.tres (cyclic/) reproduce the shape without Verse: a.tres's ext_resource needs b.tres,
# which needs a.tres back, on the same thread's stack -- the same cyclic-load detection in
# core/io/resource_loader.cpp that a `.verse` load's binding generator hits when a GDScript's text
# names a Verse class. tools/run_godot_contract.py additionally asserts the exact sentence
# ("Error loading resource: 'res://cyclic/a.tres'.") reached this run's own output, since a script
# cannot read what only reaches Godot's log.
func _check_cyclic_resource_load() -> void:
	var loaded = ResourceLoader.load("res://cyclic/a.tres")
	_check("cyclic_resource_load_answers_null", loaded == null,
		"got %s, expected null" % str(loaded))
