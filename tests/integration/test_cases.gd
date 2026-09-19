# The integration cases R-QUAL-1 asks for, as a library two drivers share.
#
# `test_main.gd` runs them against the editor binary, where the project is compiled at startup;
# `export_check.gd` runs the same lines inside an exported game, where it was cooked instead. One
# set of lines, so the two cannot disagree about what passing means -- which is the whole point of
# running them twice. Shaped like `dodge-the-creeps/checks.gd`, which did this first.
#
# Asserting on behaviour rather than on the ABI's own return codes: the unit and ABI layers cover
# those, and what only this layer can see is whether Godot's own dispatch reaches Verse at all.
# One line per case, no framework.
extends RefCounted

# Whose tree these run against. A SceneTree driver is one; an autoload's get_tree() is the other.
var tree: SceneTree

# False in an exported game. Eleven compiler-side entry points answer VH_ERR_UNSUPPORTED there and
# res:// is a read-only pack, so a case that needs either is skipped rather than failed -- and
# counted, because a case that stops running in an export must not read as a shorter log.
var editor := true

# A precondition failed and nothing after it means anything.
var fatal := false

var _passed := 0
var _failed := 0
var _skipped := 0
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
var _vararg_ping := -1
var _vararg_pokes := 0

# An `@export_signal` member declared as an `event(t)` holds one Godot connection of its own, made
# at the first entry into the instance and held for the instance's life. A bare event's `Await` is
# Verse's own native, so there is no hook at the await for the host to connect from -- which is the
# one place R-SIG-5's "the connection lives exactly as long as the wait" is traded away, and it is
# traded only for the thing that buys it.
#
# So every count below is the member's own plus whatever the case made. A *foreign* signal and an
# engine accessor still connect per subscription and per wait, which is why the `Tally` and
# `timeout` cases further down carry no such term.
const OWN_CONNECTION := 1


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


# Zero-argument, so the *no-tail* arity of the generated vararg pair has something to reach.
func _say() -> String:
	return "said"


func _add_two(a: int, b: int) -> int:
	return a + b


func _on_verse_pinged(n: int) -> void:
	_vararg_ping = n


func _on_verse_poked() -> void:
	_vararg_pokes += 1


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
	tree.root.add_child(node)
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

	tree.root.remove_child(node)
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


# A case that cannot run where it is being run, for a reason this suite knows in advance. It is
# counted rather than dropped, and the count is asserted: a case that quietly stops running in an
# export would otherwise read as a shorter log and nothing else.
func _skip(name: String, why := "editor only") -> void:
	_skipped += 1
	print("[integration] %s: skip -- %s" % [name, why])


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


func begin() -> void:
	var script: Script = load("res://scripts/marshal.verse")
	if script == null:
		print("[integration] load marshal.verse: FAIL (nothing loaded)")
		fatal = true
		return
	_check("load marshal.verse", true)
	_check("the script compiled", script.can_instantiate())

	var node := Node2D.new()
	node.set_script(script)
	tree.root.add_child(node)
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

	# R-TYPE-2: a container the script built for itself, from the bare archetype. `godot_array{}`
	# used to hold reference 0, which crosses as Nil, so every one of these would have been null.
	var made_array: Variant = node.call("MadeArray")
	_check("a bare godot_array{} arrives as an empty Array",
			made_array is Array and (made_array as Array).is_empty())

	var filled: Variant = node.call("MadeArrayFilled")
	_check("and one the script filled arrives with its elements",
			filled is Array and (filled as Array).size() == 2 and filled[0] == 7 and filled[1] == "x")

	var made_dict: Variant = node.call("MadeDictionary")
	_check("a bare dictionary{} arrives as a Dictionary",
			made_dict is Dictionary and (made_dict as Dictionary).get("hp") == 3)


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

	# R-NODE-9: the class a parameter declares, which is the whole of what Godot draws in a call
	# hint. A PropertyInfo with no class_name is `Object` however specific the declaration was.
	#
	# Read off the *node* here and off the script below, because the two are built by different
	# code: an instance answers get_method_list through the raw GDExtension vtable and a script
	# answers _get_script_method_list with a Dictionary, and only one of them used to be wrong at
	# a time when the other was right.
	if names.has("TakesTimer"):
		_check_eq("a mirrored class parameter is named to Godot",
				String(names["TakesTimer"]["args"][0]["class_name"]), "Timer")
	else:
		_check("a mirrored class parameter is named to Godot", false)
	if names.has("TakesMarshal"):
		_check_eq("a registered script class parameter is named as Godot registered it",
				String(names["TakesMarshal"]["args"][0]["class_name"]), "Marshal")
	else:
		_check("a registered script class parameter is named as Godot registered it", false)
	if names.has("TakesHelper"):
		_check_eq("an unregistered one falls back to its nearest mirrored class",
				String(names["TakesHelper"]["args"][0]["class_name"]), "Node2D")
	else:
		_check("an unregistered one falls back to its nearest mirrored class", false)
	if names.has("TakesMaybeTimer"):
		# `?timer` reaches Godot as an object, not as a variant: null is the empty case and every
		# object slot can hold it, so the two spellings differ in what Verse makes the body prove
		# and in nothing Godot is told. Asserted because it was assumed to go the other way.
		var maybe_arg: Dictionary = names["TakesMaybeTimer"]["args"][0]
		_check_eq("an optional reference is an object like any other",
				int(maybe_arg["type"]), TYPE_OBJECT)
		_check_eq("and is named by the same class", String(maybe_arg["class_name"]), "Timer")
	else:
		_check("an optional reference is an object like any other", false)

	# Null, and the class the parameter names. The host decided which class a declaration named
	# before it looked at whether a value had arrived at all, so an optional parameter of the
	# project's own class refused Godot's own null with "Cannot convert argument 1 from Nil to
	# Object" -- a sentence about the one value that signature exists to accept
	# (`by-hand-findings.md` B34).
	_check("null satisfies an optional mirrored parameter", node.call("MaybeTimerIsEmpty", null))
	_check("and an optional parameter of the project's own class",
			node.call("MaybeMarshalIsEmpty", null))
	_check("which a real node then does not", not node.call("MaybeMarshalIsEmpty", node))
	# The non-empty half, which is what says the refusal was not simply moved: the object that
	# arrives is that node's own script instance, so a member read off it holds what the script does.
	_check_eq("and the object that arrives is that node's own instance",
			node.call("MaybeMarshalCounter", node), node.get("Counter"))
	_check_eq("a bare parameter of the project's own class takes one too",
			node.call("MarshalCounter", node), node.get("Counter"))

	# And null the other way, through a parameter of the *mirror* rather than of a script. Godot's
	# own dump says which object arguments accept it -- `"meta": "required"` marks the ones that do
	# not -- and `Node.set_owner` is not one of them, so the mirror declares `Owner:?node`. Before
	# that spelling existed a Verse script could not make this call at all: a class has no value
	# that means nothing (R-TYPE-4).
	# Read in a later call than the write, because a Godot write defers to the transaction's commit.
	node.call("SetOwnerTo", node.get_parent())
	_check("a real node reaches a mirrored optional parameter", node.call("HasOwner"))
	node.call("ClearOwner")
	_check("and null reaches it too, which is what the option is for", not node.call("HasOwner"))
	# And the same metadata read at the other end: a result Godot marks as one that cannot be null
	# drops the `<decides>` a caller would otherwise need a failure context for. The Verse body
	# spells `GetRoot()` rather than `GetRoot[]`, so it would not have compiled before this.
	_check_eq("a result Godot marks required is reached without a failure context",
			node.call("RootName", tree), String(tree.root.name))

	var script_by_name := {}
	for method in (script as Script).get_script_method_list():
		script_by_name[String(method["name"])] = method
	if script_by_name.has("TakesTimer"):
		_check_eq("the script's own list names it too",
				String(script_by_name["TakesTimer"]["args"][0]["class_name"]), "Timer")
	else:
		_check("the script's own list names it too", false)
	if script_by_name.has("GivesTimer"):
		_check_eq("and a result is named at the other end of the call",
				String(script_by_name["GivesTimer"]["return"]["class_name"]), "Timer")
	else:
		_check("and a result is named at the other end of the call", false)

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
	_check_eq("VariantInt writes through the reference", mixed[0], 99)

	# And a `variant` as a script method's own parameter and return type, which is what R-NODE-10's
	# _Get and _Set are declared in terms of.
	_check_eq("a variant parameter carries an int", node.call("VariantKindName", 7), "int")
	_check_eq("and a string", node.call("VariantKindName", "hi"), "string")
	_check_eq("and Godot's own null", node.call("VariantKindName", null), "nil")
	_check_eq("a variant return comes back as what it holds", node.call("EchoVariant", 42), 42)
	_check_eq("a nil variant comes back as null", node.call("NilVariant"), null)

	# --- R-SCN-2: Godot's 33 vararg entry points ------------------------------------------------
	#
	# The mirror carried none of them until Phase 4b stage 6, because `Variant...` had no Verse
	# spelling. Each is emitted as two *arities* of one name -- the fixed prefix alone, and the
	# prefix plus one `[]variant` tail -- so what has to be checked is that a call reaches the arity
	# it named. Resolving to the other one would still compile and would still return something.
	_check_eq("Object.call carries a vararg tail into another object",
			node.call("CallOther", self, "_double", 21), 42)
	_check_eq("and the no-tail arity of the same name reaches a zero-argument method",
			node.call("CallOtherNoArgs", self, "_say"), "said")
	_check_eq("and a tail of more than one", node.call("CallOtherTwo", self, "_add_two", 40, 2), 42)

	# Object.emit_signal, which is the other entry point stage 6's done-when names. A user signal
	# rather than one of Godot's own, so the payload is this suite's to choose.
	var pinger := Node.new()
	pinger.add_user_signal("pinged", [{"name": "n", "type": TYPE_INT}])
	pinger.add_user_signal("poked")
	pinger.connect("pinged", _on_verse_pinged)
	pinger.connect("poked", _on_verse_poked)
	tree.root.add_child(pinger)
	_check_eq("Object.emit_signal answers OK", node.call("EmitOn", pinger, "pinged", 99), OK)
	_check_eq("and the payload arrived", _vararg_ping, 99)
	_check_eq("a payloadless emit takes the no-tail arity",
			node.call("EmitOnNoArgs", pinger, "poked"), OK)
	_check_eq("and it fired once", _vararg_pokes, 1)

	# The other six, which live on a *reference* rather than on an object handle -- Callable.call,
	# Callable.bind, Signal.emit and three more. VhCallValue takes a Godot Object's handle and none
	# of the builtin types is one, so these had no route at all before ABI 8.7's VhRefCall.
	_check_eq("Callable.call reaches through a reference",
			node.call("CallCallable", _double, 21), 42)
	# Godot's own currying, and the order is its own too: bind appends after the call's arguments.
	_check_eq("Callable.bind curries, with its arguments last",
			node.call("CallBound", _add_two, 10, 5), 15)
	# No zero-argument arity exists for any of these, for the GDScript.new reason -- `array{}` is
	# how an empty argument list is written, and this is the case that says so.
	_check_eq("and an empty argument list is array{}",
			node.call("CallCallableNoArgs", _say), "said")

	_vararg_ping = -1
	node.call("EmitForeign", pinger, "pinged", 7)
	_check_eq("Signal.emit reaches a signal the mirror has no accessor for", _vararg_ping, 7)
	_vararg_pokes = 0
	node.call("EmitForeignNoArgs", pinger, "poked")
	_check_eq("and a payloadless one", _vararg_pokes, 1)

	# MakeVariant: the host picks the lane from the value itself. Reading the meta back through
	# Godot is what makes these assertions about the Variant type Godot stored, rather than about
	# Verse handing its own struct back to itself.
	node.call("MetaFromValue", self, "mv_int", 42)
	_check_eq("MakeVariant reaches the int lane", get_meta("mv_int"), 42)
	_check("and it is an int to Godot, not a float",
			typeof(get_meta("mv_int")) == TYPE_INT)
	node.call("MetaFromString", self, "mv_str", "hello")
	_check_eq("and the string lane", get_meta("mv_str"), "hello")
	node.call("MetaFromVector", self, "mv_vec", 3.0, 4.0)
	_check_eq("and a math struct, from the value alone", get_meta("mv_vec"), Vector2(3.0, 4.0))
	_check("which arrives as a real Vector2", typeof(get_meta("mv_vec")) == TYPE_VECTOR2)
	# A value that cannot say what it is fails rather than guessing.
	_check_eq("and it refuses a value that cannot describe itself",
			node.call("MakeFromArray"), 0)

	# RID is a `rid` struct rather than a bare int, and the lane it rides in was wrong until it
	# became one: the packer wrote Ref and the generated reader read I0, so every mirrored method
	# returning a RID answered 0. Asserting against Godot's own get_canvas_item() is what makes
	# this about the value rather than about Verse agreeing with itself.
	_check_eq("a RID crosses from a real Godot call",
			node.call("CanvasRid", node), node.get_canvas_item().get_id())
	_check("and it is not zero, which is what the bug looked like",
			node.call("CanvasRid", node) != 0)
	_check_eq("a RID survives a variant round trip", node.call("RoundTripRid", 99), 99)
	_check_eq("and a RID is not an int to the readers", node.call("RidIsNotAnInt"), -1)
	# A `rid` as a declared parameter and return type, which is a different path from the variant
	# payload above: it goes through the host's type classification rather than through a lane.
	# A `rid` as a declared parameter and return type, which is a different path from the variant
	# payload above: it goes through the host's type classification rather than through a lane, and
	# both halves of that were broken in their own way. Inbound worked only by accident -- a
	# one-field user struct that InstanceCall filled from the single int -- and outbound could not
	# work at all, because the consumer answered an empty Variant for every RID.
	_check_eq("a rid crosses into a declared parameter",
			node.call("TakeRid", node.get_canvas_item()), node.get_canvas_item().get_id())
	var given_rid = node.call("GiveRid", 99)
	_check("and back out as a Godot RID rather than a null", typeof(given_rid) == TYPE_RID)
	_check_eq("carrying its id", (given_rid as RID).get_id(), 99)

	# The *loose* arities, which exist because an author reached for `Call("test",
	# VariantInt(1))` -- what `call("test", 1)` looks like in GDScript -- and got "No overload
	# of the function `Call` matches the provided arguments (:[]char,:variant)". What these assert
	# is that the loose spelling and the array spelling reach the same call; a loose one resolving
	# to some other overload would still compile and still answer something.
	_check_eq("a vararg takes loose values, not only an array",
			node.call("CallOtherLoose", self, "_double", 21), 42)
	_check_eq("and more than one of them",
			node.call("CallOtherLooseTwo", self, "_add_two", 40, 2), 42)
	_vararg_ping = -1
	node.call("EmitOnLoose", pinger, "pinged", 55)
	_check_eq("emit_signal takes them too", _vararg_ping, 55)
	# A reference vararg has no fixed prefix, so one loose value is all it can have -- two would be
	# a tuple, and a tuple of variants is the same type as the array arity.
	_check_eq("and a reference vararg takes its one",
			node.call("CallCallableLoose", _double, 4), 8)

	pinger.queue_free()

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

	# --- R-INT-9: the same, from a `<reads>` Verse function --------------------------------------
	#
	# `CallConst` is the `<reads>` twin of `Call`, and it exists because `Object::call` is not const
	# so the generated `Call` cannot be: without it a Verse function that only *reads* a GDScript
	# node had to declare `<transacts>`, and so did everything that called it. The values here are
	# the same ones the `<transacts>` cases above assert -- what changed is the specifier the Verse
	# side could write, which the compiler checked before this ever ran.
	_check_eq("a <reads> Verse function calls a GDScript method",
			node.call("CallOtherConst", self, "_double", 21), 42)
	_check_eq("with no arguments",
			node.call("CallOtherConstNoArgs", self, "_say"), "said")
	_check_eq("with an argument tail it built",
			node.call("CallOtherConstTwo", self, "_add_two", 20, 22), 42)
	_check_eq("and through the godot_array spelling",
			node.call("CallvOtherConst", self, "_double", [21]), 42)
	
	# A <reads> function calling a <reads> function that reaches Godot, which is the cascade this
	# closes: before CallConst the leaf forced <transacts> on every caller above it.
	_check_eq("a <reads> function calls a <reads> function that reaches Godot",
			node.call("SumOtherConst", self, "_double", 10, 11), 42)

	# --- R-INT-7: the same GDScript class as a *declared type* -----------------------------------
	#
	# `res://mob.gd` is ordinary GDScript with a `class_name` and no knowledge of Verse.
	# `res://scripts/bindings.verse` names `mob`, which nothing on disk declares: the editor read
	# `Mob` out of Godot's global class list, described it from `get_script_method_list`, emitted
	# Verse for it and handed that to the host as a package of its own. So the first assertion is
	# that the fixture *compiled at all* -- a binding that did not generate is an unknown
	# identifier, not a wrong answer.
	# The *cast* is editor-only until R-INT-11 carries the class-to-binding table in the sidecar.
	# A cooked game compiles the bindings package -- the export plugin writes it and the cooker
	# is handed it -- so `bindings.verse` is in the export and this fixture proves it compiled
	# there. What an exported host cannot yet do is *key* a crossing handle on it, because the
	# table lives only in the editor host's memory.
	var binding_script: Script = ResourceLoader.load("res://scripts/bindings.verse")
	if not editor:
		_check("a script naming a generated binding compiles", binding_script != null and binding_script.can_instantiate())
		_skip("a Verse script casts a node to its GDScript class's binding", "the sidecar carries no binding table yet (R-INT-11)")
		_skip("and reaches a method whose declared result type is GDScript's", "the sidecar carries no binding table yet (R-INT-11)")
		_skip("and a bool answers a logic rather than a <decides>", "the sidecar carries no binding table yet (R-INT-11)")
		_skip("a node carrying no such script declines the cast", "the sidecar carries no binding table yet (R-INT-11)")
		_skip("a method taking its own GDScript class is in the binding", "the sidecar carries no binding table yet (R-INT-11)")
		_skip("and one taking a class the mirror carries", "the sidecar carries no binding table yet (R-INT-11)")
		_skip("and null is a value that parameter takes", "the sidecar carries no binding table yet (R-INT-11)")
		_skip("null satisfies an optional parameter of a binding class", "the sidecar carries no binding table yet (R-INT-11)")
		_skip("and a node carrying that GDScript satisfies it", "the sidecar carries no binding table yet (R-INT-11)")
		_skip("a GDScript var is reached through the binding's accessor pair", "the sidecar carries no binding table yet (R-INT-11)")
		_skip("and written through it", "the sidecar carries no binding table yet (R-INT-11)")
		_skip("which the next call reads back", "the sidecar carries no binding table yet (R-INT-11)")
		_skip("including a string, which a member could not have carried", "the sidecar carries no binding table yet (R-INT-11)")
		_skip("a GDScript const is in the class's statics module", "the sidecar carries no binding table yet (R-INT-11)")
		_skip("and a string one", "the sidecar carries no binding table yet (R-INT-11)")
		_skip("a GDScript static is called through the script resource", "the sidecar carries no binding table yet (R-INT-11)")
		_skip("a GDScript enum is a Verse enum with Godot's numbers", "the sidecar carries no binding table yet (R-INT-11)")
		_skip("an enum crosses as an argument and comes back as a result", "the sidecar carries no binding table yet (R-INT-11)")
		_skip("and an enum-typed var reads through the pair", "the sidecar carries no binding table yet (R-INT-11)")
		_skip("and writes through it", "the sidecar carries no binding table yet (R-INT-11)")
	elif binding_script == null or not binding_script.can_instantiate():
		_check("a script naming a generated binding compiles", false)
	else:
		_check("a script naming a generated binding compiles", true)
		var binder := Node2D.new()
		binder.set_script(binding_script)
		tree.root.add_child(binder)

		var mob_node: Node2D = Mob.new()
		tree.root.add_child(mob_node)

		# The cast is Verse's own downcast over R-SCN-6, with no new machinery: the handle
		# crosses in and `ObjectForHandle` asks whether a script on it names a binding before
		# it asks what `get_class()` says -- which answers RigidBody2D here and would give a
		# mirror wrapper the cast could never succeed against.
		_check_eq("a Verse script casts a node to its GDScript class's binding",
				binder.call("AskCast", mob_node), 42)
		_check_eq("and reaches a method whose declared result type is GDScript's",
				binder.call("AskLabel", mob_node), "mob")
		_check("and a bool answers a logic rather than a <decides>",
				binder.call("AskHeavy", mob_node))

		# An object, in both directions. `mate` is annotated with the GDScript class's own name,
		# which no mirror carries and only the binding beside it declares, and a method the
		# generator cannot type is left out of the binding entirely -- so `Mob.mate` was simply
		# absent, and `Mob.place`, whose `Node2D` the mirror does carry, was emitted answering a
		# `variant` where it had declared a `node2d` and took the whole project's build down with
		# it (`by-hand-findings.md` B35).
		_check_eq("a method taking its own GDScript class is in the binding",
				binder.call("AskMate", mob_node), 42)
		_check_eq("and one taking a class the mirror carries",
				binder.call("AskPlace", mob_node), String(mob_node.name))
		# `mate` returns what it is given, so null in is a declined downcast out -- which is the
		# whole of what an optional object parameter buys a script.
		_check("and null is a value that parameter takes",
				binder.call("AskMateNothing", mob_node))

		# The same class as a *script's* own parameter, which is the host's argument wire rather
		# than the generated package.
		_check("null satisfies an optional parameter of a binding class",
				binder.call("AskMaybeMobEmpty", null))
		_check_eq("and a node carrying that GDScript satisfies it",
				binder.call("AskMaybeMobLabel", mob_node), "mob")

		# --- R-INT-9: what a binding carries besides its methods ----------------------------
		#
		# A GDScript `var` as an accessor pair. The names are the generator's, because a
		# GDScript property has none of its own and the Verse member spelling would cost the
		# class its archetype: a member with `<getter>`/`<setter>` must be uninitialized, and
		# every archetype of the class would then have to initialize it.
		_check_eq("a GDScript var is reached through the binding's accessor pair",
				binder.call("AskSpeed", mob_node), 1.5)
		# The write is checked on Godot's side and on the next call, never in the same one: a
		# write to Godot defers to the transaction's commit, so the computation that made it
		# does not see it.
		binder.call("AskSetSpeed", mob_node, 3.25)
		_check_eq("and written through it", mob_node.speed, 3.25)
		_check_eq("which the next call reads back", binder.call("AskSpeed", mob_node), 3.25)
		# A `string` var, which the member spelling could not have carried at all: `string` is
		# `[]char`, and a container-typed member is asked for indexed accessors.
		_check_eq("including a string, which a member could not have carried",
				binder.call("AskTag", mob_node), "m")

		# A constant and a static are not members of the class: Verse has data on no type and
		# has no `static`, so both live in a module named after the class, the way the mirror
		# puts Godot's own in `NodeStatics`.
		_check_eq("a GDScript const is in the class's statics module",
				binder.call("AskLimit"), 7)
		_check_eq("and a string one", binder.call("AskTitle"), "mob")
		# Dispatched through the script *resource*: a script class has no ClassDB entry, so
		# `ClassDB.class_call_static` -- what a bound GDExtension class uses -- cannot reach it.
		_check_eq("a GDScript static is called through the script resource",
				binder.call("AskSpawnCost", 3), 21)

		# A GDScript enum is a real Verse enum at the package's module scope, with `ToInt` as
		# its public conversion -- the same spelling the mirror gives its own 793.
		_check_eq("a GDScript enum is a Verse enum with Godot's numbers",
				binder.call("AskEnumValue"), 2)
		_check_eq("an enum crosses as an argument and comes back as a result",
				binder.call("AskShift", mob_node), 1)
		_check_eq("and an enum-typed var reads through the pair",
				binder.call("AskMode", mob_node), 0)
		binder.call("AskSetMode", mob_node)
		_check_eq("and writes through it", int(mob_node.mode), 2)

		# A node with no such script declines the cast rather than answering something. The
		# binding is a *type*, so this is the compiler's own failure and costs nothing at all.
		var plain := Node2D.new()
		tree.root.add_child(plain)
		_check_eq("a node carrying no such script declines the cast",
				binder.call("AskCast", plain), -1)
		plain.queue_free()
		mob_node.queue_free()
		binder.queue_free()

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
		tree.root.add_child(entity)

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
	if not editor:
		# There is no second generation in an export: the project was compiled by the cooker and
		# `build_project` is one of the eleven entry points a runtime host refuses. What the export
		# proves instead is that the *first* generation loads at all, which is every other case here.
		_skip("a second build publishes a new generation")
		_skip("a node attached before it still answers")
		_skip("and its script is still valid")
		_skip("and a node attached after it answers too")
	else:
		_check_eq("a second build publishes a new generation", VerseRuntime.build_project(), OK)
		_check_eq("a node attached before it still answers", node.call("EchoInt", 7), 7)
		_check("and its script is still valid", script.can_instantiate())
		var after_build := Node2D.new()
		after_build.set_script(script)
		tree.root.add_child(after_build)
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
		tree.root.add_child(left_node)
		var right_node := Node2D.new()
		right_node.set_script(right)
		tree.root.add_child(right_node)
		_check_eq("the left one runs its own code", left_node.call("Which"), "left")
		_check_eq("and the right one runs its own", right_node.call("Which"), "right")
		# helpers.verse is at the project root, and nothing in widget.verse imports it: a module
		# reads the root module by ordinary lexical scoping.
		_check_eq("a module reaches a root definition with nothing imported",
				left_node.call("RootConstant"), 42)

		# An `@export` typed as a `@global_class` Resource *in a module*. The slot is filtered by
		# the name Godot knows the class as, which is flat -- ClassDB has one namespace and a
		# module is deliberately not in it. Carrying the bridge's own `left/palette` here named a
		# class nothing had registered, and the inspector said "Cannot get class 'Left/Palette'".
		var skin := {}
		for p in left_node.get_property_list():
			if p["name"] == "Skin":
				skin = p
		_check("a module class exports a reference to another class in its module", not skin.is_empty())
		if not skin.is_empty():
			_check_eq("the slot is filtered by the registered class name, with no module in it",
					skin.get("hint_string", ""), "Palette")
			_check_eq("and it is a resource picker", skin.get("hint", -1),
					PROPERTY_HINT_RESOURCE_TYPE)
			# The invariant that broke, stated directly: the name the slot filters by and the name
			# the class registers under are the same string. They are produced by two different
			# code paths from two different inputs, which is how they came to disagree.
			#
			# Editor only, and for a reason worth knowing rather than worked around: `get_global_name`
			# is read out of the *source text*, and an exported game ships every `.verse` as a
			# one-byte stub, so it answers nothing there. It costs an export nothing, because the
			# registry an export uses was baked into `project.godot` when it was made.
			if editor:
				var palette: Script = load("res://widgets/left/palette.verse")
				_check_eq("and it is the very name the class registers under",
						skin.get("hint_string", ""),
						String(palette.get_global_name()) if palette != null else "")
			else:
				_skip("and it is the very name the class registers under",
						"get_global_name reads the source text, which an export strips to a stub")

	# --- R-EXP-5: @tool ------------------------------------------------------------------------
	#
	# The attribute is the bridge's own, declared in the package the host adds at runtime, so
	# tool_probe.verse compiling is what says it resolves. Whether Ready runs in the editor is not
	# a question a headless run can ask -- there is no editor to be a hint of -- but the answer
	# Godot acts on is is_tool(), and that is answered here.
	var tool_script: Script = load("res://scripts/tool_probe.verse")
	_check("a @tool script compiles", tool_script != null and tool_script.can_instantiate())
	if not editor:
		# `is_tool()` is read off the *source*, the way `_get_global_class_name` is, because Godot
		# asks both of scripts it has merely scanned. An export ships every `.verse` as a one-byte
		# stub (7a D10), so there is no `@tool` left to find -- and nothing is lost by it: what the
		# flag decides is whether the editor hands out a real instance or a placeholder, and an
		# exported game is never `is_editor_hint()`.
		_skip("and reports itself a tool script", "the source is a stub in an export")
	elif tool_script != null:
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
		tree.root.add_child(probe)
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

	# --- R-SCN-6: a singleton crosses as the class its name says --------------------------------
	#
	# Two of the 41 accessors could never succeed, and for a reason `<decides>` hid: the object Godot
	# hands back is of a *driver* class -- IPWindows, GodotNavigationServer2D -- which is in ClassDB
	# because GDCLASS puts it there on first construction, and in no extension_api.json because it is
	# not exposed. The mirror has a row only for what the dump carries, so the handle resolved to
	# nothing and every cast declined. Asserted against Godot's own answer rather than against the
	# literal "IPWindows", which is a driver name and not a promise.
	var singleton_script: Script = load("res://scripts/singletons.verse")
	_check("singletons.verse compiles", singleton_script != null and singleton_script.can_instantiate())
	if singleton_script != null:
		var singles := Node2D.new()
		singles.set_script(singleton_script)
		tree.root.add_child(singles)
		_check("Godot reports the NavigationServer2D singleton as a class of its own",
				Engine.get_singleton("NavigationServer2D").get_class() != "NavigationServer2D")
		_check_eq("and the accessor answers that object anyway",
				singles.call("NavigationServer2dClass"),
				Engine.get_singleton("NavigationServer2D").get_class())
		# IP is the other one of the two, and the accessor is deliberately not called: asking Godot
		# for that singleton segfaults the process at exit, on master as well (B21). The premise is
		# still worth asserting, because it is what makes IP the second case.
		_check("IP is the same shape, which is why it is the other one",
				Engine.get_singleton("IP").get_class() != "IP")
		_check_eq("while a singleton registered under its own class is unchanged",
				singles.call("EngineClass"), "Engine")
		# What the two remaining `<decides>` accessors buy. EditorInterface is registered only by a
		# build that starts an editor, so this run and an exported one are both without it, and the
		# script handles that and answers. GDScript cannot get this far: `EditorInterface` is an
		# identifier a TOOLS_ENABLED build registers, so an export template refuses the whole script
		# at load rather than the one line that named it.
		_check_eq("an editor singleton a game cannot have is a handled absence, not a dead script",
				singles.call("EditorInterfaceClass"), "absent")

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
		tree.root.add_child(virt)

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

		tree.root.remove_child(virt)
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
		tree.root.add_child(ctrl)
		_check_eq("Godot asks a value-returning virtual and acts on the answer",
				ctrl.get_minimum_size(), Vector2(73, 31))
		tree.root.remove_child(ctrl)
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
	tree.root.add_child(quiet)
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
		tree.root.add_child(emitter_node)

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

		if by_name.has("Touched"):
			# A signal argument is a vh_param_desc like a method's, so it answers the same
			# question -- and this is the one the connect dialog reads to say what a handler
			# receives.
			var touched_args: Array = by_name["Touched"]["args"]
			_check_eq("an object payload is one argument", touched_args.size(), 1)
			if touched_args.size() == 1:
				_check_eq("named as the Godot class it carries",
						String(touched_args[0]["class_name"]), "Node2D")
		else:
			_check("an object payload is one argument", false)

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

		# An emission raised by *GDScript* on this node reaches a Verse subscriber. The member is an
		# `event(t)`, whose Godot connection is made once at construction rather than for the length
		# of one wait -- a bare event offers no hook at the await to connect from -- so this works
		# with nothing awaiting and is the half of R-SIG-6 that connection model has to keep.
		# `Struck` rather than `Scored`, which the subscription cases below count connections on:
		# this one leaves a subscription behind on purpose, to show it keeps receiving.
		emitter_node.call("ResetSeen")
		emitter_node.call("SubscribeToStruck")
		emitter_node.emit_signal("Struck", 9, "spike")
		_check_eq("a GDScript emission of a declared signal reaches a Verse handler",
				[emitter_node.call("ReadSeen"), emitter_node.call("ReadSeenBy")], [9, "spike"])

		# --- the other spelling, supported alongside --------------------------
		#
		# `signal(t)` declares a signal too. Both are kept because they are not interchangeable --
		# this one satisfies `listenable(t)` and connects only for the length of a wait, an
		# `event(t)` satisfies `awaitable(t)` and holds one connection for the instance's life --
		# and the claim here is that Godot cannot tell them apart.
		_check("a signal(t) member reaches get_signal_list beside the event ones",
				by_name.has("Chimed"))
		if by_name.has("Chimed"):
			var chimed_args: Array = by_name["Chimed"]["args"]
			_check_eq("with the same one argument an event payload of that type gets",
					chimed_args.size(), 1)

		_signal_points = 0
		emitter_node.connect("Chimed", _on_verse_scored)
		emitter_node.call("EmitChimed", 4)
		_check_eq("Signal on a signal(t) member reaches a GDScript handler", _signal_points, 4)

		emitter_node.call("ResetSeen")
		emitter_node.call("SubscribeToChimed")
		emitter_node.emit_signal("Chimed", 5)
		_check_eq("and a GDScript emission of one reaches a Verse handler",
				emitter_node.call("ReadSeen"), 5)

		# Its connection is scoped to the subscription rather than to the instance, which is the
		# property the event spelling trades away: no OWN_CONNECTION term here.
		_check_eq("a signal(t) member carries no connection of its own",
				emitter_node.get_signal_connection_list("Chimed").size(), 2)

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
		tree.root.add_child(subscriber)
		emitter_node.connect("Scored", Callable(subscriber, "OnScored"))
		tree.root.remove_child(subscriber)
		subscriber.free()
		emitter_node.call("EmitScored", 1)
		_check("emitting after the subscriber was freed is not an error", true)

		# --- access levels: `@export_signal` is the whole gate -----------------
		#
		# None of these four carries `<public>`, and all four register. The bridge used to refuse
		# them, on the reading that connecting is done from outside the class -- but connecting is
		# not done from Verse at all: the Node panel connects by name and so does GDScript, and
		# neither consults a Verse specifier. What a specifier governs is which Verse code may name
		# the member, and it buys no privacy from Godot: each of these is connected and emitted from
		# here, which is the whole claim.
		var access_names := {}
		for entry in emitter_node.get_signal_list():
			access_names[String(entry["name"])] = entry
		_check("a member with no access specifier registers", access_names.has("Unspecified"))
		_check("and a `<protected>` one", access_names.has("Guarded"))
		_check("and a `<private>` one", access_names.has("Own"))
		_check("and a `<private>` signal(t), whose binding id the host writes into the member",
				access_names.has("Quiet"))

		_signal_points = 0
		emitter_node.connect("Unspecified", _on_verse_scored)
		emitter_node.call("EmitUnspecified", 3)
		_check_eq("an unspecified member emits to a GDScript handler", _signal_points, 3)
		emitter_node.connect("Guarded", _on_verse_scored)
		emitter_node.call("EmitGuarded", 4)
		_check_eq("a `<protected>` one does too", _signal_points, 4)
		emitter_node.connect("Own", _on_verse_scored)
		emitter_node.call("EmitOwn", 5)
		_check_eq("and a `<private>` one", _signal_points, 5)
		emitter_node.connect("Quiet", _on_verse_scored)
		emitter_node.call("EmitQuiet", 6)
		_check_eq("and a `<private>` signal(t)", _signal_points, 6)

		# Delivery *back* into the member, which is the half a non-public event could have lost on
		# its own: the connection ConnectDelivery makes is by name off the installed script
		# instance, and the name is all Godot ever had.
		emitter_node.call("ResetSeen")
		emitter_node.call("SubscribeToOwn")
		emitter_node.emit_signal("Own", 7)
		_check_eq("a GDScript emission of a `<private>` signal reaches a Verse handler",
				emitter_node.call("ReadSeen"), 7)

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
		_check_eq("nor one whose struct payload nests a struct", reject_names.has("Nested"), false)
		_check_eq("nor one whose payload has no Godot type", reject_names.has("Maybe"), false)

		var reject_node := Node2D.new()
		reject_node.set_script(reject_script)
		tree.root.add_child(reject_node)
		_check("has_signal agrees with the list", reject_node.has_signal("Fine"))
		_check_eq("and refuses the rejected one", reject_node.has_signal("Reassignable"), false)
		_signal_points = 0
		reject_node.connect("Fine", _on_verse_scored)
		reject_node.call("EmitFine", 6)
		_check_eq("the good signal on that class still emits", _signal_points, 6)
		# Emitting a refused signal reports its own reason rather than the generic "names nothing".
		# The *text* is what matters and GDScript cannot read push_error output, so what is asserted
		# here is that none of them is fatal; the sentences are eyeballed in the run log and recorded
		# in phase-4-gaps.md §2.
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
		tree.root.add_child(mx)

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

		tree.root.remove_child(mx)
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
	# What the refusal looks like from GDScript depends on the build, and the assertion must not:
	# in a debug build the invalid call aborts the statement, leaving "ran" behind, and in an export
	# template it answers null and carries on. Either way the Verse method did not run, which is the
	# claim -- and the one thing it could never be is 7.
	var answered: bool = typeof(_thread_answer) == TYPE_INT and _thread_answer == 7
	_check_eq("but the Verse method it called did not", answered, false)
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
		tree.root.add_child(caster)

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

	# --- R-NODE-3: an object that is not a node --------------------------------------------------
	#
	# host_smoke counts the peers; what only a real Godot can say is that what `helper{}` produced
	# is a real Godot object -- valid, of the class the mirror walked up to, and the *same* object
	# on the way back rather than a fresh wrapper around its handle.
	var objects_script: Script = load("res://scripts/plain_objects.verse")
	_check("plain_objects.verse compiles", objects_script != null and objects_script.can_instantiate())
	if objects_script != null:
		var maker := Node2D.new()
		maker.set_script(objects_script)
		tree.root.add_child(maker)

		_check_eq("a Verse class instantiates with no node anywhere in it",
				maker.call("MakeAndUse"), 3)

		var minted: Object = maker.call("MakeAndKeep")
		_check("and what comes back is a Godot object", minted != null)
		_check_eq("of the Godot class the mirror walked up to",
				minted.get_class() if minted != null else "", "RefCounted")
		_check("which Godot considers valid", is_instance_valid(minted))

		# Identity. A fresh mirror wrapper of the same handle would be a `ref_counted` and the
		# script's own downcast would decline, which is what -1 means here.
		_check_eq("handing it back reaches the very object Verse minted",
				maker.call("BumpThrough", minted), 42)
		_check_eq("and the script's own reference saw that write",
				maker.call("ReadHeld"), 42)

		maker.call("DropHeld")
		minted = null
		_check("dropping it is not an error", true)

		# The other lifetime: a node, which the tree owns from the moment it is parented.
		maker.call("MakeChild", "VerseMade")
		_check("a node a Verse script made can be added to the tree",
				maker.get_node_or_null("VerseMade") != null)

	# --- R-EXP-6: a Verse class as a custom Resource ---------------------------------------------
	#
	# The runtime half of the round-trip, which is the half that can be automated: a Resource with a
	# Verse script, its exported values written and read, saved to `.tres` and loaded back with its
	# values and its methods intact. The editor half -- the New Resource dialog and the inspector --
	# is a by-hand check, because nothing a script can ask reaches either.
	var res_script: Script = load("res://scripts/settings_resource.verse")
	_check("settings_resource.verse compiles", res_script != null and res_script.can_instantiate())
	if res_script != null:
		_check_eq("a Verse resource class reports Resource as its base",
				res_script.get_instance_base_type(), &"Resource")

		var settings := Resource.new()
		settings.set_script(res_script)
		_check("a Verse script attaches to a Resource", settings.get_script() == res_script)
		_check_eq("its exported defaults are readable", settings.get("Rounds"), 3)

		# B19. `stowaway` carries `@global_class` and registers nothing, because Godot collects one
		# global class per script *path* and this is a second class in the file. The member is
		# exported anyway, filtered by its nearest mirrored ancestor -- a Resource picker that
		# accepts a .tres of that class, rather than no slot at all or a slot naming a class ClassDB
		# has never heard of, which is the error that started this.
		var stowaway := {}
		for p in settings.get_property_list():
			if p["name"] == "Stowaway":
				stowaway = p
		_check("a member typed as an unregisterable class is still exported", not stowaway.is_empty())
		if not stowaway.is_empty():
			_check_eq("filtered by its nearest mirrored Godot class",
					stowaway.get("hint_string", ""), "Resource")
			_check_eq("and drawn as a resource picker", stowaway.get("hint", -1),
					PROPERTY_HINT_RESOURCE_TYPE)
			_check("which ClassDB can resolve, unlike the name the class failed to register",
					ClassDB.class_exists(String(stowaway.get("hint_string", ""))))

		# B19 A2. A1 widened the picker to every `Resource`, so whether that is parity or a hole
		# turns entirely on the *write* being narrow. Both refusals live in `vh_instance_set_field`
		# and this reaches both, on its own instance so that nothing below inherits what it wrote.
		#
		# Read back through Verse rather than through `get()`: a bad write and a matching bad read
		# agree with each other, and only the interpreter reading the slot says the value is really
		# of the class the compiled code expects.
		var writes := Resource.new()
		writes.set_script(res_script)

		# The script-class member first. Only Verse can put a legal value in this slot -- `stowaway`
		# is not the class named after its file, so no Godot object ever carries it as a script --
		# which is exactly what gives the refusal something to preserve.
		writes.call("Stow")
		_check_eq("Verse can fill a slot no Godot object could", writes.call("StowedValue"), 5)

		# A plain resource takes the handle path, because it carries no Verse script instance for
		# the other one. The host builds a wrapper for a handle only when the declared class is a
		# mirrored one, and `stowaway` is the project's own, so this is refused before any class
		# comparison happens at all.
		writes.set("Stowaway", Resource.new())
		_check_eq("a bare resource offered to a script-class slot is refused, old value kept",
				writes.call("StowedValue"), 5)

		# The same slot through the other entry point. A resource that *does* carry a Verse script
		# takes `vh_instance_set_field_instance`, where the check is the declared class against the
		# instance's own -- and `settings_resource` is not a `stowaway`.
		var wrong_class := Resource.new()
		wrong_class.set_script(res_script)
		writes.set("Stowaway", wrong_class)
		_check_eq("and so is an instance of a different Verse class",
				writes.call("StowedValue"), 5)

		# The second refusal: a *mirrored* member, where the declared class is one ClassDB knows and
		# the test is `IsA` rather than "is it mirrored at all". `host_smoke` cannot reach this one
		# -- its harness answers no class for any handle, so the declared class is used as the
		# fallback and every handle matches it. Only a Godot that knows what a handle is can fail it.
		writes.set("Palette", Curve.new())
		_check("a mirrored slot refuses a handle naming the wrong class",
				writes.call("PaletteIsSet") == false)
		writes.set("Palette", Gradient.new())
		_check("and takes one naming the right class", writes.call("PaletteIsSet") == true)

		# B19 Stage C. What survives a save, which is the measurement C1's rule rests on -- and the
		# half worth guarding is the *positive* one: a mirrored reference member round-trips, so the
		# loss below is specific to a second class in the file rather than general to references.
		#
		# The reason is structural. A Verse object's members live in the VM and its Godot peer
		# carries none of them; what bridges the two is being a *script*, and by R-LANG-6 only the
		# class named after the file can be one. `stowaway`'s peer is therefore a bare `Resource`
		# with nothing on it, and an empty sub-resource is all ResourceSaver has to write.
		const C_SAVED := "user://stage_c_probe.tres"
		_check_eq("a resource holding both kinds of reference saves",
				ResourceSaver.save(writes, C_SAVED), OK)
		var reread: Resource = ResourceLoader.load(C_SAVED, "", ResourceLoader.CACHE_MODE_IGNORE)
		_check("and loads back", reread != null)
		if reread != null:
			_check("a mirrored reference member survives the round trip",
					reread.call("PaletteIsSet") == true)
			_check("and comes back as a real Gradient", reread.get("Palette") is Gradient)
			# Recorded, not desired: C1 is the rule that keeps a project out of this, and Stage B's
			# warning is what says so at the attribute. If this ever starts passing a value back,
			# C2 was built and property-export.md §"Stage C" is the section to correct.
			_check_eq("a second-class member does not, which is why C1 is the rule",
					reread.call("StowedValue"), -1)

		_check_eq("and a method runs on an owner that is not a node",
				settings.call("Describe"), "untitled x3")

		settings.set("Title", "campaign")
		settings.set("Rounds", 7)
		settings.set("Speed", 2.5)
		_check_eq("a write through the inspector's own path lands",
				settings.call("Describe"), "campaign x7")

		const SAVED := "user://settings_probe.tres"
		_check_eq("saving it to .tres succeeds", ResourceSaver.save(settings, SAVED), OK)

		# take_over_path is off and the cache is bypassed, or `load` would hand back the very
		# object just saved and the round trip would assert nothing.
		var loaded: Resource = ResourceLoader.load(SAVED, "", ResourceLoader.CACHE_MODE_IGNORE)
		_check("it loads back", loaded != null)
		if loaded != null:
			_check("and carries the script it was saved with", loaded.get_script() == res_script)
			_check_eq("with its exported values", loaded.get("Title"), "campaign")
			_check_eq("every one of them", loaded.get("Rounds"), 7)
			_check_eq("floats included", loaded.get("Speed"), 2.5)
			_check_eq("and its methods run against the loaded values",
					loaded.call("Describe"), "campaign x7")
			_check_eq("a method that writes a member still writes it", loaded.call("Bump"), 8)

		# A `.tres` the project *ships* rather than one this run just wrote, which is the shape a
		# real project has and the only one that says anything about an export: there the `.verse`
		# it names is stripped to a one-byte stub, so `ext_resource path=` resolving at all is the
		# claim. The class behind it comes from the cooked Verse, not from the file on disk.
		var shipped: Resource = load("res://resources/shipped_settings.tres")
		_check("a .tres the project ships loads", shipped != null)
		if shipped != null:
			_check_eq("with the values it was saved with", shipped.get("Title"), "shipped")
			_check_eq("and its script bound to it", shipped.call("Describe"), "shipped x11")

	# --- R-EXP-7: a Verse script as an autoload singleton ----------------------------------------
	#
	# Godot's own rules decide this and the bridge follows them. `_create_autoload` refuses a script
	# whose `get_instance_base_type()` is not a Node (`editor_autoload_settings.cpp:354-355`), and
	# that answer is the bridge's to give -- so the gate itself is testable in both runs, on both
	# sides of it, without naming a bad autoload in `project.godot` and stopping the project.
	var autoload_script: Script = load("res://scripts/game_state.verse")
	_check("game_state.verse compiles", autoload_script != null and autoload_script.can_instantiate())
	if autoload_script != null:
		_check_eq("a `class(node)` reports the base type an autoload needs",
				autoload_script.get_instance_base_type(), &"Node")
		_check("which is the exact test _create_autoload applies",
				ClassDB.is_parent_class(autoload_script.get_instance_base_type(), "Node"))
	if res_script != null:
		_check("and a `class(resource)` reports one that test refuses",
				not ClassDB.is_parent_class(res_script.get_instance_base_type(), "Node"))

	# --- R-NODE-10: what the object calls itself ------------------------------------------------
	#
	# The Verse spelling is an extension method, not a `_ToString` override -- there is no such
	# virtual and deliberately so, because a class member of that name cannot be declared at all.
	# game_state.verse writes one; every other fixture writes none, which is the control.
	#
	# Asserted on a plain instance rather than on the autoload, so it runs in both runs: nothing
	# about this needs a singleton.
	if autoload_script != null:
		var printable := Node.new()
		printable.set_script(autoload_script)
		tree.root.add_child(printable)
		_check_eq("a Verse ToString extension method is what str() shows",
				str(printable), "game_state(fresh 0)")
		printable.call("Rename", "named")
		_check_eq("and it is re-read rather than cached",
				str(printable), "game_state(named 0)")
		# The round trip that makes one implementation serve both languages: Verse's own "{Obj}"
		# desugars to the free ToString(Obj), which is the mirror's ToString(:object), which calls
		# Godot's to_string() -- which lands back on the extension method above.
		_check_eq("and Verse's own interpolation arrives at it through Godot",
				printable.call("DescribeSelf"), "game_state(named 0)")
		printable.queue_free()

	# A script that writes no ToString must keep Godot's own representation rather than gain an
	# empty one -- `r_is_valid = false` is the difference, and it is the common case.
	if res_script != null:
		var plain := Resource.new()
		plain.set_script(res_script)
		_check("a script with no ToString keeps Godot's own text",
				str(plain).contains("Resource"))

	# The singleton itself is the *exported* run's alone, and not because of anything about Verse:
	# `--script` replaces the main loop before Godot sets any autoload up. Measured rather than
	# assumed -- under the editor-side driver `/root` has no children at all, not even this suite's
	# own `VerseExportCheck`. Skipped with that reason rather than dropped.
	if editor:
		const AUTOLOAD_WHY := "--script replaces the main loop, so no autoload is set up"
		_skip("the Verse autoload is in the tree", AUTOLOAD_WHY)
		_skip("it carries its Verse script", AUTOLOAD_WHY)
		_skip("its methods answer through the singleton", AUTOLOAD_WHY)
		_skip("and a write through it is what the next read sees", AUTOLOAD_WHY)
		_skip("and it hangs off the root rather than the scene", AUTOLOAD_WHY)
	else:
		var game_state: Node = tree.root.get_node_or_null("GameState")
		_check("the Verse autoload is in the tree", game_state != null)
		if game_state == null:
			_skip("it carries its Verse script", "no autoload node")
			_skip("its methods answer through the singleton", "no autoload node")
			_skip("and a write through it is what the next read sees", "no autoload node")
			_skip("and it hangs off the root rather than the scene", "no autoload node")
		else:
			_check("it carries its Verse script", game_state.get_script() != null)
			_check_eq("its methods answer through the singleton",
					game_state.call("Describe"), "fresh 0")
			game_state.call("Rename", "run")
			game_state.call("AddScore", 7)
			# Fetched again rather than reusing the handle, because what a singleton is for is that
			# the next lookup finds the same object rather than a fresh one.
			var looked_up_again: Node = tree.root.get_node_or_null("GameState")
			_check_eq("and a write through it is what the next read sees",
					looked_up_again.call("Describe"), "run 7")
			# What makes it *every* scene's rather than this one's, and the only part of "answers
			# from every scene" a single-scene run can honestly assert: it is a child of the root,
			# beside the current scene rather than inside it, so a scene change does not touch it.
			_check("and it hangs off the root rather than the scene",
					game_state.get_parent() == tree.root and game_state != tree.current_scene)

	# --- R-NODE-10: the four property hooks -----------------------------------------------------
	#
	# The half of R-NODE-10 that lets a class serve a property nothing declares. None of the four is
	# in extension_api.json -- Godot offers them to scripts rather than registering them in ClassDB
	# -- so they are hand-written on the native root with empty bodies, and `resolve` finding only
	# what a class *declares* is what keeps a script that overrides none of them out of the VM.
	var hooks_script: Script = load("res://scripts/hooks.verse")
	_check("hooks.verse compiles", hooks_script != null and hooks_script.can_instantiate())
	if hooks_script == null:
		_check("a property no member declares is served by _Get", false)
	else:
		var hooked := Node2D.new()
		hooked.set_script(hooks_script)
		tree.root.add_child(hooked)

		# _Get, for a name Godot's own lookup missed. `get()` is the same path the inspector, the
		# scene packer and any reflective tool take.
		_check_eq("a property no member declares is served by _Get", hooked.get("Virtual"), 7)
		_check_eq("and one of another type", hooked.get("Label"), "from _Get")
		# A `variant` holding nothing is what the hook answers for "not mine", and it arrives as the
		# same nil a missing member does -- which is the right answer either way.
		_check_eq("while a name it does not serve stays absent", hooked.get("NoSuchThing"), null)

		# _Set, and the write reaching the member the hook chose rather than a property of its own.
		hooked.set("Virtual", 21)
		_check_eq("_Set takes a write no member took", hooked.call("ReadVirtual"), 21)
		_check_eq("and the read that follows is the value it stored", hooked.get("Virtual"), 21)
		hooked.set("Label", "written")
		_check_eq("and a string through the same hook", hooked.call("ReadLabel"), "written")

		# The composition rule, which is the design question inside this stage: an @export and a
		# hook that both claim one name. Godot consults `_get`/`_set` only for a name the property
		# list did not carry, so the export wins and the hook is never asked -- which is what stops
		# the two from being a silent race.
		var set_calls_before: int = hooked.call("ReadSetCalls")
		hooked.set("Declared", 33)
		_check_eq("an @export is written by the export machinery", hooked.call("ReadDeclared"), 33)
		_check_eq("and read back by it", hooked.get("Declared"), 33)
		_check_eq("so _Set never sees an exported name",
				hooked.call("ReadSetCalls"), set_calls_before)

		# _GetPropertyList, appended after the declared members. This is the list a running game
		# walks -- what PackedScene::pack and Object::get_property_list ask -- rather than the
		# inspector's, which a non-tool script answers with a placeholder.
		var hook_names: Array = []
		var hook_types := {}
		for entry in hooked.get_property_list():
			hook_names.append(entry["name"])
			hook_types[entry["name"]] = entry["type"]
		_check("_GetPropertyList adds a property the class does not declare", hook_names.has("Virtual"))
		_check("and a second one", hook_names.has("Label"))
		_check_eq("with the Variant type the script chose", hook_types.get("Virtual"), TYPE_INT)
		_check_eq("and for the other", hook_types.get("Label"), TYPE_STRING)
		_check("while the exported member is still listed", hook_names.has("Declared"))

		# _ValidateProperty, which Godot asks of every ClassDB property of the object, one at a
		# time. The dictionary is a reference, so what Verse writes is what Godot reads back -- and
		# hiding Node2D's own `rotation` is a change no other part of this suite could make.
		_check("_ValidateProperty ran", hooked.call("ReadValidated") > 0)
		var rotation_usage := -1
		for entry in hooked.get_property_list():
			if entry["name"] == "rotation":
				rotation_usage = entry["usage"]
		_check_eq("and the usage it wrote is what Godot read back", rotation_usage, 0)

		hooked.queue_free()

	# A script that overrides none of the four is never asked about any of them, which is what makes
	# them free: `marshal.verse` declares no hook, so a name nothing declares stays absent rather
	# than reaching a VM entry that would answer nothing.
	_check_eq("a script with no hooks answers nothing for an unknown property",
			node.get("NoSuchThingAtAll"), null)


	# --- R-EXP-9: @rpc, and the config Godot reads it out of ------------------------------------
	#
	# The receiving half. `Script.get_rpc_config()` is bound in ClassDB, so what Godot's own
	# SceneRPCInterface walks is exactly what a test can read -- which is the whole of what makes
	# this assertable without a second peer. The Dictionary's keys are method names and each value
	# carries Godot's own four fields with Godot's own numbering.
	var rpc_script: Script = load("res://scripts/rpcs.verse")
	_check("rpcs.verse compiles", rpc_script != null and rpc_script.can_instantiate())
	if rpc_script == null:
		_check("a method with @rpc reaches the rpc config", false)
	else:
		var rpc_config: Dictionary = rpc_script.get_rpc_config()
		_check("a method with @rpc reaches the rpc config", rpc_config.has("TakeDamage"))
		# Absence is how "not remote-callable" is spelled: Godot reads the keys it is given and
		# nothing else, so a method with no attribute must not be a key at all.
		_check("and a method without one is absent", not rpc_config.has("Ordinary"))

		# The defaults, which the *host* applies rather than the consumer, so that there is one
		# statement of what a partial @rpc means. These are GDScript's own and
		# SceneRPCInterface::_parse_rpc_config's both.
		var authority: Dictionary = rpc_config.get("TakeDamage", {})
		_check_eq("@rpc(\"authority\") is Godot's default mode",
				authority.get("rpc_mode"), MultiplayerAPI.RPC_MODE_AUTHORITY)
		_check_eq("and does not run on the caller", authority.get("call_local"), false)
		_check_eq("and travels reliably",
				authority.get("transfer_mode"), MultiplayerPeer.TRANSFER_MODE_RELIABLE)
		_check_eq("on channel zero", authority.get("channel"), 0)

		# Every field at once, written in an order that is not Godot's, because the words are
		# matched rather than positional -- and the number among them is the channel.
		var nudge: Dictionary = rpc_config.get("Nudge", {})
		_check_eq("a word out of order still lands", nudge.get("rpc_mode"), MultiplayerAPI.RPC_MODE_ANY_PEER)
		_check_eq("and the locality with it", nudge.get("call_local"), true)
		_check_eq("and the transfer mode",
				nudge.get("transfer_mode"), MultiplayerPeer.TRANSFER_MODE_UNRELIABLE_ORDERED)
		_check_eq("and a number among the words is the channel", nudge.get("channel"), 3)

		# One word from one category leaves the other three at their defaults.
		var ping: Dictionary = rpc_config.get("Ping", {})
		_check_eq("one word sets its own category", ping.get("rpc_mode"), MultiplayerAPI.RPC_MODE_ANY_PEER)
		_check_eq("and leaves the others alone",
				ping.get("transfer_mode"), MultiplayerPeer.TRANSFER_MODE_RELIABLE)

		# A refused config is dropped rather than half-applied: registering what survived parsing
		# would make the author's belief about the method nearly true, which is worse than not.
		_check("a refused @rpc is not registered at all", not rpc_config.has("Misspelled"))

		# The sending half needs no attribute -- `Node.rpc` is one of stage 6's vararg methods.
		#
		# **Which answer comes back is deliberately not asserted**, and the difference is Godot's
		# rather than Verse's. The editor-side driver's SceneTree has no MultiplayerAPI at all, so
		# the call stops at Node::rpcp with ERR_UNCONFIGURED; an exported game has one, whose
		# default offline peer reports itself connected, so the call reaches SceneRPCInterface,
		# finds the method in the very config this suite just read, and sends it to nobody --
		# which is OK. Asserting either number would be asserting which of Godot's guards fired
		# in which run.
		#
		# What a single-process run *can* say is that the call left Verse and came back as one of
		# Godot's Error ordinals rather than as a bridge failure. R-EXP-9's other half -- the call
		# that arrives at a second peer -- needs two processes, and is owed as a by-hand check.
		var sender := Node2D.new()
		sender.set_script(rpc_script)
		tree.root.add_child(sender)
		var sent: Variant = sender.call("SendTakeDamage", 5)
		_check("Node.rpc leaves Verse and comes back as a Godot Error", typeof(sent) == TYPE_INT)
		sender.queue_free()


	# --- R-EXP-1's remaining five, and R-EXP-8's @icon -------------------------------------------
	#
	# The rule is type-driven where the Verse type can say it and an attribute only where it cannot,
	# and these are the set where it cannot: a path, a directory, a paragraph, a bitmask and a node
	# are all `string` or `int`. Each maps to one of Godot's own hints, and each hint string is
	# already in Godot's own spelling -- so what these cases check is that nothing translated it.
	var hints_script: Script = load("res://scripts/hints.verse")
	_check("hints.verse compiles", hints_script != null and hints_script.can_instantiate())
	if hints_script == null:
		_check("@export_file reaches the inspector as a file picker", false)
	else:
		var hinted := {}
		for entry in hints_script.get_script_property_list():
			hinted[entry["name"]] = entry

		_check_eq("@export_file reaches the inspector as a file picker",
				hinted.get("Portrait", {}).get("hint"), PROPERTY_HINT_FILE)
		_check_eq("with Godot's own filter spelling, untranslated",
				hinted.get("Portrait", {}).get("hint_string"), "*.png,*.jpg")
		_check_eq("@export_dir is a directory picker",
				hinted.get("SaveFolder", {}).get("hint"), PROPERTY_HINT_DIR)
		_check_eq("@export_multiline is a text box",
				hinted.get("Notes", {}).get("hint"), PROPERTY_HINT_MULTILINE_TEXT)
		_check_eq("@export_flags is a bitmask",
				hinted.get("Elements", {}).get("hint"), PROPERTY_HINT_FLAGS)
		_check_eq("with the names comma separated, which is what Godot's own hint wants",
				hinted.get("Elements", {}).get("hint_string"), "Fire,Water,Earth")
		_check_eq("and it is still an int, because the attribute says what an int is for",
				hinted.get("Elements", {}).get("type"), TYPE_INT)
		_check_eq("@export_node_path filters by class",
				hinted.get("Target", {}).get("hint"), PROPERTY_HINT_NODE_PATH_VALID_TYPES)
		_check_eq("by the class named", hinted.get("Target", {}).get("hint_string"), "Node2D")

		# The five are additive: an export that needed no attribute still gets what its type
		# implied, which is the half of R-EXP-1 that was already done.
		_check("an export with no hint attribute is untouched", hinted.has("Plain"))

		# A hint on a type it cannot describe is refused rather than drawn as a plain field --
		# which is what it would otherwise be, and what no attribute at all draws, so the mistake
		# would be invisible. The sentence is asserted through the build's own warning log.
		_check("a hint on the wrong type is not drawn", not hinted.has("Mismatched"))

		# R-EXP-8's `@icon` is **not** asserted here, and cannot be: `get_class_icon_path` is a pure
		# virtual on Script with no ClassDB binding, so GDScript can no more call it than it can
		# call `_make_function`. Its one caller is EditorData::get_script_icon_path, in the editor.
		#
		# What is testable is where the logic actually lives: `verse_scan_class_decl` reads the
		# attribute out of the source text, and the units layer has four cases on it -- the path,
		# an @icon on another class in the same file, a bare one, and one whose argument is not a
		# literal. What is left for the editor session is whether Godot draws it, which no headless
		# run could have seen anyway.

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
	# --- the script editor's hover tooltip ------------------------------------------------------
	#
	# The one surface the editor draws that a test can reach at all. Everything about it lives in
	# the editor's own C++ -- CodeEdit decides the word and the column from the mouse, and
	# ScriptTextEditor turns a lookup result into a tooltip -- and `_lookup_code` is a virtual,
	# which ClassDB stores as metadata rather than as a callable MethodBind, so no script can call
	# it however it reaches the language. `probe_hover` is the seam, and tools/probe_hover.py is
	# the instrument that walks a whole project through it.
	#
	# These are the few shapes worth failing a build over. What each of them *was* is in the
	# commits; what matters here is that the label and the documentation page are the ones GDScript
	# would give for the same code.
	if editor:
		var hovers: Array = _verse_language().call("probe_hover", "res://scripts/hover_probe.verse")
		_check("hover_probe.verse answers hovers", not hovers.is_empty())

		# A class the project declares, at its own declaration -- which resolved to nothing at all
		# until the host's walk learned that a class's name is not inside the node it maps to.
		_check_eq("a script's own class hovers as a class",
				_hover_type(hovers, "hover_probe"), ScriptLanguageExtension.LOOKUP_RESULT_CLASS)
		_check_eq("named as the class the script registers a doc for",
				_hover(hovers, "hover_probe").get("class_name"), "hover_probe")

		# A parameter is immutable in Verse -- only a `var` is mutable -- so it hovers as a Local
		# Constant. GDScript spells a parameter the other way because there it is reassignable.
		_check_eq("a parameter is a Local Constant, not a Local Variable",
				_hover_type(hovers, "Factor"), ScriptLanguageExtension.LOOKUP_RESULT_LOCAL_CONSTANT)

		# A parameter's own comment reaches its hover, both a `#` line above it and an inline
		# `<# #>` before it. The host reads it off the parameter's node; the consumer's line-based
		# reader could not, because a parameter's source line is the one the function opens on (B41).
		_check_eq("a `#` line above a parameter is its description",
				_hover(hovers, "First").get("description"), "the first parameter")
		_check_eq("an inline `<# #>` before a parameter is its description",
				_hover(hovers, "Second").get("description"), "the second parameter")
		_check_eq("and carries its declared type", _hover(hovers, "Factor").get("doc_type"), "float")

		# Verse's primitives are what they cross as, and GDScript sends `int` to the same page.
		_check_eq("`int` hovers as Godot's int",
				_hover(hovers, "int").get("class_name"), "int")

		# A signal accessor is a Verse function and a Godot *signal*: asking Godot for a method
		# named `timeout` is an empty tooltip rather than a slightly wrong label.
		_check_eq("a mirrored engine signal hovers as a signal",
				_hover_type(hovers, "Timeout"), ScriptLanguageExtension.LOOKUP_RESULT_CLASS_SIGNAL)
		_check_eq("named as Timer.timeout",
				_hover(hovers, "Timeout").get("class_member"), "timeout")

		# A `...Statics` module and its constant, both of which are Godot's Vector2.
		_check_eq("a statics module hovers as the class it stands for",
				_hover(hovers, "Vector2Statics").get("class_name"), "Vector2")
		_check_eq("and its constant as a constant of that class",
				_hover_type(hovers, "Zero"), ScriptLanguageExtension.LOOKUP_RESULT_CLASS_CONSTANT)

		# An @GlobalScope utility, reached through the module holding what belongs to no class.
		_check_eq("a global utility hovers as an @GlobalScope method",
				_hover(hovers, "RandfRange").get("class_name"), "@GlobalScope")

		# A mirrored enum, whose Verse spelling cannot be inverted without the generated table.
		_check_eq("a mirrored enum hovers as a Godot enum",
				_hover_type(hovers, "node_internal_mode"),
				ScriptLanguageExtension.LOOKUP_RESULT_CLASS_ENUM)
		_check_eq("named as Node.InternalMode",
				_hover(hovers, "node_internal_mode").get("class_member"), "InternalMode")

		# `void` has no Godot page and GDScript answers nothing for it either. A box with the
		# label and the symbol and nothing else in it is worse than no box.
		_check_eq("`void` draws no tooltip rather than an empty one",
				_hover(hovers, "void").get("result"), ERR_UNAVAILABLE)

		# The type names the mirror exports that are not mirrored classes. Every one of these was
		# a "Local Constant" or nothing at all, and each reached the editor through a different
		# table: `vector2` was named in the class table and the other fifteen value types were not,
		# and `variant` was dropped from the type set when it briefly stopped being public and was
		# never put back when it became public again.
		_check_eq("a value type hovers as the Godot type it is",
				_hover(hovers, "vector2i").get("class_name"), "Vector2i")
		_check_eq("and Godot's RID likewise",
				_hover(hovers, "rid").get("class_name"), "RID")
		_check_eq("`variant` hovers as Godot's Variant, not as a local constant",
				_hover(hovers, "variant").get("class_name"), "Variant")
		_check_eq("and as a class result, which is what fetches the page",
				_hover_type(hovers, "variant"), ScriptLanguageExtension.LOOKUP_RESULT_CLASS)

		# `string` is `[]char`: one type the compiler prints two ways, so both spellings answer the
		# page that describes what crosses.
		_check_eq("`char` hovers as the String a []char becomes",
				_hover(hovers, "char").get("class_name"), "String")

		# A parametric type is a function to the compiler -- the one that answers the type -- so
		# its name arrives by a different route than `variant` above and needs its own arm.
		_check_eq("a parametric type hovers as the Godot type it wraps",
				_hover(hovers, "typed_array").get("class_name"), "Array")

		# An extension method on a value type: a module-level definition of `operator'.Length'`,
		# whose owner is the file it is written in. All 159 of GodotMath's hovered as locals with
		# their signature until the receiver was read off the declared type instead.
		_check_eq("a math method hovers as the Godot method it mirrors",
				_hover(hovers, "Length").get("class_name"), "Vector2")
		_check_eq("named as Vector2.length",
				_hover(hovers, "Length").get("class_member"), "length")

		# An extension method on a Verse type Godot has no page for -- `event(t).Emit`. It is a
		# method of the receiver, not a local constant of a function type, and a page is registered
		# for it under the receiver's own name so the tooltip has one to draw (B40). The registration
		# needs the script editor, which a headless run has none of, so what this asserts is the
		# result shape the editor would render; the drawing itself is a by-hand check.
		_check_eq("an event extension method hovers as a method, not a local",
				_hover_type(hovers, "Emit"), ScriptLanguageExtension.LOOKUP_RESULT_CLASS_METHOD)
		_check_eq("named under its receiver",
				_hover(hovers, "Emit").get("class_name"), "event")
		_check_eq("and by its own name",
				_hover(hovers, "Emit").get("class_member"), "Emit")

		# A scalar of the same file's, which Godot documents where it documents `randf_range`.
		_check_eq("a scalar math global hovers on @GlobalScope",
				_hover(hovers, "Smoothstep").get("class_name"), "@GlobalScope")

		# And the pair that proves which of the two it asks first: one name, a method on a vector
		# and a utility on a float, told apart by the receiver and by nothing else.
		_check_eq("one name written both ways answers both pages",
				_hover_answers(hovers, "Snapped"),
				["@GlobalScope.snapped", "Vector2.snapped"])

		# Verse's own library documents itself with `@doc("...")` rather than with a comment block,
		# and an attribute's text is reachable from nowhere on this side: re-reading the source
		# above the declaration finds an attribute line. The host reads it and hands it over, which
		# is the only reason either of these draws prose at all.
		#
		# Epic's own words rather than "not empty": an empty box and a box holding the wrong thing
		# both pass a length check, and what is being proved is that the text came from the engine
		# rather than from anything written here.
		_check("a Verse standard library function hovers with Epic's own documentation",
				"square root" in _hover(hovers, "Sqrt").get("description", ""))
		_check("and so does one of its classes",
				_hover(hovers, "event").get("description", "") != "")

		# A method of a parametric class, which is a CFunction to the compiler -- `signal(t)` is a
		# function answering a type -- so the walk that records where a mirror definition was
		# written stopped at it, and after the first build its members had no location to read a
		# comment from. This one is the bridge's own prose, by the other route.
		_check("a parametric class's method hovers with the comment above it",
				_hover(hovers, "Await").get("description", "") != "")

		# The description's *format*. Godot draws a local result's description as its own doc
		# BBCode -- `_add_text_to_rt`, where `\n` is a paragraph and `[` opens a tag -- and the
		# reader joined comment lines with `\n`, so a five-line comment was five paragraphs with
		# its backticks printed. This is the whole converted string, because the join, the
		# escape, the code span and the indented block are one answer and a substring check
		# would pass a description with any two of them wrong. What the tooltip *draws* with it
		# is by hand (by-hand-findings.md).
		_check_eq("a description reaches Godot as its doc BBCode",
				_hover(hovers, "Prose").get("description", ""),
				"The prose itself, which reaches Godot as its own doc BBCode: lines join into a paragraph, a blank line separates two, a [code]span[/code] is code, a [b]word[/b] is bold and Floor[lb]X[rb] is not a tag.\nThe second paragraph, with the sample the mirror's own comments write after a blank line:\n[codeblock lang=verse]\nResult := Floor[X]\n    Nested := 1\n[/codeblock]")

		# The other two comment forms. Verse has no doc-comment syntax, so whatever comment sits
		# above a declaration documents it -- and a `<# #>` block's inner lines carry no
		# delimiter, so a reader walking up by line prefix answered `>` for one and the first
		# line alone for a `<#>` comment. The reader lexes now (verse_doc_markup.h).
		_check_eq("a block comment documents the member under it",
				_hover(hovers, "Blocked").get("description", ""),
				"The other two comment forms, which the reader used to misread: this block read as [code]>[/code], the closing line stripped to that and the walk stopped at the line above it.")
		_check_eq("an indented comment documents the member under it",
				_hover(hovers, "Indented").get("description", ""),
				"An indented comment, whose body is whatever sits indented under the marker. The second line is the proof, because only the first survived before.")

		# A class under a `.vmodule`. Its doc is registered as `left/widget`, and Godot looks a
		# member's doc up by the class name the lookup reports -- which was the bare `widget`, a
		# name no doc was registered under, so every member of a class in a module hovered with
		# an empty box (by-hand-findings.md B38).
		var module_hovers: Array = _verse_language().call("probe_hover", "res://widgets/left/widget.verse")
		_check_eq("a class in a module hovers under its module-qualified name",
				_hover(module_hovers, "widget").get("class_name"), "left/widget")
		_check_eq("and so does a member of it",
				_hover(module_hovers, "RootConstant").get("class_name"), "left/widget")
		_check("with the comment above the member",
				"From helpers.verse" in _hover(module_hovers, "RootConstant").get("description", ""))

		# The types the file declares beside its own class. A type's name has no type to spell --
		# the host fills one for a data member and for a function, and a type is neither -- so each
		# of these drew the label, the name, a colon and nothing after it. The word comes from the
		# declaration, which is the only place it is: a struct and an interface both arrive here as
		# a class.
		_check_eq("a second class in the file hovers as a class",
				_hover(hovers, "hover_helper").get("doc_type"), "class")
		_check_eq("a struct is not called a class",
				_hover(hovers, "hover_reading").get("doc_type"), "struct")
		_check_eq("an enum the project declares keeps its own word",
				_hover(hovers, "hover_tempo").get("doc_type"), "enum")

		# And the label stays a local, which is a decision rather than the gap above: nothing
		# registers a doc for a second class in a file, so a CLASS result would draw an empty box
		# where the local carries the comment written above it (by-hand-findings.md B31).
		_check_eq("a second class keeps the local result, which is what carries its comment",
				_hover_type(hovers, "hover_helper"),
				ScriptLanguageExtension.LOOKUP_RESULT_LOCAL_CONSTANT)

		# A comment is prose. The mirror spells Godot's classes in lowercase, so this is the
		# difference between hovering a sentence and hovering code.
		_check_eq("a Godot class name in a comment draws nothing",
				_comment_hover(hovers, "node").get("result"), ERR_UNAVAILABLE)

		# The column one past a word's last character is still that word (TextEdit::get_word), and
		# it is where the pointer is over the right half of the last glyph. Every hover row for one
		# occurrence has to agree, which is what the whole-word check is.
		_check("every column of a word answers alike", _hover_columns_agree(hovers, "Timeout"))

		# --- the completion popup behind a `.` -------------------------------------------------
		#
		# Completion has two answers and only the first is a popup: `_complete_code` answers from the
		# last analysis at once and queues the buffer this caret needs, and the editor draws what it
		# was handed. A receiver the snapshot cannot type answers nothing, and nothing is an empty
		# popup -- so what is asserted here is the **first** answer. The refined one was already
		# right when `mob{}.` drew nothing at all (by-hand-findings.md B33).
		var binding_path := "res://scripts/bindings.verse"
		# Normalised the way the seam normalises it, or a CRLF checkout shifts every column.
		var binding_source := FileAccess.get_file_as_string(binding_path).replace("\r\n", "\n")
		var caret := binding_source.find("mob{}.")
		_check("bindings.verse writes an archetype receiver", caret >= 0)
		if caret >= 0:
			var head := binding_source.substr(0, caret + "mob{}.".length())
			var rows: Array = _verse_language().call("probe_complete", binding_path,
					PackedInt32Array([head.count("\n"), head.length() - (head.rfind("\n") + 1)]))
			_check_eq("the completion seam answers one row per caret", rows.size(), 1)
			if rows.size() == 1:
				var opened := []
				for option in rows[0]["first_options"]:
					opened.append(String(option["insert_text"]))
				_check("a binding's own members are offered the moment the popup opens",
						opened.has("Hit(") and opened.has("Label()"))
				var refined := []
				for option in rows[0]["options"]:
					refined.append(String(option["insert_text"]))
				_check("and the analysis keeps them rather than replacing them",
						refined.has("Hit(") and refined.size() > opened.size())
	else:
		_skip("the script editor's hover tooltip", "no analysis in an exported game")

	var tx_script: Script = load("res://scripts/transactions.verse")
	_check("transactions.verse compiles", tx_script != null and tx_script.can_instantiate())
	if tx_script == null:
		fatal = true
		return
	_tx = Node2D.new()
	_tx.set_script(tx_script)
	tree.root.add_child(_tx)


# The Verse language object. ScriptLanguage exposes no `get_name` to ClassDB -- `get_class()` is
# the only question a bare one answers about itself.
func _verse_language() -> Object:
	for i in Engine.get_script_language_count():
		var lang := Engine.get_script_language(i)
		if lang != null and lang.get_class() == "VerseScriptLanguage":
			return lang
	return null


# The first row for a name that is code rather than prose. probe_hover answers one row per word
# and per run of columns that agree, so a name written once is one row.
func _hover(rows: Array, symbol: String) -> Dictionary:
	for row in rows:
		if row["symbol"] == symbol and row["token"] != "comment" and row["token"] != "string":
			return row
	return {}


func _hover_type(rows: Array, symbol: String) -> int:
	return _hover(rows, symbol).get("type", -1)


# Every distinct Godot page a name hovers to, sorted. A name written two ways -- `Snapped` as a
# method on a vector2 and as a utility on a float -- is one symbol with two right answers, and
# _hover would only ever report whichever came first in the file.
func _hover_answers(rows: Array, symbol: String) -> Array:
	var seen := {}
	for row in rows:
		if row["symbol"] != symbol or row["token"] == "comment" or row["token"] == "string":
			continue
		if row["class_name"] != "":
			seen["%s.%s" % [row["class_name"], row["class_member"]]] = true
	var answers := seen.keys()
	answers.sort()
	return answers


func _comment_hover(rows: Array, symbol: String) -> Dictionary:
	for row in rows:
		if row["symbol"] == symbol and row["token"] == "comment":
			return row
	return {}


# Whether every column of every occurrence of a word draws the same tooltip. probe_hover splits a
# word into one row per distinct *answer*, and the answer carries what the host resolved beside
# what the editor would draw -- so this compares only the half the author sees.
func _hover_columns_agree(rows: Array, symbol: String) -> bool:
	var seen := {}
	for row in rows:
		if row["symbol"] != symbol or row["token"] == "comment":
			continue
		var key := "%d:%d" % [row["line"], row["word"]]
		var drawn := "%s|%s|%s|%s|%s" % [row["result"], row["type"], row["class_name"],
				row["class_member"], row["doc_type"]]
		if seen.get(key, drawn) != drawn:
			return false
		seen[key] = drawn
	return not seen.is_empty()


# True when the last frame-stepped case has run. The driver is what quits.
func step() -> bool:
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
					_tx.get_signal_connection_list("Hit").size(), OWN_CONNECTION + 1)
			_tx.call("SubscribeThenDecline")
			_check_eq("a Subscribe undone by a top-level decline leaves no connection",
					_tx.get_signal_connection_list("Hit").size(), OWN_CONNECTION + 1)
			_tx.call("SubscribeThenFailInner")
			_check_eq("a Subscribe undone by a failed context leaves no connection",
					_tx.get_signal_connection_list("Hit").size(), OWN_CONNECTION + 1)
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
					_tx.get_signal_connection_list("Scored").size(), OWN_CONNECTION + 1)

			# R-DIAG-3: the same raise, over and over, must not print its stack over and over.
			# Godot already drops the *errors* past max_errors_per_second and says so once; the
			# stack the bridge prints underneath each one is ordinary output, counted against the
			# character budget every other script shares, and before Phase 6 a script raising every
			# frame spent that budget on its own stack. What this asserts is only that the calls
			# still run; what the suppression actually printed is asserted in run_tests.py, because
			# a script cannot read the output log.
			for _i in 12:
				_tx.call("SetThenRaise")
			_check_eq("a script raising over and over still runs after it",
					_tx.call("ReadObservedX"), 7.0)

		# --- R-ASYNC-1/2/4/5, R-SIG-5: tasks --------------------------------------------------
		#
		# A frame apart on purpose. A task spawned in one call suspends; what resumes it happens in
		# a later one, and a `vh_tick` runs in between -- which is the claim being tested, not a
		# scheduling detail.
		3:
			var conc_script: Script = load("res://scripts/concurrency.verse")
			_check("concurrency.verse compiles", conc_script != null and conc_script.can_instantiate())
			if conc_script == null:
				fatal = true
				return true
			_conc = Node2D.new()
			_conc.set_script(conc_script)
			tree.root.add_child(_conc)
			_conc2 = Node2D.new()
			_conc2.set_script(conc_script)
			tree.root.add_child(_conc2)

			# Awaiting a signal the script itself declares. The task suspends inside the call that
			# spawned it, and the call returns normally -- which is what "the call returns VH_OK"
			# means from out here.
			_conc.call("StartWaitForFired")
			_check_eq("a spawned task runs up to its first await", _conc.call("ReadStage"), 1)

			# One Godot connection, ours, for the duration of the wait.
			_check_eq("awaiting connects to the signal",
					_conc.get_signal_connection_list("Fired").size(), OWN_CONNECTION)
		4:
			_check_eq("and the task is still suspended a frame later", _conc.call("ReadStage"), 1)

			# GDScript emits, and the Verse task resumes inside the emission (R-SIG-5).
			_conc.emit_signal("Fired")
			_check_eq("emitting resumes the awaiting task", _conc.call("ReadStage"), 2)
			_check_eq("and the connection it made is gone again",
					_conc.get_signal_connection_list("Fired").size(), OWN_CONNECTION)

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
			tree.root.add_child(awaited_timer)
			_conc.call("StartWaitForTimer", awaited_timer)
			_check_eq("awaiting an engine signal connects to it",
					awaited_timer.get_signal_connection_list("timeout").size(), 1)
			awaited_timer.emit_signal("timeout")
			_check_eq("and the task resumes when the engine emits", _conc.call("ReadStage"), 10)
			tree.root.remove_child(awaited_timer)
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
							_conc.get_signal_connection_list("Scored").size()], [OWN_CONNECTION, OWN_CONNECTION])
			_conc.emit_signal("Fired")
			_check_eq("the race returns when the first fires", _conc.call("ReadStage"), 30)
			_check_eq("and the loser leaves no connection behind",
					[_conc.get_signal_connection_list("Fired").size(),
							_conc.get_signal_connection_list("Scored").size()], [OWN_CONNECTION, OWN_CONNECTION])

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
			tree.root.remove_child(doomed)
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
			tree.root.add_child(_hit)
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
			tree.root.remove_child(_hit)
			_hit.free()
			_hit = null

			# A source change reaching an attached node, which needs the instance replaced: it
			# holds a vh_instance made against the retiring generation and adopts nothing. Before
			# B8 the edit compiled and the node kept answering the old code until a restart.
			if editor:
				_check_reload_replaces_the_instance()
			else:
				# Both halves are impossible in an export: res:// is a read-only pack, so the edit
				# cannot be written, and `build_project` is refused even if it could.
				_skip("the attached node answers the code it was built against")
				_skip("the edited project builds")
				_skip("the attached node answers the reloaded code")
				_skip("and its exported value survived the instance being replaced")
		6, 8:
			# One frame for the injected event to be delivered.
			pass
		_:
			return true
	return false
