extends SceneTree

# The integration layer R-QUAL-1 asks for: a headless Godot with Verse scripts attached, asserting
# on behaviour rather than on the ABI's own return codes. The unit and ABI layers already cover
# those; what only this layer can see is whether Godot's own dispatch reaches Verse at all.
#
# Shaped like the other suites in this repo: one line per case, non-zero exit on failure, no
# framework. Run through tools/run_tests.py.

var _passed := 0
var _failed := 0
var _thread_answer: Variant = null
var _signal_hits := 0
var _signal_points := 0
var _signal_by := ""
var _signal_object: Object = null
var _signal_report: Array = []
var _tx: Node2D = null
var _tx_step := 0
var _conc: Node2D = null
var _conc2: Node2D = null
var _foreign: Object = null
var _hit: Control = null


func _on_verse_touched(body: Node2D) -> void:
	_signal_object = body


# Three parameters against a one-struct payload: the struct decomposed on the way out, which is
# what puts `damage`/`by`/`point` in the connect dialog instead of a single unnamed value.
func _on_verse_reported(damage: int, by: String, point: Vector2) -> void:
	_signal_report = [damage, by, point]


func _on_verse_hit() -> void:
	_signal_hits += 1


func _on_verse_scored(points: int) -> void:
	_signal_points = points


func _on_verse_struck(damage: int, by: String) -> void:
	_signal_points = damage
	_signal_by = by



# Runs on a WorkerThreadPool thread. `call` on a Verse-scripted node from here must come back
# without having entered the VM.
func _call_verse_off_thread(target: Node) -> void:
	_thread_answer = "ran"
	_thread_answer = target.call("EchoInt", 7)


func _double(n: int) -> int:
	return n * 2


# Rewrites a script on disk, rebuilds, reloads, and checks the attached node answers the new code.
#
# Restores the file whatever happens: it is a checked-in fixture, and a test that leaves the tree
# dirty when it fails is worse than the failure.
func _check_reload_replaces_the_instance() -> void:
	const PATH := "res://scripts/reload_probe.verse"
	var original := FileAccess.get_file_as_string(PATH)
	if original.is_empty():
		_check("reload_probe.verse is readable", false)
		return

	var script: Script = load(PATH)
	var node := Node2D.new()
	node.set_script(script)
	root.add_child(node)
	_check_eq("the attached node answers the code it was built against", node.call("Answer"), 1)
	node.set("Tag", 5)

	var edited := original.replace("Answer<public>()<transacts>:int = 1",
			"Answer<public>()<transacts>:int = 2")
	var out := FileAccess.open(PATH, FileAccess.WRITE)
	out.store_string(edited)
	out.close()

	# The edit is only *code* after a build: a generation is what carries a body, and reload alone
	# refreshes analysis. Both are needed, in this order.
	var runtime := Engine.get_singleton("VerseRuntime")
	_check_eq("the edited project builds", runtime.call("build_project"), OK)
	script.reload()

	_check_eq("the attached node answers the reloaded code", node.call("Answer"), 2)
	_check_eq("and its exported value survived the instance being replaced", node.get("Tag"), 5)

	var restore := FileAccess.open(PATH, FileAccess.WRITE)
	restore.store_string(original)
	restore.close()
	runtime.call("build_project")
	script.reload()

	root.remove_child(node)
	node.free()


# A left click at a viewport position, pressed and released. Godot's own picking runs off this the
# same way it runs off a real mouse -- the dummy display server is not in the path.
func _click_at(at: Vector2) -> void:
	for pressed in [true, false]:
		var ev := InputEventMouseButton.new()
		ev.button_index = MOUSE_BUTTON_LEFT
		ev.pressed = pressed
		ev.position = at
		Input.parse_input_event(ev)


# Sprite2D is a Node2D, but `call` needs the argument typed as what the Verse parameter declares;
# this is only here to say that plainly at the call site.
func sprite2d_as_node2d(s: Sprite2D) -> Node2D:
	return s


func _check(name: String, ok: bool) -> void:
	if ok:
		_passed += 1
		print("[integration] %s: ok" % name)
	else:
		_failed += 1
		print("[integration] %s: FAIL" % name)


# For a *computed* float, where exact equality is the wrong question: Verse's float is 64-bit and
# Godot's real_t is 32-bit in a standard build, so anything that accumulates -- a quaternion product,
# a slerp -- differs in the last bits however right both sides are. An exact comparison still belongs
# on values that are exact, like a floor or a snap, and those keep using _check_eq.
func _check_close(name: String, got: float, expected: float, tol: float = 1e-6) -> void:
	if absf(got - expected) <= tol:
		_passed += 1
		print("[integration] %s: ok" % name)
	else:
		_failed += 1
		print("[integration] %s: FAIL (got %s, expected %s)" % [name, str(got), str(expected)])


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

	# --- R-TYPE-1/2/3: the reference types ----------------------------------------------------
	#
	# The point of these is the last two: Godot's Array and Dictionary are references, and a Verse
	# script mutating one has to be mutating the caller's. A copy would pass every other assertion
	# here and fail those.
	var items: Array = [10, 20, 30]
	_check_eq("an Array's length crosses", node.call("ArrayLength", items), 3)
	_check_eq("and its elements", node.call("ArrayFirstInt", items), 10)

	node.call("ArrayAppendTo", items, 1, 99)
	_check_eq("a write through an Array reaches the caller's", items[1], 99)

	var data := {"hp": 7, "mp": 3}
	_check_eq("a Dictionary lookup", node.call("DictLookup", data, "hp"), 7)
	_check_eq("a missing key is nil, not an error", node.call("DictLookup", data, "nope"), null)
	_check_eq("a Dictionary's size", node.call("DictLength", data), 2)

	node.call("DictWrite", data, "hp", 42)
	_check_eq("a write through a Dictionary reaches the caller's", data["hp"], 42)

	# R-TYPE-3: a Callable held, invoked, and its result returned.
	#
	# A bound method, not a lambda. Invoking a GDScript *lambda* from Verse segfaults Godot during
	# shutdown -- see spec R-TYPE-3 for what is known about it. Holding one and handing it back is
	# fine; only calling one is not, so this covers the case `connect` and every callback-taking
	# engine API actually use.
	var doubler := Callable(self, "_double")
	_check_eq("a Callable is invocable from Verse", node.call("CallIt", doubler, 21), 42)

	# --- packed arrays ------------------------------------------------------------------------
	#
	# A Verse `[]float` is equally a PackedFloat32Array, a PackedFloat64Array and an Array, so a
	# script method taking one declares the general type. Godot builds an Array from any packed
	# array; it will not convert between them, which is why this is the direction that works.
	_check_eq("a float array arrives as a Verse array",
			node.call("SumFloats", [1.0, 2.0, 4.0]), 7.0)
	_check_eq("and a string array",
			node.call("JoinStrings", ["a", "b"]), "a/b/")
	var made: Variant = node.call("MakeFloats", 3)
	_check("a Verse array comes back as a sequence Godot can read", made != null and made.size() == 3)
	_check_eq("with its values", float(made[2]), 2.0)

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

	# A Godot virtual is listed under Godot's own name, which is how the engine finds it. Asked of
	# the *script's* list rather than the node's: a node's merges ClassDB's, where `_ready` is
	# registered whatever the script says, so the node's list can never tell the two apart.
	var script_methods := []
	for method in (script as Script).get_script_method_list():
		script_methods.append(String(method["name"]))
	_check("an overridden Godot virtual is listed under Godot's own name",
			script_methods.has("_ready"))
	_check("and the engine can call it", node.has_method("_ready"))

	# --- R-TYPE-7 amended: a script can name a Variant and read it -----------------------------
	#
	# The 231 Variant-typed methods of the mirror were the largest single bucket of unreachable API.
	# What a script says about one is VariantKind to branch and As<GodotType> to ask.
	var mixed: Array = [7, 1.5, "hi", Vector2(3.0, 4.0), null]
	_check_eq("VariantKind reports an int element", node.call("KindAt", mixed, 0), "int")
	_check_eq("a float element", node.call("KindAt", mixed, 1), "float")
	_check_eq("a string element", node.call("KindAt", mixed, 2), "string")
	_check_eq("a vector2 element", node.call("KindAt", mixed, 3), "vector2")
	_check_eq("and a nil element", node.call("KindAt", mixed, 4), "nil")
	_check_eq("an index past the end is a miss", node.call("KindAt", mixed, 99), "missing")

	_check_eq("AsInt reads the int back", node.call("IntAt", mixed, 0), 7)
	_check_eq("AsVector2 reads a component", node.call("Vector2XAt", mixed, 3), 3.0)
	_check_eq("asking the wrong type declines rather than erroring",
			node.call("StringAtFails", mixed, 0), null)
	_check_eq("and the right one answers", node.call("StringAtFails", mixed, 2), "hi")

	# The Array is a reference, so what Verse wrote is what GDScript sees.
	node.call("PutInt", mixed, 0, 99)
	_check_eq("VariantFromInt writes through the reference", mixed[0], 99)

	# --- R-TYPE-1: PackedVector2Array ----------------------------------------------------------
	var points := PackedVector2Array([Vector2(1.0, 0.0), Vector2(2.0, 0.0), Vector2(4.0, 0.0)])
	_check_eq("a packed array of structs crosses element by element",
			node.call("SumVector2Xs", points), 7.0)
	var made_points: Variant = node.call("MakeVector2s", 3)
	_check_eq("and comes back as one", typeof(made_points), TYPE_PACKED_VECTOR2_ARRAY)
	if typeof(made_points) == TYPE_PACKED_VECTOR2_ARRAY:
		_check_eq("with every element", made_points.size(), 3)
		if made_points.size() == 3:
			_check_eq("with its elements intact", made_points[2], Vector2(2.0, 4.0))

	# --- the reference path for a packed array -------------------------------------------------
	#
	# A packed array crossing to a mirrored Godot method goes through the reference table, which the
	# script-return tests above never touch. Marshalls round-trips bytes with no scene involved.
	_check_eq("a Verse []int reaches a Godot PackedByteArray parameter",
			node.call("Base64Of", [72, 105]), "SGk=")
	var raw: Variant = node.call("BytesOfBase64", "SGk=")
	_check_eq("and comes back the same way", Array(raw), [72, 105])

	# --- R-TYPE-2, R-SCN-1: objects in a container ----------------------------------------------
	var kid := Node2D.new()
	kid.name = "Kid"
	node.add_child(kid)
	var kid2 := Node2D.new()
	kid2.name = "Sibling"
	node.add_child(kid2)

	_check_eq("GetChildren reports how many there are", node.call("ChildCount"), 2)
	_check_eq("an element is an object a method can be called on",
			node.call("FirstChildName"), "Kid")
	_check_eq("and every element is", node.call("ChildNames"), "Kid/Sibling/")
	_check_eq("an index past the end is a miss, not an error", node.call("HasChildAt", 99), null)

	# --- R-SCN-5: Godot's enums ------------------------------------------------------------------
	_check_eq("an enum-typed property reads as its enumerator", node.call("ProcessModeName"), "inherit")
	node.call("SetAlways")
	_check_eq("and writing one reaches Godot", node.process_mode, Node.PROCESS_MODE_ALWAYS)
	_check_eq("it still reads back as the enumerator", node.call("ProcessModeName"), "always")
	_check_eq("and its integer is reachable for a bitfield",
			node.call("ProcessModeInt"), Node.PROCESS_MODE_ALWAYS)
	# --- R-EXP-1: an exported Godot enum is a dropdown -------------------------------------------
	var exported := {}
	for entry in node.get_property_list():
		exported[entry["name"]] = entry
	if exported.has("Mode"):
		var info: Dictionary = exported["Mode"]
		_check_eq("an exported enum is an int to Godot", info["type"], TYPE_INT)
		_check_eq("with the enum hint", info["hint"], PROPERTY_HINT_ENUM)
		_check_eq("and every enumerator in the dropdown", info["hint_string"],
				"Inherit,Pausable,WhenPaused,Always,Disabled")
	else:
		_check("an exported enum reaches the inspector", false)
	_check_eq("its declared default is the ordinal of the enumerator", node.get("Mode"), 1)
	_check_eq("and the script reads it back as that enumerator", node.call("ModeName"), "pausable")
	node.set("Mode", 3)
	_check_eq("picking another one reaches the script", node.call("ModeName"), "always")

	_check_eq("flags combine with Verse's own BitOr",
			node.call("CtrlAndShift"), KEY_MASK_CTRL | KEY_MASK_SHIFT)

	# --- R-INT-2: Verse calls a GDScript method by name ------------------------------------------
	#
	# Object.callv is what delivers it, and Object is only mirrorable now that Variant and Array both
	# cross. The receiver here is this SceneTree, whose _double is defined in GDScript above.
	_check_eq("a method declared on the mirrored class it was passed as",
			node.call("NodeNameOf", kid), "Kid")
	_check_eq("a method inherited from Object", node.call("NodeClassOf", kid), "Node2D")
	_check_eq("Object.get_class answers Godot's own class name",
			node.call("ClassOf", kid), "Node2D")
	_check_eq("a Verse script calls a GDScript method dynamically",
			node.call("CallSibling", self, "_double", [21]), 42)
	_check("Object.has_method sees a GDScript method", node.call("CanCall", self, "_double"))

	# --- R-TYPE-2 / R-INT-2: a container the script built itself --------------------------------
	#
	# Until now a script could only hold a container Godot handed it. `godot_array{}` compiles and
	# holds reference 0, which crosses as Nil, so the one method that takes an argument list could
	# never be given one from Verse.
	_check_eq("a script calls a GDScript method with an argument list it built itself",
			node.call("CallSiblingWith", self, "_double", 16), 32)
	var built: Variant = node.call("MakeMixed", 3, "tail")
	_check_eq("an Array built in Verse crosses back as an Array", typeof(built), TYPE_ARRAY)
	if typeof(built) == TYPE_ARRAY:
		_check_eq("with the elements it was given, in order", built, [0, 1, 2, "tail"])
	var lookup: Variant = node.call("MakeLookup", "hp", 7)
	_check_eq("a Dictionary built in Verse crosses back as a Dictionary", typeof(lookup), TYPE_DICTIONARY)
	if typeof(lookup) == TYPE_DICTIONARY:
		_check_eq("with the key it was given", lookup.get("hp"), 7)
	_check_eq("a typed Array built in Verse holds what was added to it",
			node.call("MakeChildListLength", node, node), 2)
	_check_eq("and its elements come back at their own type",
			node.call("MakeChildListFirstName", node, node), String(node.name))
	_check("and denies one nothing declares", not node.call("CanCall", self, "_no_such_method"))

	# --- Object.to_string reached as Verse's own ToString -----------------------------------------
	_check_eq("ToString on a Godot object answers what Godot's to_string does",
			node.call("Described", kid), str(kid))
	_check_eq("and string interpolation desugars to the same function",
			node.call("Interpolated", kid), "<%s>" % str(kid))

	# --- R-LANG-1/2/3: the Verse language itself -------------------------------------------------
	#
	# Script-to-script inheritance, interfaces, structs, enums and parametric types. Most of this was
	# expected to work and none of it was verified, which is the whole reason for these cases.
	var derived_script: Script = load("res://scripts/derived_entity.verse")
	_check("a script that extends another script loads", derived_script != null)
	if derived_script != null:
		var entity := Node2D.new()
		entity.set_script(derived_script)
		root.add_child(entity)

		# R-LANG-1: the derived class overrides a method its *base script* declared.
		_check_eq("an override of another script's method wins", entity.call("Describe"), "derived")
		# And the base's own code still runs, reading the base's own exported member.
		_check_eq("the base class' method runs on the derived instance",
				entity.call("BaseDoubled"), 200)
		_check_eq("and an exported member declared by the base is reachable",
				entity.get("Health"), 100)
		entity.set("Health", 7)
		_check_eq("writing it reaches the base's member", entity.call("BaseDoubled"), 14)

		# R-LANG-1: an interface, implemented and dispatched through.
		_check_eq("an interface method is implemented", entity.call("Name"), "derived_entity")
		_check_eq("and dispatches when called through the interface type",
				entity.call("NameThroughInterface"), "derived_entity")

		# R-LANG-2: structs in user code, including a nested one.
		_check_eq("a user struct nests and reads back", entity.call("TotalAttack", 3, 4), 7)

		# R-LANG-2: an enum in user code, exported as a dropdown.
		_check_eq("a user enum's declared default", entity.call("StanceName"), "neutral")
		entity.set("Stance", 2)
		_check_eq("and picking another value reaches the script", entity.call("StanceName"), "aggressive")
		var entity_props := {}
		for entry in entity.get_property_list():
			entity_props[entry["name"]] = entry
		if entity_props.has("Stance"):
			_check_eq("a user enum exports as a dropdown too",
					entity_props["Stance"]["hint"], PROPERTY_HINT_ENUM)
			_check_eq("with its own enumerators",
					entity_props["Stance"]["hint_string"], "Defensive,Neutral,Aggressive")
		else:
			_check("a user enum reaches the inspector", false)

		# R-LANG-3: a parametric class instantiated at two types, read through a generic function.
		_check_eq("a parametric class carries an int", entity.call("BoxedInt", 5), 5)
		_check_eq("and a string, in the same script", entity.call("BoxedString", "hi"), "hi")

		# The base is a script in its own right, not only something to derive from.
		var base_script: Script = load("res://scripts/base_entity.verse")
		_check("the base script is attachable on its own", base_script != null and base_script.can_instantiate())

	# --- R-LANG-6: library files ---------------------------------------------------------------
	#
	# helpers.verse declares no class. It is a Script and it compiles, its module-level functions
	# are reachable from marshal.verse with nothing written to import them, and it cannot go on a
	# node -- which Godot decides from the empty instance base type rather than from a broken
	# script.
	_check_eq("a library file's function is reachable from another file", node.call("CallHelper", 21), 42)
	_check_eq("and its module-level constant", node.call("ReadHelperConstant"), 42)
	_check_eq("and a string-returning one", node.call("CallHelperString", "verse"), "hello, verse")

	var library: Script = load("res://scripts/helpers.verse")
	_check("a .verse with no class of its own still loads as a Script", library != null)
	if library != null:
		# reload() answers with _is_valid()'s own verdict, which is the editor-facing half of this
		# and is not itself bound for GDScript to ask: OK where a broken script gives
		# ERR_COMPILATION_FAILED.
		_check_eq("it is not reported broken", library.reload(), OK)
		_check("but it cannot be instantiated", not library.can_instantiate())
		_check_eq("and it offers no base type to attach to", library.get_instance_base_type(), &"")
		_check_eq("and it registers no global class name", library.get_global_name(), &"")

	# --- R-ITER-1: a second generation, inside Godot -------------------------------------------
	#
	# host_smoke proves the host can publish generation N+1; what only this layer can see is
	# whether Godot's own script instances survive it. The editor triggers this from Play and from
	# the Build action; headless, VerseRuntime.build_project() is the same call.
	#
	# Nothing here edits a file: the point is not that the edit lands -- host_smoke has that -- but
	# that a live node attached to a script goes on working across a build, which is the no-adoption
	# rule seen from the engine's side.
	_check_eq("a second build publishes a new generation", VerseRuntime.build_project(), OK)
	_check_eq("a node attached before it still answers", node.call("EchoInt", 7), 7)
	_check("and its script is still valid", script.can_instantiate())
	var after_build := Node2D.new()
	after_build.set_script(script)
	root.add_child(after_build)
	_check_eq("and a node attached after it answers too", after_build.call("EchoInt", 11), 11)

	# --- R-LANG-6: modules ---------------------------------------------------------------------
	#
	# widgets/left/ and widgets/right/ each carry a .vmodule and each declare a `widget`. Before
	# modules that was unfixable: the project shared one flat scope, so two files could not both be
	# called widget.verse however far apart they sat. Both attach, and each runs its own code.
	var left: Script = load("res://widgets/left/widget.verse")
	var right: Script = load("res://widgets/right/widget.verse")
	_check("two same-named classes in two modules both load", left != null and right != null)
	if left != null and right != null:
		_check("and both are attachable", left.can_instantiate() and right.can_instantiate())
		var left_node := Node2D.new()
		left_node.set_script(left)
		root.add_child(left_node)
		var right_node := Node2D.new()
		right_node.set_script(right)
		root.add_child(right_node)
		_check_eq("the left one runs its own code", left_node.call("Which"), "left")
		_check_eq("and the right one runs its own", right_node.call("Which"), "right")
		# helpers.verse is at the project root, and nothing in widget.verse imports it: a module
		# reads the root module by ordinary lexical scoping.
		_check_eq("a module reaches a root definition with nothing imported",
				left_node.call("RootConstant"), 42)

	# --- R-EXP-5: @tool ------------------------------------------------------------------------
	#
	# The attribute is the bridge's own, declared in the package the host adds at runtime, so
	# tool_probe.verse compiling is what says it resolves. Whether Ready runs in the editor is not
	# a question a headless run can ask -- there is no editor to be a hint of -- but the answer
	# Godot acts on is is_tool(), and that is answered here.
	var tool_script: Script = load("res://scripts/tool_probe.verse")
	_check("a @tool script compiles", tool_script != null and tool_script.can_instantiate())
	if tool_script != null:
		_check("and reports itself a tool script", tool_script.is_tool())
	var plain_script: Script = load("res://scripts/marshal.verse")
	_check("while an unmarked one does not", plain_script != null and not plain_script.is_tool())

	# --- OQ-11 / R-SCN-3: the math types, as ordinary Verse -------------------------------------
	_check_eq("vector addition is an operator", node.call("VecAdd", Vector2(1, 2), Vector2(3, 4)),
			Vector2(4, 6))
	_check_eq("and scaling", node.call("VecScale", Vector2(3, 4), 2.0), Vector2(6, 8))
	_check_eq("length", node.call("VecLength", Vector2(3, 4)), 5.0)
	_check_eq("dot", node.call("VecDot", Vector2(1, 2), Vector2(3, 4)), 11.0)
	_check_eq("normalizing a zero vector answers zero, as Godot's own does",
			node.call("VecNormalized", Vector2(0, 0)), Vector2(0, 0))
	_check_eq("and a real one answers a unit vector",
			node.call("VecNormalized", Vector2(0, 5)), Vector2(0, 1))
	var rotated: Vector2 = node.call("VecRotated", Vector2(1, 0), PI / 2.0)
	_check("rotating by a quarter turn matches Godot's own",
			rotated.distance_to(Vector2(1, 0).rotated(PI / 2.0)) < 0.0001)

	# --- R-SCN-3: constants and statics ---------------------------------------------------------
	_check_eq("a class constant is reachable through its statics module",
			node.call("ReadyNotification"), Node.NOTIFICATION_READY)
	_check_eq("and a math type's", node.call("UpVector"), Vector2.UP)
	_check_eq("including the zero one", node.call("ZeroVector"), Vector2.ZERO)

	# Godot's own RNG, not a Verse-side one: seeding and asking again has to repeat.
	var first: int = node.call("SeededRandom", 12345)
	var again: int = node.call("SeededRandom", 12345)
	_check_eq("a seeded random repeats, so it is Godot's stream and not a second one",
			first, again)
	seed(12345)
	_check_eq("and GDScript seeding the same stream sees the same number",
			randi_range(0, 1000000), first)

	# --- R-NODE-4 / R-NODE-5: a script's own statics, and an abstract base ----------------------
	var statics_script: Script = load("res://scripts/statics_probe.verse")
	_check("statics_probe.verse compiles", statics_script != null and statics_script.can_instantiate())
	if statics_script != null:
		var probe := Node2D.new()
		probe.set_script(statics_script)
		root.add_child(probe)
		# The half that needed no bridge: a module inside a script file is ordinary Verse.
		_check_eq("a script reads its own statics with nothing from the bridge",
				probe.call("ReadMaxSpeed"), 400.0)
		_check_eq("including a static function", probe.call("ReadDescription"), "the probe")

		# The half Godot has to be told about, which is what @statics buys.
		var constants: Dictionary = (statics_script as Script).get_script_constant_map()
		_check("Godot sees the statics module's constants", constants.has("MaxSpeed"))
		if constants.has("MaxSpeed"):
			_check_eq("with their values", constants["MaxSpeed"], 400.0)
			_check_eq("of every type the module declares",
					[constants.get("Label"), constants.get("Lives")], ["probe", 3])
		# `_has_static_method` is a ScriptExtension virtual Godot calls internally and does not bind
		# for GDScript, so what can be asserted from here is that a *function* stays out of the
		# constant map -- the same split, from the side that is reachable.
		_check("while a static function is not a constant", not constants.has("Describe"))

	var abstract_script: Script = load("res://scripts/abstract_base.verse")
	_check("abstract_base.verse compiles", abstract_script != null)
	if abstract_script != null:
		_check("a class<abstract> reports itself abstract", (abstract_script as Script).is_abstract())
		_check("while an ordinary one does not", not (statics_script as Script).is_abstract())

	# --- R-NODE-7 / R-NODE-8: the full virtual set ----------------------------------------------
	#
	# Before Phase 4 exactly three of Godot's 1413 virtuals were carried, hand-written on the native
	# root -- which also meant a Resource-derived script had a `_Process` it could never receive.
	# Now each is generated onto the class that declares it, so a virtual a future Godot adds
	# arrives by regenerating the mirror with no code change here.
	var virt_script: Script = load("res://scripts/virtuals.verse")
	_check("virtuals.verse compiles", virt_script != null and virt_script.can_instantiate())
	if virt_script != null:
		var virt := Node2D.new()
		virt.set_script(virt_script)
		root.add_child(virt)

		# Dispatched by hand rather than by entering the tree: a SceneTree script's _init runs
		# before the tree is standing, so a node added here is parented and never enters. What is
		# under test is the mechanism -- that Godot's own name reaches the Verse method -- and
		# `call` is the same path GDVIRTUAL_CALL(_enter_tree) takes out of Node::_propagate_enter_tree.
		var virt_methods := []
		for m in (virt_script as Script).get_script_method_list():
			virt_methods.append(String(m["name"]))
		_check("a virtual Godot declares is listed under Godot's own name",
				virt_methods.has("_enter_tree") and virt_methods.has("_exit_tree"))
		_check("and the leading underscore is not doubled on the way out",
				not virt_methods.has("__enter_tree"))
		_check("_notification is there too, hand-declared rather than generated",
				virt_methods.has("_notification"))

		virt.call("ClearTrail")
		virt.call("_enter_tree")
		_check_eq("_enter_tree reaches the script", virt.call("ReadTrail"), "enter;")
		virt.call("ClearTrail")
		virt.call("_ready")
		_check_eq("and so does _ready", virt.call("ReadTrail"), "ready;")
		virt.call("ClearTrail")
		virt.call("_exit_tree")
		_check_eq("and _exit_tree", virt.call("ReadTrail"), "exit;")

		# R-NODE-8: `_notification` is in no part of extension_api.json, so it is hand-declared on
		# the native root and reached through the script instance's own notification hook rather
		# than through a generated declaration.
		virt.call("ClearTrail")
		virt.notification(Node.NOTIFICATION_PARENTED)
		_check_eq("_Notification reaches the script with Godot's own constant",
				virt.call("ReadTrail"), "n%d;" % Node.NOTIFICATION_PARENTED)

		# A value-returning virtual, which is what makes the default body matter: Godot asks and
		# acts on the answer, so an unoverridden one has to mean what Godot's own default means.
		_check_eq("a value-returning virtual answers the script",
				virt.call("_get_configuration_warnings"), PackedStringArray(["deliberate"]))

		root.remove_child(virt)
		virt.free()

	# The same claim, but made by *Godot* rather than by a direct call: the engine asks a virtual a
	# question and acts on the answer. `_get_configuration_warnings` above is called here by hand,
	# which proves the method resolves and not that anything consults it.
	#
	# `Control.get_minimum_size()` is the one of that family a headless run can reach -- it is the
	# only one with a public caller. `_HasPoint` and `_CanDropData` are reached from pointer-input
	# and drag paths that need a window and a mouse, so they are on the by-hand checklist. A wrong
	# default in any of the three is a working script with wrong engine behaviour and nothing said.
	var ctrl_script: Script = load("res://scripts/control_virtuals.verse")
	_check("control_virtuals.verse compiles", ctrl_script != null and ctrl_script.can_instantiate())
	if ctrl_script != null:
		var ctrl := Control.new()
		ctrl.set_script(ctrl_script)
		root.add_child(ctrl)
		_check_eq("Godot asks a value-returning virtual and acts on the answer",
				ctrl.get_minimum_size(), Vector2(73, 31))
		root.remove_child(ctrl)
		ctrl.free()

	# R-SIG-4's generated handler is NOT tested here, and the reason is worth writing down:
	# `_make_function` is a ScriptLanguageExtension virtual with no ClassDB entry, so GDScript
	# can reach neither it nor the language object usefully -- `Script.get_language()` is not in
	# the public API either, and calling the virtual by name answers "Nonexistent function". The
	# editor's C++ is its only caller, so by-hand-checklist.md carries the check, with the exact
	# text it should produce.

	# A script that overrides none of the per-frame virtuals must not claim to have them: that is
	# what decides whether Godot puts the node in the process list at all.
	var quiet := Node2D.new()
	quiet.set_script(load("res://scripts/casting.verse"))
	root.add_child(quiet)
	var quiet_methods := []
	for m in (quiet.get_script() as Script).get_script_method_list():
		quiet_methods.append(String(m["name"]))
	_check("a script that overrides no virtual does not claim one",
			not quiet_methods.has("_process") and not quiet_methods.has("_ready"))

	# --- R-SIG-1/2/3/4: signals a script declares -----------------------------------------------
	#
	# There is no @signal attribute: the member's type is the declaration and its name is the
	# signal's name. Godot learns both through _get_script_signal_list, which is read out of the
	# last analysis rather than the running program -- so a signal added in the editor shows up
	# without a build, the same bargain the export list makes.
	var sig_script: Script = load("res://scripts/signals.verse")
	_check("signals.verse compiles", sig_script != null and sig_script.can_instantiate())
	if sig_script != null:
		var emitter_node := Node2D.new()
		emitter_node.set_script(sig_script)
		root.add_child(emitter_node)

		var by_name := {}
		for entry in emitter_node.get_signal_list():
			by_name[String(entry["name"])] = entry
		_check("a Verse-declared signal appears in get_signal_list", by_name.has("Hit"))
		_check("and so do the ones with payloads",
				by_name.has("Scored") and by_name.has("Struck"))
		if by_name.has("Hit"):
			_check_eq("a tuple() payload is a signal with no arguments",
					(by_name["Hit"]["args"] as Array).size(), 0)
		if by_name.has("Scored"):
			var scored_args: Array = by_name["Scored"]["args"]
			_check_eq("a bare payload is one argument", scored_args.size(), 1)
			if scored_args.size() == 1:
				_check_eq("named for its type, because Verse tuples cannot name their elements",
						String(scored_args[0]["name"]), "Int")
				_check_eq("and typed", int(scored_args[0]["type"]), TYPE_INT)
		if by_name.has("Struck"):
			var struck_args: Array = by_name["Struck"]["args"]
			_check_eq("a tuple payload is one argument per element", struck_args.size(), 2)
			if struck_args.size() == 2:
				_check_eq("positionally named", String(struck_args[0]["name"]), "Arg0")
				_check_eq("and typed element by element", int(struck_args[1]["type"]), TYPE_STRING)

		_check("Object.has_signal sees it", emitter_node.has_signal("Hit"))

		# GDScript connects, Verse emits.
		_signal_hits = 0
		_signal_points = 0
		_signal_by = ""
		emitter_node.connect("Hit", _on_verse_hit)
		emitter_node.connect("Scored", _on_verse_scored)
		emitter_node.connect("Struck", _on_verse_struck)
		emitter_node.call("EmitHit")
		_check_eq("GDScript receives a signal a Verse script emitted", _signal_hits, 1)
		emitter_node.call("EmitScored", 5)
		_check_eq("with its payload", _signal_points, 5)
		emitter_node.call("EmitStruck", 9, "spike")
		_check_eq("and a tuple payload arrives as positional arguments",
				[_signal_points, _signal_by], [9, "spike"])

		_signal_object = null
		emitter_node.connect("Touched", _on_verse_touched)
		emitter_node.call("EmitTouched", emitter_node)
		_check("an object payload crosses as the node it names", _signal_object == emitter_node)

		# A struct payload: one argument per top-level field, named by the field. The whole reason
		# the mapping is not "one payload, one argument" -- these are the names the connect dialog
		# shows and the names Make Function writes, where a tuple payload can only offer Arg0.
		if by_name.has("Reported"):
			var reported_args: Array = by_name["Reported"]["args"]
			_check_eq("a struct payload is one argument per top-level field", reported_args.size(), 3)
			if reported_args.size() == 3:
				_check_eq("named by the field, which is what a tuple payload cannot do",
						[String(reported_args[0]["name"]), String(reported_args[1]["name"]),
								String(reported_args[2]["name"])],
						["Damage", "By", "Point"])
				_check_eq("and typed field by field",
						[int(reported_args[0]["type"]), int(reported_args[1]["type"]),
								int(reported_args[2]["type"])],
						[TYPE_INT, TYPE_STRING, TYPE_VECTOR2])
		else:
			_check("a struct payload appears in get_signal_list", false)

		_signal_report.clear()
		emitter_node.connect("Reported", _on_verse_reported)
		emitter_node.call("EmitReported", 12, "axe")
		_check_eq("a struct payload arrives as its fields, in declaration order",
				_signal_report, [12, "axe", Vector2(3, 4)])

		# And back the other way: Godot invokes with three arguments, the host builds the struct,
		# and the Verse handler reads it as one value. The inbound half is not the outbound half
		# run backwards -- nothing else on this wire can construct a struct a project declared.
		emitter_node.call("SubscribeToReported")
		emitter_node.call("EmitReported", 31, "pike")
		_check_eq("a Verse handler receives a struct payload as one value",
				[emitter_node.call("ReadReportedDamage"), emitter_node.call("ReadReportedBy"),
						emitter_node.call("ReadReportedX")],
				[31, "pike", 3.0])

		# A *direct* call cannot do the same, and the refusal is Godot's rather than the bridge's:
		# `Object::call` checks arity against the script's own method list -- which says one
		# parameter -- and answers "Expected 1 argument(s)" without entering the host at all. So the
		# packing rule reaches a signal handler and not a direct caller. A Callable invocation is
		# the difference: it arrives through vh_callback_invoke, which Godot does not arity-check.
		_check_eq("a struct-taking method still reports one parameter to Godot",
				(emitter_node.get_method_list().filter(
						func(m): return String(m["name"]) == "OnReported")[0]["args"] as Array).size(),
				1)

		# Godot's own signals, through the accessor the generator emits per signal per class. The
		# engine emits `renamed` itself, so nothing here emits it: setting the name is the event.
		# Emitted by hand rather than by setting the name: Node::set_name only emits `renamed` for a
		# node inside the tree, and a SceneTree script's _init runs before the tree is standing.
		emitter_node.call("SubscribeToRename")
		emitter_node.emit_signal("renamed")
		_check_eq("a Verse handler runs when Godot emits one of its own signals",
				emitter_node.call("ReadRenames"), 1)

		# Verse subscribes to a signal GDScript emits (R-SIG-6), through the same Callable.
		var gd_emitter := Object.new()
		gd_emitter.add_user_signal("Tally", [{"name": "points", "type": TYPE_INT}])
		emitter_node.call("ResetSeen")
		emitter_node.call("ConnectTo", gd_emitter, "Tally")
		gd_emitter.emit_signal("Tally", 4)
		_check_eq("a Verse handler runs when a GDScript object emits",
				emitter_node.call("ReadSeen"), 4)
		gd_emitter.free()

		# Two subscriptions of one handler are two connections, each with its own cancel.
		emitter_node.call("ResetSeen")
		emitter_node.call("SubscribeTwice")
		emitter_node.call("EmitScored", 1)
		_check_eq("subscribing twice connects twice", emitter_node.call("ReadSeen"), 2)
		emitter_node.call("CancelFirst")
		emitter_node.call("ResetSeen")
		emitter_node.call("EmitScored", 1)
		_check_eq("cancelling one leaves the other alive", emitter_node.call("ReadSeen"), 1)
		emitter_node.call("CancelFirstAgain")
		_check("cancelling twice is not an error", true)

		# A freed subscriber stops delivery, and emitting again is not an error: get_object() is
		# the node, so Godot drops the connection itself.
		var subscriber := Node2D.new()
		subscriber.set_script(sig_script)
		root.add_child(subscriber)
		emitter_node.connect("Scored", Callable(subscriber, "OnScored"))
		root.remove_child(subscriber)
		subscriber.free()
		emitter_node.call("EmitScored", 1)
		_check("emitting after the subscriber was freed is not an error", true)

	# --- R-SIG-1: the declarations that compile and cannot work ---------------------------------
	#
	# Four members the compiler accepts and the bridge refuses, each decidable from the declaration.
	# What is asserted here is the *drop*: Godot must not be told about a signal nothing can emit,
	# so the script's signal list carries neither the name nor a row. The sentence an author reads
	# needs the editor, and `by-hand-checklist.md` owns that half -- a headless run has no
	# _validate to ask.
	var reject_script: Script = load("res://scripts/signal_rejects.verse")
	_check("signal_rejects.verse compiles -- none of the four is a compile error", reject_script != null)
	if reject_script != null:
		var reject_names := {}
		for entry in reject_script.get_script_signal_list():
			reject_names[String(entry["name"])] = entry
		_check("a signal with nothing wrong with it is still registered", reject_names.has("Fine"))
		_check_eq("a `var` signal is not", reject_names.has("Reassignable"), false)
		_check_eq("nor a non-public one", reject_names.has("Unseen"), false)
		_check_eq("nor one whose struct payload nests a struct", reject_names.has("Nested"), false)
		_check_eq("nor one whose payload has no Godot type", reject_names.has("Maybe"), false)

		var reject_node := Node2D.new()
		reject_node.set_script(reject_script)
		root.add_child(reject_node)
		_check("has_signal agrees with the list", reject_node.has_signal("Fine"))
		_check_eq("and refuses the rejected one", reject_node.has_signal("Unseen"), false)
		_signal_points = 0
		reject_node.connect("Fine", _on_verse_scored)
		reject_node.call("EmitFine", 6)
		_check_eq("the good signal on that class still emits", _signal_points, 6)
		# Emitting a refused signal reports its own reason rather than the generic "names nothing".
		# The *text* is what matters and GDScript cannot read push_error output, so what is asserted
		# here is that none of the four is fatal; the four sentences are eyeballed in the run log and
		# recorded in phase-4-gaps.md §2.
		reject_node.call("EmitUnseen", 1)
		reject_node.call("EmitReassignable", 1)
		reject_node.call("EmitNested")
		reject_node.call("EmitMaybe")
		_check("emitting a refused signal is reported, not fatal", true)
		reject_node.call("EmitFine", 8)
		_check_eq("and the good signal on that class still works afterwards", _signal_points, 8)

	# A class with no Godot object at all: nothing ever hands it a handle, so the signal on it can
	# never be registered, connected or emitted.
	var orphan_script: Script = load("res://scripts/signal_no_owner.verse")
	_check("signal_no_owner.verse compiles", orphan_script != null)
	if orphan_script != null:
		var orphan_names := {}
		for entry in orphan_script.get_script_signal_list():
			orphan_names[String(entry["name"])] = entry
		_check_eq("a signal on a class that does not derive from `object` is not registered",
				orphan_names.has("Orphan"), false)

	# --- R-SCN-3 / OQ-11: the math written in Verse -----------------------------------------------
	#
	# Every expected value here is Godot's own answer, computed by the engine in this very script
	# where it can be, so the two are compared rather than the Verse side being compared to a
	# number someone believed. The cases are edges: negative floors, integer division of a negative,
	# an angle lerp across the wrap point.
	var mx_script: Script = load("res://scripts/mathx.verse")
	_check("mathx.verse compiles", mx_script != null and mx_script.can_instantiate())
	if mx_script != null:
		var mx := Node2D.new()
		mx.set_script(mx_script)
		root.add_child(mx)

		_check_eq("floor rounds toward negative infinity, as Godot's does",
				mx.call("FloorY", 2.7, -1.2), Vector2(2.7, -1.2).floor().y)
		_check_eq("and ceil away from it", mx.call("CeilY", 2.7, -1.2), Vector2(2.7, -1.2).ceil().y)
		_check_eq("and round", mx.call("RoundY", 2.7, -1.2), Vector2(2.7, -1.2).round().y)
		_check_eq("snapped lands on the step", mx.call("SnappedX", 7.3, 0.5),
				Vector2(7.3, 0.0).snapped(Vector2(0.5, 0.5)).x)
		_check_eq("vector min is componentwise", mx.call("MinX", 1.0, 5.0, 4.0, 2.0),
				Vector2(1, 5).min(Vector2(4, 2)).x)
		_check_eq("and so is max", mx.call("MaxY", 1.0, 5.0, 4.0, 2.0),
				Vector2(1, 5).max(Vector2(4, 2)).y)

		# The one that would have been off by one: Verse's own Quotient floors where C truncates.
		_check_eq("integer vector division truncates toward zero", mx.call("IntDivY", -3, 2),
				(Vector2i(0, -3) / 2).y)
		_check_eq("an integer vector's length is a float",
				mx.call("IntLength", 7, -3), Vector2i(7, -3).length())
		_check_eq("a vector4 dot", mx.call("Vector4Dot"),
				Vector4(1, 2, 3, 4).dot(Vector4(1, 2, 3, 4)))

		_check_eq("lerp_angle takes the short arc across the wrap point",
				mx.call("LerpAngleHalf", 0.0, 6.0), lerp_angle(0.0, 6.0, 0.5))
		_check_eq("wrapf handles a negative value", mx.call("WrapNegative", -1.0, 0.0, 5.0),
				wrapf(-1.0, 0.0, 5.0))
		_check_eq("smoothstep is the clamped cubic", mx.call("SmoothstepAt", 0.25),
				smoothstep(0.0, 1.0, 0.25))
		_check_eq("ease with a positive curve", mx.call("EaseAt", 0.5, 2.0), ease(0.5, 2.0))
		_check_eq("ease with a negative curve is in-out", mx.call("EaseAt", 0.75, -2.0),
				ease(0.75, -2.0))

		# Both of these answered 0.0 until a continuation line that began with an operator was
		# found to be silently dropped, so they are asserted against Godot rather than eyeballed.
		_check_eq("cubic_interpolate", mx.call("CubicAt"),
				cubic_interpolate(0.0, 10.0, -10.0, 20.0, 0.5))
		_check_eq("bezier_derivative", mx.call("BezierDerivAt"),
				bezier_derivative(0.0, 0.0, 1.0, 1.0, 0.5))

		_check_eq("darkened leaves alpha alone", mx.call("DarkenedAlpha"),
				Color(0.5, 0.5, 0.5, 0.25).darkened(0.5).a)
		_check_eq("and lightened moves toward white", mx.call("LightenedRed"),
				Color(0.5, 0.5, 0.5, 1.0).lightened(0.5).r)

		_check_eq("a rect with a negative size contains nothing",
				mx.call("NegativeRectHasPoint"), Rect2(10, 10, -4, -2).has_point(Vector2(8, 9)))
		_check_eq("and its abs contains the point",
				mx.call("AbsRectHasPoint"), Rect2(10, 10, -4, -2).abs().has_point(Vector2(8, 9)))
		_check_eq("rect area", mx.call("RectArea", 10.0, 4.0), Rect2(0, 0, 10, 4).get_area())

		_check_eq("acosh declines below its domain", mx.call("AcoshOk", 0.5), false)
		_check_eq("and answers inside it", mx.call("AcoshOk", 2.0), true)
		_check("is_nan recognises 0/0", mx.call("IsNanOfZeroOverZero"))

		# The utilities that are Godot's behaviour rather than Verse's spelling: these have no
		# Verse answer at all, so the assertion is that the engine's own reaches the script.
		_check_eq("type_string reaches Godot's table", mx.call("TypeNameOf", TYPE_VECTOR2),
				type_string(TYPE_VECTOR2))
		_check_eq("and error_string", mx.call("ErrorNameOf", ERR_FILE_NOT_FOUND),
				error_string(ERR_FILE_NOT_FOUND))
		_check("instance_from_id finds the node back", mx.call("SelfFromId"))

		# --- the transform family -----------------------------------------------------------
		#
		# Godot's Basis.x is column 0, and Phase 1 shipped the transposed version of this once
		# already, so every one of these is compared against the engine building the same value.
		var qa := Quaternion(0.1, 0.2, 0.3, 0.4)
		var qb := Quaternion(0.5, 0.6, 0.7, 0.8)
		_check_close("quaternion composition matches Godot's, order included",
				mx.call("QuatMulW", qa.x, qa.y, qa.z, qa.w, qb.x, qb.y, qb.z, qb.w), (qa * qb).w)
		var quarter := Quaternion(0, 0, sqrt(2.0) / 2.0, sqrt(2.0) / 2.0)
		_check_close("a quarter turn about Z takes +X to +Y",
				mx.call("QuatXformY"), (quarter * Vector3(1, 0, 0)).y)
		_check_close("slerp halfway takes the short arc", mx.call("QuatSlerpW", 0.5),
				Quaternion(0, 0, 0, 1).slerp(Quaternion(0, 0, 1, 0), 0.5).w)

		var bs := Basis(Vector3(1, 2, 0), Vector3(0, 1, 0), Vector3(0, 0, 1))
		_check_eq("a basis transforms by its columns, not its rows",
				mx.call("BasisXformY"), (bs * Vector3(3, 0, 0)).y)
		_check_close("and its determinant is the volume scale", mx.call("BasisDet", 2.0, 3.0, 4.0),
				Basis.from_scale(Vector3(2, 3, 4)).determinant())

		var t2 := Transform2D(Vector2(2, 0), Vector2(0, 2), Vector2(5, 0))
		_check_eq("a transform2d applies its origin to a point",
				mx.call("Transform2dXformX", 5.0, 3.0), (t2 * Vector2(3, 0)).x)
		_check_eq("and does not apply it to a direction",
				mx.call("Transform2dBasisXformX", 5.0, 3.0), t2.basis_xform(Vector2(3, 0)).x)
		_check_close("the affine inverse undoes the transform",
				mx.call("Transform2dRoundTripX", 4.0, -1.0), 4.0)

		var t3 := Transform3D(Basis(Vector3(1, 0, 0), Vector3(0, 1, 0), Vector3(0, 0, 2)), Vector3(0, 0, 5))
		_check_eq("a transform3d composes basis then origin",
				mx.call("Transform3dXformZ"), (t3 * Vector3(0, 0, 3)).z)

		_check_eq("a plane's distance is signed", mx.call("PlaneDistance", 0.0),
				Plane(Vector3(1, 0, 0), 2.0).distance_to(Vector3(0, 0, 0)))
		_check_eq("an aabb's volume", mx.call("AabbVolume", 2.0, 3.0, 4.0),
				AABB(Vector3(), Vector3(2, 3, 4)).get_volume())

		# The last six utilities. fmod truncates toward zero, so -7 mod 3 is -1 and not 2.
		_check_close("fmod truncates toward zero", mx.call("FModAt", -7.0, 3.0), fmod(-7.0, 3.0))
		_check_eq("nearest_po2", mx.call("NearestPo2At", 100), nearest_po2(100))
		_check_eq("and is exact on a power of two", mx.call("NearestPo2At", 64), nearest_po2(64))
		_check_eq("step_decimals", mx.call("StepDecimalsAt", 0.01), step_decimals(0.01))
		_check_close("cubic_interpolate_angle",
				mx.call("CubicAngleAt"), cubic_interpolate_angle(0.1, 0.2, 0.0, 0.3, 0.5))

		root.remove_child(mx)
		mx.free()

	# --- R-ASYNC-8: a call from another thread is refused rather than served --------------------
	#
	# VerseVM asserts the game thread at the top of every VM entry, and it is an `ensure` rather
	# than a `check`: proceeding would be a logged callstack followed by undefined behaviour. So
	# the host compares the thread it was initialised on and answers VH_ERR_THREAD having run
	# nothing, which reaches GDScript as an invalid call.
	_thread_answer = "not started"
	var task_id := WorkerThreadPool.add_task(_call_verse_off_thread.bind(node))
	WorkerThreadPool.wait_for_task_completion(task_id)
	_check_eq("the worker task ran", _thread_answer != "not started", true)
	_check_eq("but the Verse method it called did not", _thread_answer, "ran")
	_check_eq("and the main thread still works afterwards", node.call("EchoInt", 3), 3)

	# --- R-INT-4 / R-SIG-3: a Verse function as a Godot Callable --------------------------------
	var verse_callable: Variant = node.call("MakeOnCalled")
	_check_eq("a Verse function crosses as a Callable", typeof(verse_callable), TYPE_CALLABLE)
	if typeof(verse_callable) == TYPE_CALLABLE:
		_check("and it reports the node it is bound to", (verse_callable as Callable).get_object() == node)
		_check("and is valid while that node lives", (verse_callable as Callable).is_valid())
		(verse_callable as Callable).call(21)
		_check_eq("calling it from Godot runs the Verse method", node.call("ReadCallbackSeen"), 21)

		# The engine half: Godot emits, and a Verse function connected to the signal runs. A user
		# signal on a bare Object rather than a timer, because a headless run has no time to wait.
		var emitter := Object.new()
		emitter.add_user_signal("ping", [{"name": "value", "type": TYPE_INT}])
		emitter.connect("ping", verse_callable as Callable)
		emitter.emit_signal("ping", 34)
		_check_eq("a Verse function connected to a signal runs when Godot emits it",
				node.call("ReadCallbackSeen"), 34)

		var two: Variant = node.call("MakeOnCalledTwice")
		if typeof(two) == TYPE_CALLABLE:
			emitter.add_user_signal("ping2", [
				{"name": "a", "type": TYPE_INT}, {"name": "b", "type": TYPE_INT}])
			emitter.connect("ping2", two as Callable)
			emitter.emit_signal("ping2", 7, 9)
			_check_eq("a two-parameter handler takes a two-argument emission",
					node.call("ReadCallbackSeen"), 709)
		emitter.free()

	# --- R-SCN-6: the cast, and object identity ------------------------------------------------
	#
	# The two halves are separate claims. A node with no script must cross as the mirror of the
	# Godot class it actually is, or every cast fails; a node carrying a Verse script must cross as
	# *that script's own object*, or a cast to the script's class fails while the mirror one works.
	# Both are asked here, along with the case that must decline rather than error.
	var cast_script: Script = load("res://scripts/casting.verse")
	_check("casting.verse compiles", cast_script != null and cast_script.can_instantiate())
	if cast_script != null:
		var caster := Node2D.new()
		caster.set_script(cast_script)
		root.add_child(caster)

		var sprite := Sprite2D.new()
		sprite.name = "Pic"
		caster.add_child(sprite)
		_check_eq("a node Godot handed back casts to its own Godot class",
				caster.call("SpriteName", "Pic"), "Pic")
		_check_eq("and a cast to a class it is not declines rather than erroring",
				caster.call("TimerName", "Pic"), "no")

		var scripted := Node2D.new()
		scripted.name = "Scripted"
		scripted.set_script(load("res://scripts/derived_entity.verse"))
		caster.add_child(scripted)
		_check_eq("a node carrying another Verse script casts to that script's class, and runs its code",
				caster.call("Describe", "Scripted"), "derived")
		_check_eq("the same node handed over as an argument casts the same way",
				caster.call("DescribeGiven", scripted), "derived")
		_check_eq("while a node with no script declines that cast",
				caster.call("DescribeGiven", sprite2d_as_node2d(sprite)), "no")

	# --- R-AUD-1: what a failure undoes ---------------------------------------------------------
	#
	# Phase 4.5's spikes S-3 and S-4, kept as behavioural cases because the rule written next to
	# R-AUD-1 has to describe measured behaviour rather than intended behaviour. `transactions.verse`
	# carries the commentary; what is only visible here is what the node's properties ended up as.
	#
	# It used to run a step per frame, and that was the first thing Phase 4.5's spike found: a raise
	# stopped every script until the next `vh_tick`, so a second Verse call in the same frame
	# answered VH_ERR_HALTED and never ran. Phase 5 replaced that with a task scope per instance
	# (R-ASYNC-4), so a raise now stops only the call that raised -- and the cases below run one
	# after another in a single frame, raises included, which is the test that it did.
	#
	# `_process` is still where the frames are, because the concurrency section after it needs
	# them: a task suspended on a signal resumes when Godot emits, and that takes a frame.
	var tx_script: Script = load("res://scripts/transactions.verse")
	_check("transactions.verse compiles", tx_script != null and tx_script.can_instantiate())
	if tx_script == null:
		print("[integration] %d passed, %d failed" % [_passed, _failed])
		quit(1)
		return
	_tx = Node2D.new()
	_tx.set_script(tx_script)
	root.add_child(_tx)


# Returning true quits the loop, which is how the suite ends now that its last section spans frames.
func _process(_delta: float) -> bool:
	if _tx == null:
		return false
	_tx_step += 1
	match _tx_step:
		1:
			# Phase 4.5 stage 1: a helper narrowed to `<reads>`, which is the whole point of giving
			# Godot's 6728 const-and-answering methods that effect. It compiling at all is the
			# assertion; the numbers only say it ran.
			var kid := Node2D.new()
			kid.name = "Kid"
			_tx.add_child(kid)
			_tx.position = Vector2(3, 4)
			_check_eq("a <reads> helper reads the scene", _tx.call("DescribeReads"), 6.0)
			_check_eq("and a <transacts> caller can still reach it",
					_tx.call("ReadFromTransacts"), 6.0)
			_check_eq("a failure context works inside a <reads> body",
					_tx.call("FirstChildName"), "Kid")
			_tx.remove_child(kid)
			kid.free()

			# A write inside a failure context that declines. `set Position` defers to commit, and
			# the Verse-level unwind has to take the deferral with it.
			_tx.position = Vector2(1, 1)
			_tx.call("SetThenFailInner")
			_check_eq("a write inside a failed context does not reach Godot",
					_tx.position, Vector2(1, 1))
			_check_eq("and nothing after the failed context runs either", _tx.rotation, 0.0)

			# The same write declining at the *top* level of the call. `InstanceCall` reads that as
			# `FOpResult::Fail` and its own AutoRTFM transaction still commits -- so were the host's
			# transaction the only one, this write would land. It does not: VerseVM wraps the
			# invocation of a `<decides>` function in a failure context of its own, and that is the
			# transaction the deferral was registered against.
			_tx.position = Vector2(1, 1)
			_check_eq("a <decides> method that declines answers nothing",
					_tx.call("SetThenDecline"), null)
			_check_eq("and a top-level decline drops its deferred write too",
					_tx.position, Vector2(1, 1))

			# A read after a deferred write in the same call sees the *old* value: the write has not
			# happened yet. This is the sharp edge, not a defect.
			_tx.position = Vector2(7, 7)
			_tx.call("SetThenRead")
			_check_eq("the deferred write lands at commit", _tx.position, Vector2(44, 44))
			_check_eq("but a read in the same call saw the value from before it",
					_tx.call("ReadObservedX"), 7.0)

			# Two deferred writes to one property: the queue is ordered, so the second wins.
			_tx.call("SetTwice")
			_check_eq("deferred writes commit in the order they were made",
					_tx.position, Vector2(66, 66))

			# S-3's control, and its two non-raising failures. `Subscribe` mutates Godot and answers,
			# so it is compensated rather than deferred: the host registers an
			# `AutoRTFM::OnAbort<SameAsClosed>` that disconnects. These are what say it runs.
			_tx.call("SubscribePlainly")
			_check_eq("a plain Subscribe connects",
					_tx.get_signal_connection_list("Hit").size(), 1)
			_tx.call("SubscribeThenDecline")
			_check_eq("a Subscribe undone by a top-level decline leaves no connection",
					_tx.get_signal_connection_list("Hit").size(), 1)
			_tx.call("SubscribeThenFailInner")
			_check_eq("a Subscribe undone by a failed context leaves no connection",
					_tx.get_signal_connection_list("Hit").size(), 1)
		2:
			# A raise, which aborts the host's transaction and drops what it had deferred. Three of
			# them, in one frame, with ordinary calls between: before Phase 5 the second and third
			# would have answered VH_ERR_HALTED without running, and every assertion after the first
			# would have been about a call that never happened.
			_tx.position = Vector2(1, 1)
			_tx.call("SetThenRaise")
			_check_eq("a raise drops the writes its transaction had deferred",
					_tx.position, Vector2(1, 1))
			_check_eq("and the very next call in the same frame still runs",
					_tx.call("ReadObservedX"), 7.0)

			var before: int = _tx.get_signal_connection_list("Hit").size()
			_tx.call("SubscribeThenRaise")
			_check_eq("a Subscribe undone by a raise leaves no connection",
					_tx.get_signal_connection_list("Hit").size(), before)

			# The uncompensated shape beside it: `Object.connect` mutates Godot *and* answers, so it
			# can be neither deferred nor ignored, and nothing undoes it.
			_tx.call("ConnectThenRaise")
			_check_eq("a mutate-and-answer call survives the raise that follows it",
					_tx.get_signal_connection_list("Scored").size(), 1)

		# --- R-ASYNC-1/2/4/5, R-SIG-5: tasks --------------------------------------------------
		#
		# A frame apart on purpose. A task spawned in one call suspends; what resumes it happens in
		# a later one, and a `vh_tick` runs in between -- which is the claim being tested, not a
		# scheduling detail.
		3:
			var conc_script: Script = load("res://scripts/concurrency.verse")
			_check("concurrency.verse compiles", conc_script != null and conc_script.can_instantiate())
			if conc_script == null:
				print("[integration] %d passed, %d failed" % [_passed, _failed])
				quit(1)
				return true
			_conc = Node2D.new()
			_conc.set_script(conc_script)
			root.add_child(_conc)
			_conc2 = Node2D.new()
			_conc2.set_script(conc_script)
			root.add_child(_conc2)

			# Awaiting a signal the script itself declares. The task suspends inside the call that
			# spawned it, and the call returns normally -- which is what "the call returns VH_OK"
			# means from out here.
			_conc.call("StartWaitForFired")
			_check_eq("a spawned task runs up to its first await", _conc.call("ReadStage"), 1)

			# One Godot connection, ours, for the duration of the wait.
			_check_eq("awaiting connects to the signal",
					_conc.get_signal_connection_list("Fired").size(), 1)
		4:
			_check_eq("and the task is still suspended a frame later", _conc.call("ReadStage"), 1)

			# GDScript emits, and the Verse task resumes inside the emission (R-SIG-5).
			_conc.emit_signal("Fired")
			_check_eq("emitting resumes the awaiting task", _conc.call("ReadStage"), 2)
			_check_eq("and the connection it made is gone again",
					_conc.get_signal_connection_list("Fired").size(), 0)

			# The payload comes back typed, which is what the event buys.
			_conc.call("Reset")
			_conc.call("StartWaitForScored")
			_conc.emit_signal("Scored", 17)
			_check_eq("an awaited payload arrives typed", _conc.call("ReadSeen"), 17)

			_conc.call("Reset")
			_conc.call("StartWaitForStruck")
			_conc.emit_signal("Struck", 9, "spike")
			_check_eq("and a tuple payload arrives as its elements",
					[_conc.call("ReadSeen"), _conc.call("ReadSeenBy")], [9, "spike"])

			# One of Godot's own, through the accessor the generator emits per signal per class.
			# Nothing per-signal was written for this: every mirrored accessor answers the same
			# `signal(t)` a declaration does.
			_conc.call("Reset")
			var awaited_timer := Timer.new()
			awaited_timer.one_shot = true
			root.add_child(awaited_timer)
			_conc.call("StartWaitForTimer", awaited_timer)
			_check_eq("awaiting an engine signal connects to it",
					awaited_timer.get_signal_connection_list("timeout").size(), 1)
			awaited_timer.emit_signal("timeout")
			_check_eq("and the task resumes when the engine emits", _conc.call("ReadStage"), 10)
			root.remove_child(awaited_timer)
			awaited_timer.free()

			# A signal nothing in the mirror knows about (R-INT-1): one made with add_user_signal,
			# reached by naming it. There is no declared payload, so the arguments arrive as the
			# Godot Array they would have been anyway.
			_conc.call("Reset")
			_foreign = Object.new()
			_foreign.add_user_signal("Tally", [{"name": "points", "type": TYPE_INT}])
			_conc.call("StartWaitForForeign", _foreign, "Tally")
			_check_eq("awaiting a foreign signal connects to it",
					_foreign.get_signal_connection_list("Tally").size(), 1)
			_foreign.emit_signal("Tally", 23)
			_check_eq("and resumes with the arguments as an Array",
					[_conc.call("ReadSeen"), _conc.call("ReadStage")], [23, 20])

			# The rollback-safe way to *subscribe* to one, which is what `Object.Connect` could not
			# be: it mutates Godot and answers a value, so a raise after it left the connection
			# behind. This one is compensated the way signal.Subscribe is.
			_conc.call("Reset")
			_conc.call("SubscribeForeign", _foreign, "Tally")
			_check_eq("subscribing to a foreign signal connects",
					_foreign.get_signal_connection_list("Tally").size(), 1)
			_foreign.emit_signal("Tally", 5)
			_check_eq("and the handler receives the arguments", _conc.call("ReadSeen"), 5)
			_conc.call("SubscribeForeignThenFail", _foreign, "Tally")
			_check_eq("while one undone by a failure leaves no connection",
					_foreign.get_signal_connection_list("Tally").size(), 1)

			# What a race leaves behind. The loser is cancelled and its `defer` is the only thing
			# that takes its connection away, so this is the case that says `defer` runs on
			# cancellation as well as on return.
			_conc.call("Reset")
			_conc.call("StartRace")
			_check_eq("racing two awaits connects to both",
					[_conc.get_signal_connection_list("Fired").size(),
							_conc.get_signal_connection_list("Scored").size()], [1, 1])
			_conc.emit_signal("Fired")
			_check_eq("the race returns when the first fires", _conc.call("ReadStage"), 30)
			_check_eq("and the loser leaves no connection behind",
					[_conc.get_signal_connection_list("Fired").size(),
							_conc.get_signal_connection_list("Scored").size()], [0, 0])

			# A signal handler may start a task now, which is what widening Subscribe's callback
			# was for -- and the shape a game-over sequence is written in.
			_conc.call("Reset")
			_conc.call("SubscribeStartingTask")
			_conc.emit_signal("Scored", 1)
			_check_eq("a signal handler can spawn a task", _conc.call("ReadStage"), 1)

			# R-ASYNC-4: a raise cancels the raising instance's tasks and nobody else's. Two
			# instances of one script, each with a task suspended on its own signal.
			_conc.call("Reset")
			_conc2.call("Reset")
			_conc.call("StartWaitForFired")
			_conc2.call("StartWaitForFired")
			_conc.call("Raise")
			_conc2.emit_signal("Fired")
			_check_eq("a raise leaves another instance's suspended task running",
					_conc2.call("ReadStage"), 2)
			_conc.emit_signal("Fired")
			_check_eq("while the raising instance's task is gone -- still at the stage it reached "
					+ "before it suspended, never the one past the await",
					_conc.call("ReadStage"), 1)

			# Sleep, on the host's own real-time clock. Sleep(0.0) means "resume at the next pump",
			# so this one is answered on the next frame rather than in this call.
			_conc.call("Reset")
			_conc.call("StartNap")
			_check_eq("a sleeping task has not resumed yet", _conc.call("ReadStage"), 0)
		5:
			_check_eq("Sleep(0.0) resumes at the next tick", _conc.call("ReadStage"), 40)

			# R-ASYNC-5: freeing the node cancels its tasks, which is GDScript's own trigger.
			#
			# Asserted on a *foreign* emitter rather than on the node's own signal, because that is
			# the only way to see it from out here: the connection the wait made lives on an object
			# that outlives the node, so it can be counted before and after. A cancelled task whose
			# `defer` never ran would leave it behind.
			_conc2.call("Reset")
			_conc2.call("StartWaitForForeign", _foreign, "Tally")
			_check_eq("a second node's wait connects to the foreign signal",
					_foreign.get_signal_connection_list("Tally").size(), 2)
			var doomed := _conc2
			_conc2 = null
			root.remove_child(doomed)
			doomed.free()
			_check_eq("freeing the node cancelled its task and took the connection with it",
					_foreign.get_signal_connection_list("Tally").size(), 1)
			_foreign.emit_signal("Tally", 99)
			_check("emitting afterwards is not an error", true)

			_foreign.free()
			_foreign = null

			# R-NODE-7 for the virtual the engine asks *before* deciding a click is yours.
			# Nothing calls `_HasPoint`; the engine asks it during picking, and an injected
			# InputEventMouseButton is enough to make it (by-hand-findings.md B10).
			_hit = Control.new()
			_hit.set_script(load("res://scripts/has_point_probe.verse"))
			_hit.offset_right = 200
			_hit.offset_bottom = 200
			root.add_child(_hit)
			_click_at(Vector2(50, 50))
		7:
			# `_HasPoint` answered false, so picking walked past a control that covers the point.
			_check_eq("a control whose _HasPoint says no is not picked", _hit.get("Clicks"), 0)
			_hit.set("Solid", true)
			_click_at(Vector2(50, 50))
		9:
			# The same click, the same rect, the other answer. This half is what says the first
			# was the virtual and not a control the engine failed to find for some other reason.
			_check_eq("and one whose _HasPoint says yes is", _hit.get("Clicks"), 1)
			root.remove_child(_hit)
			_hit.free()
			_hit = null

			# A source change reaching an attached node, which needs the instance replaced: it
			# holds a vh_instance made against the retiring generation and adopts nothing. Before
			# B8 the edit compiled and the node kept answering the old code until a restart.
			_check_reload_replaces_the_instance()
		6, 8:
			# One frame for the injected event to be delivered.
			pass
		_:
			print("[integration] %d passed, %d failed" % [_passed, _failed])
			quit(1 if _failed > 0 else 0)
			return true
	return false
