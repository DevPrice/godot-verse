extends SceneTree

# The integration layer R-QUAL-1 asks for: a headless Godot with Verse scripts attached, asserting
# on behaviour rather than on the ABI's own return codes. The unit and ABI layers already cover
# those; what only this layer can see is whether Godot's own dispatch reaches Verse at all.
#
# Shaped like the other suites in this repo: one line per case, non-zero exit on failure, no
# framework. Run through tools/run_tests.py.

var _passed := 0
var _failed := 0


func _check(name: String, ok: bool) -> void:
	if ok:
		_passed += 1
		print("[integration] %s: ok" % name)
	else:
		_failed += 1
		print("[integration] %s: FAIL" % name)


func _check_eq(name: String, got: Variant, expected: Variant) -> void:
	var ok: bool = typeof(got) == typeof(expected) and got == expected
	if not ok:
		print("[integration] %s: FAIL (got %s %s, expected %s %s)" % [
			name, type_string(typeof(got)), str(got),
			type_string(typeof(expected)), str(expected)])
		_failed += 1
	else:
		_passed += 1
		print("[integration] %s: ok" % name)


func _init() -> void:
	var script: Script = load("res://scripts/marshal.verse")
	if script == null:
		print("[integration] load marshal.verse: FAIL (nothing loaded)")
		quit(1)
		return
	_check("load marshal.verse", true)
	_check("the script compiled", script.can_instantiate())

	var node := Node2D.new()
	node.set_script(script)
	root.add_child(node)
	_check("a Verse script attaches to a node", node.get_script() == script)

	# --- R-NODE-6: Godot calls a Verse method, with arguments and a return value ---------------
	#
	# node.call() is the same path GDScript's `node.EchoInt(7)` takes, and the same one the engine
	# itself uses; going through it rather than through a direct binding is the whole point of this
	# layer.
	_check_eq("an int argument crosses and comes back", node.call("EchoInt", 7), 7)
	_check_eq("a large int keeps its width", node.call("EchoInt", 9007199254740993), 9007199254740993)
	_check_eq("a negative int", node.call("EchoInt", -12345), -12345)
	_check_eq("a float", node.call("EchoFloat", 0.5), 0.5)
	_check_eq("a string", node.call("EchoString", "hello"), "hello")
	_check_eq("a non-ascii string", node.call("EchoString", "héllo ✓"), "héllo ✓")
	_check_eq("an empty string", node.call("EchoString", ""), "")
	_check_eq("logic true", node.call("EchoLogic", true), true)
	_check_eq("logic false", node.call("EchoLogic", false), false)

	_check_eq("two int arguments arrive in order", node.call("AddInts", 3, 4), 3000004)
	_check_eq("two float arguments", node.call("AddFloats", 3.0, 4.0), 0.75)
	_check_eq("a string's bytes arrive, not a pointer", node.call("StringLength", "abcde"), 5)
	_check_eq("and its content", node.call("Concat", "a", "b"), "a|b")
	_check_eq("a method with no arguments", node.call("NoArgs"), 42)

	# --- R-TYPE-1: the math types ------------------------------------------------------------
	#
	# Each method permutes its components, so a lane written to the wrong slot is a wrong value
	# rather than a value that round-tripped without being looked at.
	_check_eq("a Vector2", node.call("EchoVector2", Vector2(1, 2)), Vector2(2, 1))
	_check_eq("a Vector2i keeps integer width", node.call("EchoVector2i", Vector2i(2147483647, -5)),
			Vector2i(-5, 2147483647))
	_check_eq("a Vector3", node.call("EchoVector3", Vector3(1, 2, 3)), Vector3(3, 1, 2))
	_check_eq("a Vector4", node.call("EchoVector4", Vector4(1, 2, 3, 4)), Vector4(4, 3, 2, 1))
	_check_eq("a Color", node.call("EchoColor", Color(0.1, 0.2, 0.3, 0.4)), Color(0.4, 0.3, 0.2, 0.1))
	_check_eq("a Quaternion", node.call("EchoQuaternion", Quaternion(0, 0, 0, 1)), Quaternion(0, 0, 0, 1))

	# Nested: every component has to land in its own lane and be read back out of it.
	_check_eq("a nested Rect2 reaches every component",
			node.call("Rect2Sum", Rect2(1, 2, 3, 4)), 1.0 + 20.0 + 300.0 + 4000.0)
	_check_eq("and a twice-nested Transform3D",
			node.call("Transform3DSum", Transform3D(Basis(), Vector3(7, 0, 0))),
			1.0 + 10.0 + 100.0 + 7000.0)

	# Godot's Basis.x is column 0. We used to serialise rows and rebuild from columns, so every
	# basis that crossed came back transposed; this is the assertion that says otherwise.
	var basis := Basis(Vector3(1, 2, 3), Vector3(4, 5, 6), Vector3(7, 8, 9))
	_check_eq("a Basis crosses untransposed", node.call("BasisColumnX", basis), basis.x)

	# --- the call reaches this instance, not the class default --------------------------------
	_check_eq("a method's side effect is visible on the instance", node.call("Bump", 5), 5)
	_check_eq("and accumulates across calls", node.call("Bump", 3), 8)
	_check_eq("and the exported member agrees", node.get("Counter"), 8)

	# --- <decides> ----------------------------------------------------------------------------
	_check_eq("a <decides> method that succeeds returns its value", node.call("Positive", 3), 3)
	_check_eq("and one that declines is nil, not an error", node.call("Positive", -3), null)

	# --- R-NODE-9: the method list reports what the script declares ----------------------------
	var names := {}
	for method in node.get_method_list():
		names[method["name"]] = method
	_check("get_method_list reports a declared method", names.has("EchoInt"))
	_check("has_method agrees", node.has_method("EchoInt"))
	_check("and denies one the script does not declare", not node.has_method("NoSuchMethod"))
	if names.has("AddInts"):
		var info: Dictionary = names["AddInts"]
		_check_eq("a method reports its argument count", info["args"].size(), 2)
		_check_eq("and its argument names", info["args"][0]["name"], "A")
		_check_eq("and its return type", info["return"]["type"], TYPE_INT)
	else:
		_check("a method reports its arguments", false)

	# A Godot virtual is listed under Godot's own name, which is how the engine finds it.
	_check("an overridden Godot virtual is listed under Godot's name", names.has("_ready"))
	_check("and the engine can call it", node.has_method("_ready"))

	print("[integration] %d passed, %d failed" % [_passed, _failed])
	quit(1 if _failed > 0 else 0)
