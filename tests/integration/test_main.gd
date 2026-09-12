extends SceneTree

# The integration layer R-QUAL-1 asks for: a headless Godot with Verse scripts attached, asserting
# on behaviour rather than on the ABI's own return codes. The unit and ABI layers already cover
# those; what only this layer can see is whether Godot's own dispatch reaches Verse at all.
#
# Shaped like the other suites in this repo: one line per case, non-zero exit on failure, no
# framework. Run through tools/run_tests.py.

var _passed := 0
var _failed := 0


func _double(n: int) -> int:
	return n * 2


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

	# A Godot virtual is listed under Godot's own name, which is how the engine finds it.
	_check("an overridden Godot virtual is listed under Godot's name", names.has("_ready"))
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

	print("[integration] %d passed, %d failed" % [_passed, _failed])
	quit(1 if _failed > 0 else 0)
