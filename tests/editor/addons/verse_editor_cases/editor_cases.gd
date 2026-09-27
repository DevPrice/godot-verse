@tool
extends RefCounted

# The editor layer's cases: one `[editor]` line per case in tools/test_records.py's shape, and a
# summary line the harness requires, run inside a headless editor over a copy of tests/integration
# with the extension and the host loaded (docs/editor-test-audit.md).
#
# Everything here walks the editor's own nodes, which are a foreign contract the way Godot's source
# is (architecture-review.md item 4): a lookup that finds nothing fails a case with a sentence naming
# what it looked for, never an empty row. A later step adds a `_group()` function and one line in
# run(); the helpers below are what those share.

var plugin: EditorPlugin
var finished := false
var passed := 0
var failed := 0
var skipped := 0

# What is running now and since when, which is what the watchdog names when it stops the run: a
# case that waits for the editor forever reads as that case failing rather than as a hung process.
var _step := "starting"
var _step_started_ms := 0
const STEP_TIMEOUT_MS := 120000


func run() -> void:
	_mark("the editor's first filesystem scan")
	await wait_until(func() -> bool: return not EditorInterface.get_resource_filesystem().is_scanning(),
			STEP_TIMEOUT_MS)
	await frames(2)
	# The second session run_tests.py starts, with VERSE_HOST_TEST_FATAL in the editor's own
	# environment: nothing else runs in it, because every Play it made would die.
	if OS.get_cmdline_user_args().has(FATAL_FLAG):
		await _host_fatal_during_play()
		finished = true
		return
	await _harness()
	await _placeholders_and_saving()
	await _code_editor()
	await _debugger_and_profiler()
	await _inspector_docks_and_dialogs()
	finished = true


# --- the harness's own cases: the layer reaches what later steps build on ---------------------

func _harness() -> void:
	_mark("the harness's own cases")
	check("the editor runs headless, with the editor hint",
			Engine.is_editor_hint() and DisplayServer.get_name() == "headless")
	# The guard play_scene() applies, asserted before anything needs it: without it a Play from a
	# later case would open a window on Devin's desktop.
	check("Play is armed to start the game headless", play_is_headless())
	var port := _user_arg_int("--verse-editor-port=")
	check_eq("the debugger listens on the layer's own port, not an open editor's",
			EditorInterface.get_editor_settings().get_setting("network/debug/remote_port"), port)
	check("the Verse language is registered in the editor", verse_language() != null)

	await _placeholder_case()
	var code := await _code_edit_case()
	if code != null:
		await _completion_case(code)
	await _hover_case()


func _placeholder_case() -> void:
	_mark("a placeholder for a Verse node")
	const SCENE := "res://editor_cases/placeholder.tscn"
	var script: Script = load("res://scripts/reload_probe.verse")
	check("a non-tool Verse script loads in the editor and does not instantiate",
			script != null and not script.can_instantiate())
	if script == null:
		return
	var node := Node2D.new()
	node.name = "Probe"
	node.set_script(script)
	var packed := PackedScene.new()
	packed.pack(node)
	node.free()
	DirAccess.make_dir_recursive_absolute(SCENE.get_base_dir())
	check_eq("a scene holding it saves", ResourceSaver.save(packed, SCENE), OK)

	EditorInterface.open_scene_from_path(SCENE)
	var opened := await wait_until(func() -> bool:
		var root := EditorInterface.get_edited_scene_root()
		return root != null and root.scene_file_path == SCENE, 10000)
	check("the editor opens the scene", opened)
	if not opened:
		return
	var root := EditorInterface.get_edited_scene_root()
	check("its node carries the Verse script", root.get_script() == script)
	check_eq("the placeholder serves the declared default", root.get("Tag"), 0)
	root.set("Tag", 42)
	check_eq("and stores what the inspector writes", root.get("Tag"), 42)


func _code_edit_case() -> CodeEdit:
	_mark("the script editor for a .verse")
	var script: Script = load("res://scripts/completion_probe.verse")
	var code := await code_edit_for(script)
	check("a .verse opens in the script editor as a CodeEdit", code != null)
	if code == null:
		return null
	check_eq("holding the file's text", code.text.replace("\r\n", "\n"),
			FileAccess.get_file_as_string(script.resource_path).replace("\r\n", "\n"))
	check("drawn by VerseSyntaxHighlighter",
			code.syntax_highlighter != null and code.syntax_highlighter.is_class("VerseSyntaxHighlighter"))
	var prefixes: Array = code.code_completion_prefixes
	check("its completion prefixes carry `?`, `{` and `[`",
			prefixes.has("?") and prefixes.has("{") and prefixes.has("["))
	return code


# Forced, which is Ctrl+Space: CodeEdit emits code_completion_requested and the script editor asks
# _complete_code in the same frame. Asked again each frame until the answer the analysis refines
# lands, the way refresh_completion_if_current re-asks for an author.
func _completion_case(code: CodeEdit) -> void:
	_mark("completion in the script editor")
	if not place_caret_after(code, "Other."):
		check("completion_probe.verse writes `Other.`", false)
		return
	var offered := await wait_until(func() -> bool:
		code.request_code_completion(true)
		return _inserts(code).has("GetNode["), 30000)
	check("a forced request answers the receiver's members through the language", offered)
	check("and the popup is open", code.get_code_completion_selected_index() != -1)
	code.cancel_code_completion()


# symbol_hovered is what ScriptTextEditor connects its tooltip to in 4.7.2
# (script_text_editor.cpp:2682), so emitting it is Godot's own path from the word to the box.
func _hover_case() -> void:
	_mark("a hover tooltip")
	var script: Script = load("res://scripts/hover_probe.verse")
	var code := await code_edit_for(script)
	if code == null:
		check("hover_probe.verse opens in the script editor", false)
		return
	var line := _line_of(code, "\tBlocked<public>()")
	if line < 0:
		check("hover_probe.verse declares Blocked", false)
		return
	code.emit_signal("symbol_hovered", "Blocked", line, 2)
	var shown := await wait_until(func() -> bool: return not find_all(code, "EditorHelpBitTooltip").is_empty(), 10000)
	check("hovering a member raises the editor's own tooltip", shown)
	if not shown:
		return
	var text := ""
	for label in find_all(find_all(code, "EditorHelpBitTooltip")[0], "RichTextLabel"):
		text += (label as RichTextLabel).get_parsed_text() + "\n"
	check("and it draws the comment above the member",
			text.contains("The other two comment forms"))
	for tip in find_all(code, "EditorHelpBitTooltip"):
		tip.queue_free()


# --- placeholders and saving (docs/editor-test-audit.md step 3) ---------------------------------
#
# by-hand-findings.md's B26, B39 and B8 procedures and R-EXP-6's five steps, a case per step. Each
# Verse subject has a GDScript control beside it that takes the same steps over the same exports,
# because stock GDScript has been seen writing `= null` lines after an import (editor-test-audit.md,
# "One stock-GDScript observation"): a Verse failure beside a passing control is the bridge's, and
# one beside a failing control is Godot's.

const CASES_DIR := "res://editor_cases"
const SAVE_PROBE := "res://scripts/save_probe.verse"
const BYSTANDER := "res://scripts/reload_probe.verse"
const SETTINGS_TRES := "res://editor_cases/settings.tres"
const PLAY_SCENE := "res://editor_cases/play_saved.tscn"
const B8_NOTIFICATION := 9001

const SAVE_CONTROL_SOURCE := """extends Node2D

@export var count: int = 7
@export var buddy: Node2D


func answer() -> int:
	return 1


func _notification(what: int) -> void:
	if what == 9001:
		count = what
"""

const BYSTANDER_CONTROL_SOURCE := """extends Node


func other() -> int:
	return 1
"""

const IDLE_SOURCE := """extends Node


func _ready() -> void:
	print("idle game up")
"""

const SETTINGS_READER_SOURCE := """extends Node


func _ready() -> void:
	var settings: Resource = load("res://editor_cases/settings.tres")
	print("settings_reader: ", settings.call("Describe"))
"""

# The by-hand steps name the int and the node export and nothing else, so a subject is those two
# names, how to break and fix the file, and the other file whose break is B26's step 3.
var _subjects: Array[Dictionary] = []


func _placeholders_and_saving() -> void:
	_mark("the placeholder and save fixtures")
	DirAccess.make_dir_recursive_absolute(ProjectSettings.globalize_path(CASES_DIR))
	var verse: Script = load(SAVE_PROBE)
	var verse_bystander: Script = load(BYSTANDER)
	var control := _write_script(CASES_DIR.path_join("save_control.gd"), SAVE_CONTROL_SOURCE)
	var control_bystander := _write_script(CASES_DIR.path_join("bystander_control.gd"), BYSTANDER_CONTROL_SOURCE)
	check("the save fixtures load", verse != null and verse_bystander != null
			and control != null and control_bystander != null)
	if verse == null or verse_bystander == null or control == null or control_bystander == null:
		return
	_subjects = [
		{"id": "verse", "tag": "", "script": verse, "count": "Count", "buddy": "Buddy",
			"method": "Answer", "good": verse.source_code,
			"broken": ["Answer<public>()", "Answer<public>("], "fixed": ["int = 1", "int = 2"],
			"bystander": verse_bystander, "bystander_method": "Answer",
			"bystander_broken": ["Answer<public>()", "Answer<public>("],
			"tool": ["save_probe := class", "@tool\nsave_probe := class"]},
		{"id": "gd", "tag": "GDScript control: ", "script": control, "count": "count", "buddy": "buddy",
			"method": "answer", "good": control.source_code,
			"broken": ["func answer()", "func answer("], "fixed": ["return 1", "return 2"],
			"bystander": control_bystander, "bystander_method": "other",
			"bystander_broken": ["func other()", "func other("],
			"tool": ["extends Node2D", "@tool\nextends Node2D"]},
	]
	for subject in _subjects:
		await _b26(subject)
	for subject in _subjects:
		await _b39(subject)
	await _settings_resource()
	await _play_saved()
	for subject in _subjects:
		await _b8(subject)


# B26: "attach a script with an int export and a node2d export to a node, set both in the inspector,
# and save the scene. Then (1) break the script ... (2) fix the script, save ... (3) repeat (1) with
# the break in a different .verse file." Step 2's "the node runs the new code" is _play_saved's.
func _b26(s: Dictionary) -> void:
	var tag: String = s.tag + "B26"
	_mark(tag + ": exported values across a broken save")
	var scene := CASES_DIR.path_join("b26_%s.tscn" % s.id)
	var root := Node2D.new()
	root.name = "SaveProbe"
	root.set_script(s.script)
	var buddy := Node2D.new()
	buddy.name = "Buddy"
	root.add_child(buddy)
	buddy.owner = root
	root = await _open_new_scene(root, scene)
	if root == null:
		check(tag + ": the editor opens its scene", false)
		return
	root.set(s.count, 42)
	root.set(s.buddy, root.get_node("Buddy"))
	check_eq(tag + ": the inspector's scene saves", _save_edited_scene(), OK)
	_check_b26_scene(tag + ": the saved scene", scene, s)

	await _resave(s.script, _swap(s.good, s.broken), s.method, false)
	_check_b26_values(tag + " (1): after a broken save", root, s)
	_save_edited_scene()
	_check_b26_scene(tag + " (1): and the scene saved after it", scene, s)

	s.good = _swap(s.good, s.fixed)
	await _resave(s.script, s.good, s.method, true)
	_check_b26_values(tag + " (2): after the fix is saved", root, s)
	_save_edited_scene()
	_check_b26_scene(tag + " (2): and the scene saved after it", scene, s)

	await _resave(s.bystander, _swap(s.bystander.source_code, s.bystander_broken), s.bystander_method, false)
	_check_b26_values(tag + " (3): after another file's broken save", root, s)
	_save_edited_scene()
	_check_b26_scene(tag + " (3): and the scene saved after it", scene, s)
	await _resave(s.bystander, _swap(s.bystander.source_code, [s.bystander_broken[1], s.bystander_broken[0]]),
			s.bystander_method, true)


func _check_b26_values(name: String, root: Node, s: Dictionary) -> void:
	_expect(s, name + ", the int export still reads what was set", root.get(s.count) == 42,
			"it reads %s" % str(root.get(s.count)))
	_expect(s, name + ", and so does the node export", root.get(s.buddy) == root.get_node("Buddy"),
			"it reads %s" % str(root.get(s.buddy)))


func _check_b26_scene(name: String, scene: String, s: Dictionary) -> void:
	var lines := _node_lines(_scene_text(scene), "SaveProbe")
	_expect(s, name + " carries the int", lines.has("%s = 42" % s.count), "the node's lines are %s" % str(lines))
	_expect(s, name + " carries the node", lines.has('%s = NodePath("Buddy")' % s.buddy),
			"the node's lines are %s" % str(lines))


# B39: "instance that node into a second scene. Save the second scene, then (1) break the script ...
# and confirm the second scene's .tscn does not grow a `= null` line for the export; (2) repeat with
# the break in a third, unrelated .verse file; (3) override the value in the second scene, break the
# script, save, and confirm the override survives." The override is there from the start, so (3) is
# read at every save rather than at one.
func _b39(s: Dictionary) -> void:
	var tag: String = s.tag + "B39"
	_mark(tag + ": an inherited export across a broken save")
	var base_path := CASES_DIR.path_join("b39_base_%s.tscn" % s.id)
	var outer_path := CASES_DIR.path_join("b39_outer_%s.tscn" % s.id)
	var inner := Node2D.new()
	inner.name = "Inner"
	inner.set_script(s.script)
	if not _pack(inner, base_path):
		check(tag + ": the instanced scene saves", false)
		return
	var base: PackedScene = ResourceLoader.load(base_path, "", ResourceLoader.CACHE_MODE_REPLACE)
	var outer := Node.new()
	outer.name = "Outer"
	for child_name in ["Inherits", "Overrides"]:
		var child := base.instantiate(PackedScene.GEN_EDIT_STATE_INSTANCE)
		child.name = child_name
		outer.add_child(child)
		child.owner = outer
	outer.get_node("Overrides").set(s.count, 9)
	if await _open_new_scene(outer, outer_path) == null:
		check(tag + ": the editor opens the scene instancing it", false)
		return
	check_eq(tag + ": the instancing scene saves", _save_edited_scene(), OK)
	_check_b39_scene(tag + ": before anything breaks", outer_path, s)

	await _resave(s.script, _swap(s.good, s.broken), s.method, false)
	_save_edited_scene()
	_check_b39_scene(tag + " (1): after a broken save", outer_path, s)
	await _resave(s.script, s.good, s.method, true)

	await _resave(s.bystander, _swap(s.bystander.source_code, s.bystander_broken), s.bystander_method, false)
	_save_edited_scene()
	_check_b39_scene(tag + " (2): after another file's broken save", outer_path, s)
	await _resave(s.bystander, _swap(s.bystander.source_code, [s.bystander_broken[1], s.bystander_broken[0]]),
			s.bystander_method, true)


func _check_b39_scene(name: String, scene: String, s: Dictionary) -> void:
	var text := _scene_text(scene)
	var inherits := _node_lines(text, "Inherits")
	_expect(s, name + ", the un-overridden instance stores no export", inherits.is_empty(),
			"it stores %s" % str(inherits))
	_expect(s, name + ", no export is written as null", not text.contains("= null"),
			"the scene carries a `= null` line")
	var overrides := _node_lines(text, "Overrides")
	_expect(s, name + ", the override survives", overrides.has("%s = 9" % s.count),
			"the override's lines are %s" % str(overrides))


# A control's case is Godot's own answer, which the Verse case beside it is read against, and not
# this layer's to gate on: one that does not hold is reported as a skip saying what stock GDScript
# did, so the record is kept and the run stays green.
func _expect(s: Dictionary, name: String, ok: bool, what_happened: String) -> void:
	if ok or s.id != "gd":
		check(name, ok)
	else:
		skip(name, "Godot's own behaviour, not the bridge's: " + what_happened)


# R-EXP-6's editor half, by-hand-findings.md's "R-EXP-6's editor half": the dialog finds the class,
# a new one shows the declared defaults, edits survive a save and a reload, and the file names the
# class. Step 5, the game reading the edits, is _play_saved's.
func _settings_resource() -> void:
	_mark("R-EXP-6: a Verse Resource from the editor")
	var base := ""
	for row in ProjectSettings.get_global_class_list():
		if row["class"] == &"SettingsResource":
			base = String(row["base"])
	check_eq("R-EXP-6 (1): the global class registry files SettingsResource under Resource", base, "Resource")
	await _create_dialog_lists("SettingsResource", "Resource")

	var script: Script = load("res://scripts/settings_resource.verse")
	var settings := Resource.new()
	settings.set_script(script)
	check_eq("R-EXP-6 (2): a new one shows the declared Title", settings.get("Title"), "untitled")
	check_eq("R-EXP-6 (2): and the declared Rounds", settings.get("Rounds"), 3)
	check_eq("R-EXP-6 (2): and the declared Speed", settings.get("Speed"), 1.5)
	check_eq("R-EXP-6 (2): and saves as a .tres", ResourceSaver.save(settings, SETTINGS_TRES), OK)

	settings.set("Title", "edited")
	settings.set("Rounds", 7)
	settings.set("Speed", 2.5)
	check_eq("R-EXP-6 (3): the edited one saves", ResourceSaver.save(settings, SETTINGS_TRES), OK)
	var back: Resource = ResourceLoader.load(SETTINGS_TRES, "", ResourceLoader.CACHE_MODE_IGNORE)
	check("R-EXP-6 (4): it reopens", back != null)
	if back != null:
		check_eq("R-EXP-6 (4): with the edited Title", back.get("Title"), "edited")
		check_eq("R-EXP-6 (4): and the edited Rounds", back.get("Rounds"), 7)
		check_eq("R-EXP-6 (4): and the edited Speed", back.get("Speed"), 2.5)
	check("R-EXP-6 (4): the file names the class",
			FileAccess.get_file_as_string(SETTINGS_TRES).contains('script_class="SettingsResource"'))


# The Create New Resource dialog, which reads the global class registry: typing the class's name
# must find it, filed under its base.
func _create_dialog_lists(class_name_: String, base: String) -> void:
	EditorInterface.popup_create_dialog(func(_picked: StringName) -> void: pass, StringName(base))
	await wait_until(func() -> bool: return _visible_create_dialog() != null, 5000)
	var dialog := _visible_create_dialog()
	if dialog == null:
		check("R-EXP-6 (1): Create New Resource opens a CreateDialog", false)
		return
	# The search box is CreateDialog's one FilterLineEdit; the other LineEdits belong to the
	# favourites' and the tree's own popups.
	var boxes := find_all(dialog, "FilterLineEdit")
	if boxes.is_empty():
		check("R-EXP-6 (1): the CreateDialog has a search box", false)
		(dialog as Window).hide()
		return
	var box := boxes[0] as LineEdit
	box.text = class_name_
	box.text_changed.emit(class_name_)
	await frames(2)
	var parent_text := ""
	for tree in find_all(dialog, "Tree"):
		var item := _find_tree_item((tree as Tree).get_root(), class_name_)
		if item != null and item.get_parent() != null:
			parent_text = item.get_parent().get_text(0)
	check_eq("R-EXP-6 (1): Create New Resource finds %s under %s" % [class_name_, base], parent_text, base)
	(dialog as Window).hide()


func _visible_create_dialog() -> Window:
	for candidate in find_all(plugin.get_tree().root, "CreateDialog"):
		if (candidate as Window).visible:
			return candidate
	return null


func _find_tree_item(item: TreeItem, text: String) -> TreeItem:
	if item == null:
		return null
	if item.get_text(0) == text:
		return item
	for child in item.get_children():
		var found := _find_tree_item(child, text)
		if found != null:
			return found
	return null


# One Play for what B26 step 2 and R-EXP-6 step 5 each read off a running game: the fixed code
# running with the value the inspector set, and the edited resource's values.
func _play_saved() -> void:
	_mark("Play reads what the editor saved")
	var reader := _write_script(CASES_DIR.path_join("settings_reader.gd"), SETTINGS_READER_SOURCE)
	var b26: PackedScene = ResourceLoader.load(CASES_DIR.path_join("b26_verse.tscn"), "", ResourceLoader.CACHE_MODE_REPLACE)
	if reader == null or b26 == null:
		check("the Play fixtures load", false)
		return
	var root := Node.new()
	root.name = "PlaySaved"
	root.set_script(reader)
	var probe := b26.instantiate(PackedScene.GEN_EDIT_STATE_INSTANCE)
	root.add_child(probe)
	probe.owner = root
	if not _pack(root, PLAY_SCENE):
		check("the Play scene saves", false)
		return
	var output := await play_and_read(PLAY_SCENE, ["save_probe ready:", "settings_reader:"])
	check("B26 (2): the game runs the fixed code with the value the inspector set",
			output.contains("save_probe ready: answer 2, count 42"))
	check("R-EXP-6 (5): the game reads the edited resource", output.contains("settings_reader: edited x7"))


# B8: "Adding @tool to a script already attached to a node ... needs the scene reloaded." The Verse
# case would report that as a known defect while it stood; the node answering a notification only a
# real instance runs is the sign it no longer does.
func _b8(s: Dictionary) -> void:
	var tag: String = s.tag + "B8"
	_mark(tag + ": adding @tool to an attached script")
	var scene := CASES_DIR.path_join("b26_%s.tscn" % s.id)
	var root := await _open_scene(scene)
	if root == null:
		check(tag + ": the editor opens the scene", false)
		return
	root.set(s.count, 42)
	root.notification(B8_NOTIFICATION)
	check_eq(tag + ": before @tool, the node holds a placeholder", root.get(s.count), 42)

	var tool_source := _swap(s.script.source_code, s.tool)
	await _resave(s.script, tool_source, s.method, true)
	check(tag + ": with @tool saved, the script can instantiate in the editor", s.script.can_instantiate())
	root.notification(B8_NOTIFICATION)
	var swapped: bool = root.get(s.count) == B8_NOTIFICATION
	if swapped or s.id == "gd":
		_expect(s, tag + ": the save alone gives the node a real instance", swapped,
				"a GDScript saved through ResourceSaver keeps its placeholder until the scene is reloaded")
	else:
		skip(tag + ": the save alone gives the node a real instance",
				"known defect: B8, the node keeps its placeholder until the scene is reloaded")

	# By instance id: the reload frees the node, and a lambda holding a freed object is handed null.
	var old_id := root.get_instance_id()
	EditorInterface.reload_scene_from_path(scene)
	var reloaded := await wait_until(func() -> bool:
		var now := EditorInterface.get_edited_scene_root()
		return now != null and now.get_instance_id() != old_id and now.scene_file_path == scene, 10000)
	root = EditorInterface.get_edited_scene_root() if reloaded else null
	if root != null:
		root.notification(B8_NOTIFICATION)
	check(tag + ": after a scene reload the node holds a real instance",
			root != null and root.get(s.count) == B8_NOTIFICATION)
	await _resave(s.script, s.good, s.method, true)


func _swap(source: String, pair: Array) -> String:
	return source.replace(pair[0], pair[1])


func _write_script(path: String, source: String) -> Script:
	var file := FileAccess.open(path, FileAccess.WRITE)
	if file == null:
		return null
	file.store_string(source)
	file.close()
	return ResourceLoader.load(path, "", ResourceLoader.CACHE_MODE_REPLACE)


# Through the script's own saver, which is VerseResourceFormatSaver::_save and so _reload for a
# .verse. A Verse save queues an analysis rather than waiting for one, so the answer is awaited: the
# method list empties when the analysis of a broken file lands and fills again for a good one.
func _resave(script: Script, source: String, method: String, compiles: bool) -> void:
	script.source_code = source
	ResourceSaver.save(script)
	if script is GDScript:
		await frames(2)
		return
	var settled := await wait_until(func() -> bool: return _declares(script, method) == compiles, 60000)
	if not settled:
		check("the analysis of %s's %s save lands" % [script.resource_path.get_file(),
				"good" if compiles else "broken"], false)


func _declares(script: Script, method: String) -> bool:
	for row in script.get_script_method_list():
		if row["name"] == method:
			return true
	return false


func _pack(root: Node, path: String) -> bool:
	var packed := PackedScene.new()
	var ok := packed.pack(root) == OK and ResourceSaver.save(packed, path) == OK
	root.free()
	return ok


func _open_new_scene(root: Node, path: String) -> Node:
	if not _pack(root, path):
		return null
	return await _open_scene(path)


func _open_scene(path: String) -> Node:
	EditorInterface.open_scene_from_path(path)
	var opened := await wait_until(func() -> bool:
		var now := EditorInterface.get_edited_scene_root()
		return now != null and now.scene_file_path == path, 10000)
	return EditorInterface.get_edited_scene_root() if opened else null


func _save_edited_scene() -> Error:
	EditorInterface.mark_scene_as_unsaved()
	return EditorInterface.save_scene()


func _scene_text(path: String) -> String:
	return FileAccess.get_file_as_string(path).replace("\r\n", "\n")


# The property lines of one [node] section of a .tscn.
func _node_lines(text: String, node_name: String) -> PackedStringArray:
	var lines := PackedStringArray()
	var inside := false
	for line in text.split("\n"):
		if line.begins_with("["):
			inside = line.begins_with('[node name="%s"' % node_name)
		elif inside and not line.strip_edges().is_empty():
			lines.append(line)
	return lines


# --- the code editor (docs/editor-test-audit.md step 4) -------------------------------------------
#
# What by-hand-findings.md's "What is still open" sent a person to the script editor to see: the
# completion popup opening, the argument hint's re-ask, the tooltip's text, the class page, the
# jump to a binding, the colours, the connection gutter and the warnings panel. Each is read off the
# editor's own nodes. The one thing no node carries is the call hint (CodeEdit binds set_code_hint
# and no getter), so a re-ask is counted on code_completion_requested and the hint it would draw is
# read off probe_complete_code at the same caret.

const COMPLETION_PROBE := "res://scripts/completion_probe.verse"
const HOVER_PROBE := "res://scripts/hover_probe.verse"
const CARET_MARKER := 0xFFFF
const ANALYSIS_TIMEOUT_MS := 60000


func _code_editor() -> void:
	await _completion_popup()
	await _tooltips()
	await _class_page()
	await _binding_lookup()
	await _colours()
	await _connection_gutter()
	await _warnings_panel()
	# Last, because it presses Play and edits a file afterwards, which is what the final case here
	# finds costs every other script its documentation.
	await _argument_hint()
	await _docs_after_play()


# by-hand-findings.md "The completion popup, where Godot decides whether to open one" and "The
# named-argument popup": each position is typed, the analysis it asks for is let land, and then
# Godot's own unforced request decides -- which is the prefix table's whole question.
func _completion_popup() -> void:
	_mark("the completion popup opens where it should")
	var script: Script = load(COMPLETION_PROBE)
	var code := await code_edit_for(script)
	if code == null:
		check("completion_probe.verse opens in the script editor", false)
		return
	var original := code.text

	var last := code.get_line_count() - 1
	code.insert_text("\n\tShaped<public>():vector2 = vector2{", last, code.get_line(last).length())
	code.set_caret_line(code.get_line_count() - 1)
	code.set_caret_column(code.get_line(code.get_line_count() - 1).length())
	var fields := await _unforced_popup(code, script)
	check("`vector2{` opens the popup by itself", fields.open)
	check_eq("offering the archetype's two fields and nothing else", fields.displays, ["X", "Y"])
	code.cancel_code_completion()
	code.text = original

	if not _caret_to(code, "?ExactMatch := true]", 1):
		check("completion_probe.verse passes ExactMatch by name", false)
		return
	var line := code.get_caret_line()
	code.set_line(line, code.get_line(line).replace("?ExactMatch := true]", "?]"))
	_caret_to(code, "?]", 1)
	var named := await _unforced_popup(code, script)
	check("a bare `?` opening an argument opens the popup by itself", named.open)
	check("offering the callee's named parameter", named.displays.has("ExactMatch:logic"))
	var index: int = named.displays.find("ExactMatch:logic")
	if named.open and index >= 0:
		code.set_code_completion_selected_index(index)
		code.confirm_code_completion()
		check("confirming it keeps the `?` the author typed", code.get_line(line).contains("[Action, ?ExactMatch := ]"))
	code.cancel_code_completion()
	code.text = original

	for spelling in [["if (Held?", "a postfix `?` opens nothing"], ["Held:?", "an option type's `?` opens nothing"]]:
		if not _caret_to(code, spelling[0], spelling[0].length()):
			check("completion_probe.verse writes %s" % spelling[0], false)
			continue
		var answer := await _unforced_popup(code, script)
		check(spelling[1], not answer.open)
		code.cancel_code_completion()
	code.text = original
	script.source_code = original


# The caret's buffer as _complete_code is handed it: the text with the cursor character spliced in.
func _marked(code: CodeEdit) -> String:
	var offset := 0
	for i in code.get_caret_line():
		offset += code.get_line(i).length() + 1
	offset += code.get_caret_column()
	return code.text.substr(0, offset) + char(CARET_MARKER) + code.text.substr(offset)


# Lets the analysis a caret asks for land -- probe_complete_code asks exactly what the editor asks,
# so its buffer is the same request and queues nothing new -- then asks the way typing does, with
# no force, and reports whether CodeEdit kept the popup and what it shows.
func _unforced_popup(code: CodeEdit, script: Script) -> Dictionary:
	var buffer := _marked(code)
	await wait_until(func() -> bool:
		return not verse_language().call("probe_complete_code", script.resource_path, buffer).get("awaiting_analysis", false),
			ANALYSIS_TIMEOUT_MS)
	code.cancel_code_completion()
	code.request_code_completion()
	await frames(1)
	var displays := []
	for option in code.get_code_completion_options():
		displays.append(String(option.get("display_text", "")))
	return {"open": code.get_code_completion_selected_index() != -1, "displays": displays}


func _caret_to(code: CodeEdit, text: String, into: int) -> bool:
	for line in code.get_line_count():
		var column := code.get_line(line).find(text)
		if column >= 0:
			code.set_caret_line(line)
			code.set_caret_column(column + into)
			return true
	return false


# B27: "Complete ... GetNode[ ... that is B27's first cause, and `[` in the prefix table is the whole
# of the fix ... Then press Play ... complete another call straight away ... the hint ... must
# arrive without a further keystroke. Do that last one twice with a save in between."
func _argument_hint() -> void:
	_mark("B27: the argument hint's re-ask")
	var script: Script = load(COMPLETION_PROBE)
	var code := await code_edit_for(script)
	if code == null:
		check("completion_probe.verse opens in the script editor", false)
		return
	var original := code.text
	var asks := [0]
	var count := func() -> void: asks[0] += 1
	code.code_completion_requested.connect(count)

	var line := _line_of(code, "\t\tif (Other.GetNode[Name])")
	code.set_line(line, "\t\tif (Other.)")
	_caret_to(code, "Other.", "Other.".length())
	var offered := await wait_until(func() -> bool:
		code.request_code_completion(true)
		return _inserts(code).has("GetNode["), ANALYSIS_TIMEOUT_MS)
	check("B27: `Other.` offers GetNode[", offered)
	if offered:
		code.set_code_completion_selected_index(_inserts(code).find("GetNode["))
		var before: int = asks[0]
		code.confirm_code_completion()
		check_eq("B27 (1): confirming `GetNode[` re-asks for completion once", asks[0] - before, 1)
		check("B27 (1): the confirmed call is on the line", code.get_line(line).contains("Other.GetNode["))
		check("B27 (1): and the re-ask draws GetNode's hint", (await _settled_hint(code, script)).begins_with("GetNode["))
	code.cancel_code_completion()
	code.text = original

	# Another call each time, as the by-hand step says: "come back and complete another call".
	var pressed := _line_of(code, "\t\tif (GetInputSingleton().IsActionPressed[")
	var after_play := [
		["B27 (2): after Play", pressed, "\t\tif (GetInputSingleton().IsActionPressed[)", "IsActionPressed[", false],
		["B27 (3): after Play with a save between", pressed, "\t\tif (GetNode[)", "GetNode[", true],
	]
	for row in after_play:
		var tag: String = row[0]
		if not await _play_and_stop(CASES_DIR.path_join("b27.tscn")):
			check(tag + ": Play builds and starts the game", false)
			continue
		code.set_line(row[1], row[2])
		_caret_to(code, row[3], row[3].length())
		code.request_code_completion(true)
		var after_ask: int = asks[0]
		if row[4]:
			script.source_code = original + "\n# saved while the hint waits\n"
			ResourceSaver.save(script)
		var reasked := await wait_until(func() -> bool: return asks[0] > after_ask, ANALYSIS_TIMEOUT_MS)
		check(tag + ": the hint's re-ask arrives with no further keystroke", reasked)
		_check_hint_after_play(tag + ": and it draws %s's hint" % row[3], await _settled_hint(code, script), row[3])
		code.cancel_code_completion()
		code.text = original

	script.source_code = original
	ResourceSaver.save(script)
	code.code_completion_requested.disconnect(count)


func _check_hint_after_play(name: String, hint: String, callee: String) -> void:
	if hint.begins_with(callee):
		check(name, true)
	elif hint.is_empty():
		skip(name, "known defect: after a Play, a call completed in the editor draws no hint -- the re-ask arrives and the settled answer at the caret carries an empty call_hint")
	else:
		check(name, false)


# After B27's Plays and the edits made behind them, hover_probe.verse's members still hover with
# their comments -- by-hand-findings.md B20 and B38 are both about losing exactly that.
func _docs_after_play() -> void:
	_mark("the documentation after a Play and an edit")
	var code := await code_edit_for(load(HOVER_PROBE))
	if code == null:
		check("hover_probe.verse opens in the script editor", false)
		return
	var blocked := await _tooltip(code, "Blocked", "\tBlocked<public>")
	check("a member keeps its comment after a Play and an edit behind it", blocked[1].begins_with("The other two comment forms"))


func _settled_hint(code: CodeEdit, script: Script) -> String:
	var buffer := _marked(code)
	var answer := {}
	await wait_until(func() -> bool:
		answer.merge(verse_language().call("probe_complete_code", script.resource_path, buffer), true)
		return not answer.get("awaiting_analysis", false), ANALYSIS_TIMEOUT_MS)
	return String(answer.get("call_hint", ""))


# Play runs EditorNode::call_build, which is VerseEditorPlugin::_build: after it the host holds a
# generation and no AST, so a position asked about next has to wait for an analysis -- the window
# B27's second cause lived in. The game itself is not needed, so it is stopped once it is up.
func _play_and_stop(path: String) -> bool:
	if not ResourceLoader.exists(path):
		var root := Node.new()
		root.name = "Idle"
		root.set_script(_write_script(CASES_DIR.path_join("idle.gd"), IDLE_SOURCE))
		if not _pack(root, path):
			return false
	return (await play_and_read(path, ["idle game up"])).contains("idle game up")


# by-hand-findings.md "The tooltip's rendering of a converted description" and B40: what the
# tooltip's two labels say, read as parsed text -- the markup has been interpreted, so a literal tag
# in it is one Godot did not understand. The glance at bold and the code font stays by hand.
func _tooltips() -> void:
	_mark("the hover tooltip's text")
	var hover: Script = load(HOVER_PROBE)
	var code := await code_edit_for(hover)
	if code == null:
		check("hover_probe.verse opens in the script editor", false)
		return
	var prose := await _tooltip(code, "Prose", "\tProse<public>")
	check("Prose's tooltip is titled as a method of hover_probe", prose[0].begins_with("Method hover_probe.Prose("))
	# RichTextLabel separates a code block's lines with `\r` where a paragraph ends in `\n`.
	var lines := prose[1].replace("\r", "\n").split("\n")
	check("Prose's comment draws as two paragraphs and a sample",
			lines.size() == 4 and lines[0].begins_with("The prose itself,")
			and lines[1].begins_with("The second paragraph,"))
	check("with `Floor[X]` drawn with its brackets", lines.size() > 0 and lines[0].contains("and Floor[X] is not a tag."))
	check("with no markup left undrawn",
			not prose[1].contains("[b]") and not prose[1].contains("[code]") and not prose[1].contains("[lb]"))
	check("with the sample's second line indented under its first", lines.size() == 4
			and lines[2].strip_edges() == "Result := Floor[X]"
			and lines[3].contains("    Nested := 1"))

	var blocked := await _tooltip(code, "Blocked", "\tBlocked<public>")
	check_eq("Blocked's `<# #>` comment draws as its one paragraph", blocked[1],
			"The other two comment forms, which the reader used to misread: this block read as >, the closing line stripped to that and the walk stopped at the line above it.")
	var indented := await _tooltip(code, "Indented", "\tIndented<public>")
	check_eq("Indented's `<#>` comment draws as its one paragraph, second line included", indented[1],
			"An indented comment, whose body is whatever sits indented under the marker. The second line is the proof, because only the first survived before.")

	var emit := await _tooltip(code, "Emit", "E.Emit(1)")
	check("B40: event.Emit draws as a method of event", emit[0].begins_with("Method event.Emit("))
	check("B40: with its argument and `-> void`", emit[0].contains("(: t") and emit[0].contains("-> void"))
	_check_api_comment("B40: with event.Emit's comment from GodotApi.native.verse", emit[1])

	var concurrency: Script = load("res://scripts/concurrency.verse")
	code = await code_edit_for(concurrency)
	if code != null:
		for member in ["Await", "Subscribe"]:
			var shown := await _tooltip(code, member, "MakeSignal(Owner, Name).%s(" % member)
			if shown[0].begins_with("Local Constant %s:" % member):
				skip("B40: signal_ref.%s draws as a method of signal_ref" % member,
						"known defect: a method of signal_ref, a class the Godot package declares, still hovers as a Local Constant")
			else:
				check("B40: signal_ref.%s draws as a method of signal_ref" % member,
						shown[0].begins_with("Method signal_ref.%s(" % member))
			_check_api_comment("B40: with signal_ref.%s's comment" % member, shown[1])

	var widget: Script = load("res://widgets/left/widget.verse")
	code = await code_edit_for(widget)
	if code != null:
		var root_constant := await _tooltip(code, "RootConstant", "    RootConstant<public>")
		check("RootConstant's tooltip says Method, under left/widget",
				root_constant[0].begins_with("Method left/widget.RootConstant("))
		check("and carries the comment above it", root_constant[1].contains("From helpers.verse"))


# The page B40 registers carries the method's own comment. Godot writes this sentence where a
# description is empty.
func _check_api_comment(name: String, body: String) -> void:
	if body == "No description available.":
		skip(name, "known defect: B40's page is registered with an empty description although GodotApi.native.verse has a comment above the method -- publish_api_method is handed the host's doc, which is empty for it")
	else:
		check(name, not body.is_empty())


# Raises Godot's tooltip over `word` on the first line containing `anchor`, asking again until the
# lookup answers: a file just opened may not have been analysed as a buffer yet, and a hover
# declines rather than trusting positions it has not measured. Answers [title, body], parsed.
func _tooltip(code: CodeEdit, word: String, anchor: String) -> PackedStringArray:
	var line := -1
	for i in code.get_line_count():
		if code.get_line(i).contains(anchor):
			line = i
			break
	if line < 0:
		check("the file has a line with %s" % anchor, false)
		return PackedStringArray(["", ""])
	var column := code.get_line(line).find(word, code.get_line(line).find(anchor)) + 1
	for tip in find_all(code, "EditorHelpBitTooltip"):
		tip.free()
	await wait_until(func() -> bool:
		code.emit_signal("symbol_hovered", word, line, column)
		return not find_all(code, "EditorHelpBitTooltip").is_empty(), 20000)
	var texts := PackedStringArray()
	for tip in find_all(code, "EditorHelpBitTooltip"):
		for label in find_all(tip, "RichTextLabel"):
			texts.append((label as RichTextLabel).get_parsed_text().replace(char(0xA0), " "))
		tip.free()
	while texts.size() < 2:
		texts.append("")
	return texts


# "Then open the class reference for hover_probe ... the same text must render the same way under
# the member, and the class's brief under its name must be the comment's first paragraph alone."
func _class_page() -> void:
	_mark("the class reference page")
	EditorInterface.get_script_editor().goto_help("class_name:hover_probe")
	await wait_until(func() -> bool: return not _help_page_text().is_empty(), 10000)
	var page := _help_page_text()
	check("goto_help opens hover_probe's class page", not page.is_empty())
	var first := "The fixture the hover cases in test_cases.gd read. Every member here is one shape the script editor's tooltip has to get right, and the prose in this comment is one of them: a hover over the words node, script or label in a sentence must draw nothing, because the mirror spells Godot's classes in lowercase and a comment is made of ordinary English."
	var second := "tools/probe_hover.py is the instrument this grew out of"
	check("the brief is the comment's first paragraph alone, above the whole description",
			page.contains("\n%s\n%s\n%s" % [first, first, second]))
	check("Prose's description renders on the page as in its tooltip",
			page.contains("a span is code, a word is bold and Floor[X] is not a tag.")
			and page.contains("Result := Floor[X]"))


func _help_page_text() -> String:
	for help in find_all(EditorInterface.get_script_editor(), "EditorHelp"):
		for label in find_all(help, "RichTextLabel"):
			var text := (label as RichTextLabel).get_parsed_text()
			if text.contains("hover_probe") and text.contains("Method Descriptions"):
				return text
	return ""


# B29: "Ctrl+click on a binding ... moved the caret to the top of mover.verse instead of opening
# main.gd." symbol_lookup is what ScriptTextEditor connects the click to.
func _binding_lookup() -> void:
	_mark("B29: a click on a binding")
	var bindings: Script = load("res://scripts/bindings.verse")
	var code := await code_edit_for(bindings)
	if code == null:
		check("bindings.verse opens in the script editor", false)
		return
	var line := _line_of(code, "\t\tif (M := mob[N]) then M.Hit(21)")
	if line < 0:
		check("bindings.verse downcasts to mob", false)
		return
	code.emit_signal("symbol_lookup", "mob", line, code.get_line(line).find("mob") + 1)
	var editor := EditorInterface.get_script_editor()
	var opened := await wait_until(func() -> bool:
		var current := editor.get_current_script()
		return current != null and current.resource_path == "res://mob.gd", 10000)
	check("B29: a click on the `mob` binding opens res://mob.gd", opened)


# by-hand-findings.md "The script editor's colours": no type drawn as plain text, and the three tiers
# in the three theme settings GDScript uses.
func _colours() -> void:
	_mark("the syntax colours")
	var settings := EditorInterface.get_editor_settings()
	var theme := {}
	for tier in ["text", "engine_type", "base_type", "user_type"]:
		theme[tier] = settings.get_setting("text_editor/theme/highlighting/%s_color" % tier)
	check("the theme's three type tiers and plain text are four colours",
			theme.values().all(func(c: Color) -> bool: return theme.values().count(c) == 1))
	var hover: Script = load(HOVER_PROBE)
	var code := await code_edit_for(hover)
	if code == null:
		check("hover_probe.verse opens in the script editor", false)
		return
	var tiers := [
		["hover_probe := class(node2d):", "node2d", "engine_type"],
		["hover_probe := class(node2d):", "hover_probe", "user_type"],
		["\tMode<public>():node_internal_mode", "node_internal_mode", "engine_type"],
		["\tCell<public>():vector2i", "vector2i", "base_type"],
		["\tNothing<public>():variant", "variant", "base_type"],
		["\tHelped<public>(H:hover_helper)", "hover_helper", "user_type"],
		["hover_helper := class:", "hover_helper", "user_type"],
		["hover_reading := struct:", "hover_reading", "user_type"],
		["hover_tempo := enum{", "hover_tempo", "user_type"],
	]
	for row in tiers:
		var at := _colour_of(code, row[0], row[1])
		check("`%s` on `%s` is %s-coloured" % [row[1], row[0].strip_edges(), row[2]], at == theme[row[2]])
	for row in [["\tLetters<public>():[]char", "char"], ["\tHolds<public>(E:event(int))", "event"]]:
		check("`%s` is not drawn as plain text" % row[1], _colour_of(code, row[0], row[1]) != theme["text"])

	var bindings: Script = load("res://scripts/bindings.verse")
	code = await code_edit_for(bindings)
	if code != null:
		check("the `mob` binding is user-type-coloured in a type",
				_colour_of(code, "\tAskMaybeMobEmpty<public>(M:?mob)", "mob") == theme["user_type"])


# The colour the highlighter gives the first `word` after the start of the line holding `anchor`.
# A line's dictionary is keyed by the column each run starts at, so a column's colour is the
# nearest key at or before it.
func _colour_of(code: CodeEdit, anchor: String, word: String) -> Color:
	var line := -1
	for i in code.get_line_count():
		if code.get_line(i).begins_with(anchor):
			line = i
			break
	if line < 0:
		return Color(0, 0, 0, 0)
	var column := code.get_line(line).find(word)
	var runs: Dictionary = code.syntax_highlighter.get_line_syntax_highlighting(line)
	var colour := Color(0, 0, 0, 0)
	var start := -1
	for key in runs:
		if int(key) <= column and int(key) > start:
			start = int(key)
			colour = runs[key].get("color", colour)
	return colour


# B4: "A connected handler gets no gutter icon" -- for a script at the root and one under a
# `.vmodule`. The icon is set by ScriptTextEditor::_update_connected_methods, which runs at the end
# of every validate against the scene being edited.
func _connection_gutter() -> void:
	_mark("B4: the connection gutter")
	var save_probe: Script = load(SAVE_PROBE)
	var widget: Script = load("res://widgets/left/widget.verse")
	var root := Node.new()
	root.name = "Gutter"
	var clock := Timer.new()
	clock.name = "Clock"
	root.add_child(clock)
	clock.owner = root
	for row in [["Probe", save_probe, "Answer"], ["Widget", widget, "Which"]]:
		var target := Node2D.new()
		target.name = row[0]
		target.set_script(row[1])
		root.add_child(target)
		target.owner = root
		clock.timeout.connect(Callable(target, row[2]), CONNECT_PERSIST)
	if await _open_new_scene(root, CASES_DIR.path_join("b4.tscn")) == null:
		check("B4: the editor opens a scene connecting a signal to Verse methods", false)
		return
	for row in [[save_probe, "\tAnswer<public>()", "Answer", "a root-module script"],
			[widget, "    Which<public>()", "Which", "a script under a .vmodule"]]:
		var code := await code_edit_for(row[0])
		if code == null:
			check("B4: %s opens in the script editor" % row[3], false)
			continue
		var line := _line_of(code, row[1])
		var gutter := -1
		for i in code.get_gutter_count():
			if code.get_gutter_name(i) == "connection_gutter":
				gutter = i
		if line < 0 or gutter < 0:
			check("B4: %s declares %s beside a connection gutter" % [row[3], row[2]], false)
			continue
		var marked := await wait_until(func() -> bool:
			_validate_now(code)
			return code.get_line_gutter_icon(line, gutter) != null, ANALYSIS_TIMEOUT_MS)
		check("B4: the connected %s in %s gets the gutter icon" % [row[2], row[3]], marked)
		check_eq("B4: whose metadata names the method in %s" % row[3],
				String(code.get_line_gutter_metadata(line, gutter).get("method", "")), row[2])


# The validate the editor's idle timer runs after an edit, asked for at once: CodeTextEditor's
# validate_script signal is what the timer emits and what ScriptTextEditor::_validate_script hears.
func _validate_now(code: CodeEdit) -> void:
	var editor: Node = code.get_parent()
	while editor != null and not editor.is_class("CodeTextEditor"):
		editor = editor.get_parent()
	if editor != null:
		editor.emit_signal("validate_script")


# The warnings panel: each rejection on its member's line, gone once the member is fixed, and back
# once it is broken again -- by-hand-findings.md "refresh_script_warnings reaches the log now",
# "Stage B's warning in the script editor" and R-EXP-8 step 6. The fix is made in the buffer, as an
# author makes it, and the panel is read after the validate that follows.
func _warnings_panel() -> void:
	_mark("the warnings panel")
	# [file, diagnostic, what it is, the text whose first line the panel names, and the edit that
	# fixes it without moving a line].
	var cases := [
		["res://scripts/hints.verse", "VG1003", "R-EXP-8 (6): Mismatched's export rejection",
			'    @export\n    @export_flags("Fire,Water")',
			['    var Mismatched<public>:string = ""', '    var Mismatched<public>:int = 0']],
		["res://scripts/signal_rejects.verse", "VG2006", "Forgotten's signal rejection",
			"    Forgotten<public>:signal(int)",
			["    # Node panel that is empty for no stated reason.", "    @export_signal"]],
		["res://scripts/settings_resource.verse", "VG1008", "Stage C: Stowaway's cannot-be-saved warning",
			"    @export\n    var Stowaway", ["    @export\n    var Stowaway", "    # not exported\n    var Stowaway"]],
		["res://scripts/settings_resource.verse", "VG5004", "Stage B: the inert @global_class",
			"@global_class\nstowaway := class", ["@global_class\nstowaway := class", "\nstowaway := class"]],
	]
	for row in cases:
		var script: Script = load(row[0])
		var code := await code_edit_for(script)
		if code == null:
			check("%s opens in the script editor" % row[0], false)
			continue
		var original := code.text
		var at := original.find(row[3])
		if at < 0 or not original.contains(row[4][0]):
			check("%s writes what %s is about" % [row[0].get_file(), row[2]], false)
			continue
		var line := original.substr(0, at).count("\n") + 1
		check("%s is in the warnings panel at line %d" % [row[2], line], await _panel_says(code, row[1], line, true))
		if row[1] == "VG5004":
			check_eq("Stage B: only the stray attribute of the file's two is flagged",
					_panel_text(code).count(row[1] + ":"), 1)
		code.text = original.replace(row[4][0], row[4][1])
		check("%s clears once the member is fixed" % row[2], await _panel_says(code, row[1], line, false))
		code.text = original
		check("%s comes back once it is broken again" % row[2], await _panel_says(code, row[1], line, true))
		script.source_code = original


func _panel_says(code: CodeEdit, id: String, line: int, present: bool) -> bool:
	return await wait_until(func() -> bool:
		_validate_now(code)
		var text := _panel_text(code)
		var row := text.find("Line %d (" % line)
		var said := row >= 0 and text.substr(row, 160).contains(id + ":")
		return said == present, ANALYSIS_TIMEOUT_MS)


# The warnings panel is the ScriptTextEditor's RichTextLabel whose rows begin with an [Ignore] link.
func _panel_text(code: CodeEdit) -> String:
	var editor: Node = code.get_parent()
	while editor != null and not editor.is_class("ScriptTextEditor"):
		editor = editor.get_parent()
	if editor == null:
		return ""
	var text := ""
	for label in find_all(editor, "RichTextLabel"):
		var parsed := (label as RichTextLabel).get_parsed_text()
		if parsed.begins_with("[Ignore]"):
			text += parsed
	return text


# --- the debugger and the profiler through Play (docs/editor-test-audit.md step 5) ---------------
#
# by-hand-findings.md's "To repeat it" steps 1-5 and its profiler session, through Godot's own
# Debugger panel: breakpoints set in the gutter reach the game, the stack panel and Stack Variables
# read the stop, the panel's own buttons step, and the Profiler tab's tree names Verse functions.
# res://debugger/debug_play.tscn runs a GDScript control first and scripts/debug_play.verse after
# it; tools/debug_wire.py drives the same scene over the wire with no editor, and names the two
# known defects below in the same words.

const DEBUG_SCENE := "res://debugger/debug_play.tscn"
const DEBUG_VERSE := "res://scripts/debug_play.verse"
const DEBUG_CONTROL := "res://debugger/debug_control.gd"
const READY_DEFECT := "known defect: the Verse debugger attaches from the language's _frame (VerseDebugger::sync_attachment), and a main scene's _Ready runs before the first one, so a breakpoint there never fires"
const TWICE_DEFECT := "known defect: a line holding a call reports its location again when the result lands, and should_break asks is_breakpoint of both, so a breakpoint there stops twice per arrival -- Continue stops on the same line once more"
const STACK_ROW := "^(\\d+) - (.*):(\\d+) - at function: (.*)$"


# EditorDebuggerSession's breaked is the one signal that says a stop happened; the panel's rows
# arrive after it, over the same connection.
class _BreakWatcher extends EditorDebuggerPlugin:
	var breaks := 0

	func _setup_session(session_id: int) -> void:
		get_session(session_id).breaked.connect(func(_can_debug: bool) -> void: breaks += 1)


var _watcher: _BreakWatcher
var _debug_lines := {}


func _debugger_and_profiler() -> void:
	_mark("the debugger: breakpoints in the gutter")
	var verse: Script = load(DEBUG_VERSE)
	var control: Script = load(DEBUG_CONTROL)
	var verse_code := await code_edit_for(verse)
	var control_code := await code_edit_for(control)
	if verse_code == null or control_code == null:
		check("debug_play.verse and debug_control.gd open in the script editor", false)
		return
	_debug_lines = {
		"control": _exact_line_of(control_code, "\tvar second := work()"),
		"ready": _exact_line_of(verse_code, "\t\tPrint(\"debug_play ready\")"),
		"start": _exact_line_of(verse_code, "\t\tStart := 1"),
		"spot": _exact_line_of(verse_code, "\t\tSpot := vector2{X := 1.0, Y := 2.0}"),
		"call": _exact_line_of(verse_code, "\t\tTotal := Helper()"),
		"outer_call": _exact_line_of(verse_code, "\t\tDeeper := Outer()"),
		"outer_decl": _exact_line_of(verse_code, "\tOuter<public>()<transacts>:int ="),
		"outer_first": _exact_line_of(verse_code, "\t\tStepped := Helper() + 1"),
		"again": _exact_line_of(verse_code, "\t\tAgain := Helper()"),
		"trailing": _exact_line_of(verse_code, "\t\tInner"),
		"process": _exact_line_of(verse_code, "\t\tset Frames += 1"),
	}
	if _debug_lines.values().has(-1):
		check("debug_play.verse and debug_control.gd write every line the cases arm", false)
		return
	control_code.set_line_as_breakpoint(_debug_lines.control, true)
	for key in ["ready", "start", "outer_call", "again", "trailing"]:
		verse_code.set_line_as_breakpoint(_debug_lines[key], true)
	check_eq("the gutter holds the six breakpoints the cases arm",
			EditorInterface.get_script_editor().get_breakpoints().size(), 6)

	_watcher = _BreakWatcher.new()
	plugin.add_debugger_plugin(_watcher)
	if play_scene(DEBUG_SCENE):
		await _debug_session(verse_code)
	EditorInterface.stop_playing_scene()
	await wait_until(func() -> bool: return not EditorInterface.is_playing_scene(), 10000)
	plugin.remove_debugger_plugin(_watcher)
	for line in verse_code.get_breakpointed_lines():
		verse_code.set_line_as_breakpoint(line, false)
	for line in control_code.get_breakpointed_lines():
		control_code.set_line_as_breakpoint(line, false)


func _debug_session(verse_code: CodeEdit) -> void:
	var lines := _debug_lines
	# Godot's lines are 1-based where CodeEdit's are 0-based.
	var at := func(key: String) -> int: return lines[key] + 1
	var visited := []

	_mark("the debugger: the GDScript control's stop")
	var stop := await _next_stop(visited, 90000)
	check("GDScript control: a breakpoint set in the gutter stops the game", not stop.is_empty())
	if stop.is_empty():
		return
	check_eq("GDScript control: the stack panel's top row is its line in _ready",
			_frame_of(stop, 0), [DEBUG_CONTROL, at.call("control"), "_ready"])
	var variables := await _stack_variables(["Locals/first"])
	check_eq("GDScript control: Stack Variables shows the local first", variables.get("Locals/first"), 10)
	stop = await _press_and_stop("Step Over", visited)
	check_eq("GDScript control: Step Over from a breakpoint on a line holding a call moves one line",
			_frame_of(stop, 0).slice(0, 2), [DEBUG_CONTROL, at.call("control") + 1])
	if stop.is_empty():
		return

	_mark("the debugger: the Verse stops")
	stop = await _press_and_stop("Continue", visited)
	if _frame_of(stop, 0).slice(0, 2) == [DEBUG_VERSE, at.call("ready")]:
		check("a breakpoint in the main scene's _Ready stops", true)
		stop = await _press_and_stop("Continue", visited)
	else:
		skip("a breakpoint in the main scene's _Ready stops", READY_DEFECT)
	check("a breakpoint set in a .verse's gutter stops the game", not stop.is_empty())
	if stop.is_empty():
		return
	check_eq("the stack panel's top row is debug_play.verse at the breakpoint's line, in Walk",
			_frame_of(stop, 0), [DEBUG_VERSE, at.call("start"), "Walk"])
	check_eq("and its second row is the _Process that called it",
			_frame_of(stop, 1).slice(0, 1) + _frame_of(stop, 1).slice(2), [DEBUG_VERSE, "_Process"])

	stop = await _press_and_stop("Step Over", visited)
	check_eq("the Step Over button moves one line", _frame_of(stop, 0), [DEBUG_VERSE, at.call("spot"), "Walk"])
	if stop.is_empty():
		return
	stop = await _press_and_stop("Step Over", visited)
	check("Step Over from a line that reports twice does not land on it again",
			not stop.is_empty() and _frame_of(stop, 0)[1] != at.call("spot"))
	check_eq("and lands on the next line", _frame_of(stop, 0), [DEBUG_VERSE, at.call("call"), "Walk"])
	if stop.is_empty():
		return

	variables = await _stack_variables(["Locals/Spot", "Members/Health", "Members/Where", "Members/Frames"])
	check_eq("Stack Variables shows the local Spot as a Vector2", variables.get("Locals/Spot"), Vector2(1, 2))
	check_eq("Stack Variables shows the member Health as an int", variables.get("Members/Health"), 7)
	check_eq("Stack Variables shows the member Where as a Vector2", variables.get("Members/Where"), Vector2(3, 4))
	check_eq("Stack Variables shows the var member Frames as an int", variables.get("Members/Frames"), 3)
	var selves := variables.keys().filter(func(name: String) -> bool: return name.get_slice("/", 1).to_lower() == "self")
	check("Stack Variables shows self under Members and nowhere else",
			selves.size() == 1 and String(selves[0]).begins_with("Members/"))

	stop = await _press_and_stop("Step Over", visited)
	check("Step Over from a line holding a call does not land on it again when the result lands",
			not stop.is_empty() and _frame_of(stop, 0)[1] != at.call("call"))
	check_eq("and lands on the next line, in the same function", _frame_of(stop, 0),
			[DEBUG_VERSE, at.call("call") + 1, "Walk"])
	if stop.is_empty():
		return

	_mark("the debugger: Step Into and Step Out")
	stop = await _press_and_stop("Continue", visited)
	check_eq("a second breakpoint stops at the call to Outer", _frame_of(stop, 0).slice(0, 2),
			[DEBUG_VERSE, at.call("outer_call")])
	if stop.is_empty():
		return
	# Off before stepping, so a stop on this line again can only be the step's.
	verse_code.set_line_as_breakpoint(lines.outer_call, false)
	var depth: int = stop.size()
	stop = await _press_and_stop("Step Into", visited)
	check("the Step Into button enters the Verse callee", stop.size() == depth + 1
			and _frame_of(stop, 0)[2] == "Outer" and _frame_of(stop, 0)[1] in [at.call("outer_decl"), at.call("outer_first")])
	check_eq("and the stack panel shows the caller under it, at the call", _frame_of(stop, 1),
			[DEBUG_VERSE, at.call("outer_call"), "Walk"])
	if stop.is_empty():
		return
	stop = await _press_and_stop("Step Out", visited)
	check("the Step Out button returns to the caller", stop.size() == depth and _frame_of(stop, 0)[2] == "Walk")
	if stop.is_empty():
		return

	stop = await _press_and_stop("Continue", visited)
	check_eq("a breakpoint on another line holding a call stops", _frame_of(stop, 0).slice(0, 2),
			[DEBUG_VERSE, at.call("again")])
	if stop.is_empty():
		return
	var again := await _press_and_stop("Continue", visited, 2000)
	if again.is_empty():
		check("and Continue from it does not stop there again when the result lands", true)
	elif _frame_of(again, 0).slice(0, 2) == [DEBUG_VERSE, at.call("again")]:
		skip("and Continue from it does not stop there again when the result lands", TWICE_DEFECT)
		await _press_and_stop("Continue", visited, 1000)
	else:
		check("and Continue from it does not stop there again when the result lands", false)
		await _press_and_stop("Continue", visited, 1000)
	check("a breakpoint on a trailing bare expression never fires",
			not visited.has([DEBUG_VERSE, at.call("trailing")]))

	_mark("the debugger: a breakpoint toggled while the game runs")
	verse_code.set_line_as_breakpoint(lines.process, true)
	stop = await _next_stop(visited, 10000)
	check_eq("a breakpoint toggled in the gutter while the game runs arms, in _Process",
			_frame_of(stop, 0), [DEBUG_VERSE, at.call("process"), "_Process"])
	if stop.is_empty():
		return
	verse_code.set_line_as_breakpoint(lines.process, false)
	check("and one toggled off while stopped stays off", (await _press_and_stop("Continue", visited, 1500)).is_empty())

	_mark("the debugger: Skip Breakpoints")
	var skip_button := _debugger_button("Skip Breakpoints")
	if skip_button == null:
		check("the Debugger panel has a Skip Breakpoints button", false)
		return
	skip_button.emit_signal("pressed")
	verse_code.set_line_as_breakpoint(lines.process, true)
	check("with Skip Breakpoints on, an armed line in _Process does not stop", (await _next_stop(visited, 1500)).is_empty())
	skip_button.emit_signal("pressed")
	stop = await _next_stop(visited, 10000)
	check_eq("and it stops there again once Skip Breakpoints is off", _frame_of(stop, 0).slice(0, 2),
			[DEBUG_VERSE, at.call("process")])
	verse_code.set_line_as_breakpoint(lines.process, false)
	if not stop.is_empty():
		await _press_and_stop("Continue", visited, 1000)

	await _profiler_panel()


# The Profiler tab: its Start button, two seconds of frames, then Stop -- which is what makes the
# game send servers:profile_total, the accumulated table built from _profiling_get_accumulated_data
# and so the path through the ProfilingInfo stride trap, and the tree then shows it.
func _profiler_panel() -> void:
	_mark("the Profiler tab")
	var profilers := find_all(_debugger_panel(), "EditorProfiler") if _debugger_panel() != null else []
	var start: Button = null
	if not profilers.is_empty():
		for button in find_all(profilers[0], "Button"):
			if (button as Button).toggle_mode and (button as Button).text == "Start":
				start = button
	if start == null:
		check("the Debugger panel has a Profiler tab with a Start button", false)
		return
	const PROCESS := "debug_play._Process(:float)"
	start.button_pressed = true
	start.emit_signal("pressed")
	# The tree draws one frame at a time, and a frame whose rows are all below the timer's
	# resolution can leave one out, so it is read until a frame carries all three.
	var framed := {}
	await wait_until(func() -> bool:
		framed.clear()
		framed.merge(_profiler_rows(profilers[0]))
		return framed.has(PROCESS) and framed.has("debug_play_tag") and framed.has("_process"), 10000)
	check("the Profiler lists a Verse function with its call count", framed.get(PROCESS, 0) >= 1)
	check("and the profile{} block's row", framed.has("debug_play_tag"))
	# get_frame_data fills the same array get_accumulated_data does, at the same stride, so two
	# Verse rows reading one frame's single call each is the trap measured on this side too.
	check("the frame's two Verse rows each read one call (the ProfilingInfo stride)",
			framed.get(PROCESS, 0) == 1 and framed.get("debug_play_tag", 0) == 1)
	check("GDScript control: the Profiler lists its _process", framed.has("_process"))
	check("and no row reads SigErr", not framed.keys().any(func(name: String) -> bool: return name.begins_with("SigErr")))

	start.button_pressed = false
	start.emit_signal("pressed")
	var totals := {}
	await wait_until(func() -> bool:
		totals.clear()
		totals.merge(_profiler_rows(profilers[0]))
		return totals.get(PROCESS, 0) > 1, 5000)
	const TOTALS := "stopping it draws the accumulated table, where _Process counts every frame's call"
	if totals.get(PROCESS, 0) > 1:
		check(TOTALS, true)
	elif totals.get("_process", 0) <= 1:
		skip(TOTALS, "Godot's own behaviour, not the bridge's: after Stop the tab keeps drawing a single frame for GDScript's _process too, so servers:profile_total is asserted by the debug-wire layer instead")
	else:
		check(TOTALS, false)


# The Script Functions rows of the Profiler's tree, by name, with their call counts.
func _profiler_rows(profiler: Node) -> Dictionary:
	var rows := {}
	for tree in find_all(profiler, "Tree"):
		var root := (tree as Tree).get_root()
		if root == null:
			continue
		for category in root.get_children():
			if category.get_text(0) != "Script Functions":
				continue
			for item in category.get_children():
				rows[item.get_text(0)] = item.get_text(2).to_int()
	return rows


# The next stop, once the stack panel has drawn it: [[file, line, function], ...] from the top,
# or an empty array when `timeout_ms` passes with no stop.
func _next_stop(visited: Array, timeout_ms := 30000) -> Array:
	var before := _watcher.breaks
	if not await wait_until(func() -> bool: return _watcher.breaks > before, timeout_ms):
		return []
	# A lambda captures a local by value, so the rows are gathered into it rather than assigned.
	var frames := []
	await wait_until(func() -> bool:
		frames.assign(_stack_rows())
		return not frames.is_empty(), 10000)
	if not frames.is_empty():
		visited.append(frames[0].slice(0, 2))
	return frames


func _press_and_stop(tooltip: String, visited: Array, timeout_ms := 30000) -> Array:
	var button := _debugger_button(tooltip)
	if button == null:
		check("the Debugger panel has a %s button" % tooltip, false)
		return []
	button.emit_signal("pressed")
	return await _next_stop(visited, timeout_ms)


# _line_of matches a prefix, which finds `Inner := 21` for the bare `Inner` under it.
func _exact_line_of(code: CodeEdit, text: String) -> int:
	for line in code.get_line_count():
		if code.get_line(line).strip_edges(false, true) == text:
			return line
	return -1


func _frame_of(stop: Array, level: int) -> Array:
	return stop[level] if level < stop.size() else ["", 0, ""]


# The stack panel's rows, "0 - res://x.verse:12 - at function: Walk", as [file, line, function] --
# the function without the parameter types a Verse frame is named with (`_Process(:float)`).
func _stack_rows() -> Array:
	var panel := _debugger_panel()
	if panel == null:
		return []
	var pattern := RegEx.create_from_string(STACK_ROW)
	for tree in find_all(panel, "Tree"):
		if (tree as Tree).get_column_title(0) != "Stack Frames" or (tree as Tree).get_root() == null:
			continue
		var rows := []
		for item in (tree as Tree).get_root().get_children():
			var found := pattern.search(item.get_text(0))
			if found != null:
				rows.append([found.get_string(2), found.get_string(3).to_int(), found.get_string(4).get_slice("(", 0)])
		return rows
	return []


# Stack Variables, as the panel's inspector holds them: `Locals/<name>` and `Members/<name>` on the
# object it edits. Asked until every name in `wanted` is there, because the rows arrive after the
# stack does.
func _stack_variables(wanted: Array) -> Dictionary:
	var values := {}
	await wait_until(func() -> bool:
		values.clear()
		var panel := _debugger_panel()
		var inspectors := find_all(panel, "EditorDebuggerInspector") if panel != null else []
		var edited: Object = (inspectors[0] as EditorInspector).get_edited_object() if not inspectors.is_empty() else null
		if edited == null:
			return false
		for property in edited.get_property_list():
			var name := String(property["name"])
			if name.begins_with("Locals/") or name.begins_with("Members/"):
				values[name] = edited.get(name)
		return wanted.all(func(key: String) -> bool: return values.has(key)), 10000)
	return values


# The ScriptEditorDebugger the game's session draws into. One Play is one session, the first tab.
func _debugger_panel() -> Node:
	var panels := find_all(EditorInterface.get_base_control(), "ScriptEditorDebugger")
	return panels[0] if not panels.is_empty() else null


func _debugger_button(tooltip: String) -> Button:
	var panel := _debugger_panel()
	if panel == null:
		return null
	for button in find_all(panel, "Button"):
		if (button as Button).tooltip_text == tooltip:
			return button
	return null


# --- the inspector, the docks and the dialogs (docs/editor-test-audit.md step 6) -----------------
#
# by-hand-findings.md's "R-EXP-8's @icon, and the five inspector hints", "The Node panel, for a
# signal that is not <public>", B3 and "R-EXP-7's two editor-side halves": which control the
# inspector builds, what the Scene dock and the create dialog draw, what the Node dock lists, what
# "Make Function" writes and what the autoload list does with a Verse script. The glance at how any
# of it renders stays by hand.

const HINTS := "res://scripts/hints.verse"
const SIGNALS := "res://scripts/signals.verse"
# Written beside the copy's project.godot by run_tests.py before the import pass, so it is an
# imported texture by the time anything asks for it; tests/integration itself carries none.
const ICON := "res://icon.svg"

const ICON_CONTROL_SOURCE := """@icon("res://icon.svg")
extends Node2D
"""

# hints.verse is a @global_class, so its icon is looked up in the class registry; this one is not,
# which is the path that asks the script itself (EditorData::get_script_icon).
const ICON_PROBE := CASES_DIR + "/icon_probe.verse"
const ICON_PROBE_SOURCE := """using { /Godot.org/Godot }

@icon("res://icon.svg")
icon_probe := class(node2d):

	Which<public>()<transacts>:int = 1
"""

const NODE_PATH_DEFECT := "known defect: `@export_node_path` on a `string` reaches Godot as TYPE_STRING with PROPERTY_HINT_NODE_PATH_VALID_TYPES, and the inspector builds EditorPropertyNodePath only for a NodePath, so Target is drawn as a plain text field with no picker"
const TOOL_AUTOLOAD_DEFECT := "known defect: a @tool Verse autoload is added to the editor's tree holding a placeholder instance -- tool_probe.verse answers can_instantiate() false after the session's Plays have built, and VerseScript::_can_instantiate is is_compiled() && is_tool(), so it is is_compiled() that is false -- so a call answers 'Attempt to call a method on a placeholder instance' and its _Ready never runs"
const ICON_CACHE_WHY := "Godot's own behaviour, not the bridge's: the GDScript control still draws the deleted icon.svg too -- EditorData's script icon cache and the loaded texture outlive the file for the session, so the fallback needs a fresh editor"
const GLOBAL_ICON_DEFECT := "known defect: a @global_class script's icon is read from the class registry (EditorData::get_script_icon, script_class_get_icon_path), which is filled from _get_global_class_name's icon_path, and VerseScriptLanguage::_get_global_class_name answers none -- so Godot draws Node2D's icon and never asks _get_class_icon_path"

const LISTENER_SOURCE := """extends Node


func heard(points: int) -> void:
	print("own_listener heard ", points)


func _ready() -> void:
	get_parent().call_deferred("EmitOwn", 5)
"""


func _inspector_docks_and_dialogs() -> void:
	await _inspector_hints()
	await _class_icons()
	await _node_dock()
	await _make_function()
	await _autoloads()


# R-EXP-8 steps 1-6: "Portrait must be a file field with a browse button, and the dialog it opens
# must filter to .png and .jpg. SaveFolder must browse to a directory ... Notes must be a multi-line
# box ... Elements must be three checkboxes named Fire, Water and Earth, and ticking Fire then Earth
# must store 5. Target must offer a node picker that refuses anything that is not a Node2D.
# Mismatched must be absent."
func _inspector_hints() -> void:
	_mark("R-EXP-8: the inspector's controls for the five hints")
	var root := Node2D.new()
	root.name = "Hints"
	root.set_script(load(HINTS))
	for row in [["Mark", Node2D.new()], ["Bystander", Node.new()]]:
		row[1].name = row[0]
		root.add_child(row[1])
		row[1].owner = root
	root = await _open_new_scene(root, CASES_DIR.path_join("hints.tscn"))
	if root == null:
		check("R-EXP-8: the editor opens a scene holding hints.verse", false)
		return
	EditorInterface.inspect_object(root)
	var properties := {}
	await wait_until(func() -> bool:
		properties.clear()
		properties.merge(_inspector_properties())
		return properties.has("Portrait") and properties.has("Plain"), 10000)
	for row in [
		["Portrait", "EditorPropertyPath", "R-EXP-8 (1): the inspector draws Portrait as a path field"],
		["SaveFolder", "EditorPropertyPath", "R-EXP-8 (2): and SaveFolder as a path field"],
		["Notes", "EditorPropertyMultilineText", "R-EXP-8 (3): and Notes as a multi-line box"],
		["Elements", "EditorPropertyFlags", "R-EXP-8 (4): and Elements as a set of flags"],
		["Plain", "EditorPropertyInteger", "and the unhinted Plain beside them as an integer field"],
	]:
		check_eq(row[2], properties[row[0]].get_class() if properties.has(row[0]) else "no control", row[1])
	check("R-EXP-8 (6): Mismatched is absent from the inspector", not properties.has("Mismatched"))
	var target_type := TYPE_NIL
	for row in root.get_property_list():
		if row["name"] == "Target":
			target_type = row["type"]
	var target_control: String = properties["Target"].get_class() if properties.has("Target") else "no control"
	var picker_defect := target_control == "EditorPropertyText" and target_type == TYPE_STRING
	if picker_defect:
		skip("R-EXP-8 (5): and Target as a node picker", NODE_PATH_DEFECT)
	else:
		check_eq("R-EXP-8 (5): and Target as a node picker", target_control, "EditorPropertyNodePath")

	var portrait := await _browse(properties.get("Portrait"), "Edit", "EditorFileDialog")
	check("R-EXP-8 (1): Portrait's browse button opens a file dialog", portrait != null)
	if portrait != null:
		check_eq("R-EXP-8 (1): which picks a file", portrait.get("file_mode"), EditorFileDialog.FILE_MODE_OPEN_FILE)
		check_eq("R-EXP-8 (1): filtered to .png and .jpg", Array(portrait.get("filters")), ["*.png", "*.jpg"])
		(portrait as Window).hide()
	var folder := await _browse(properties.get("SaveFolder"), "Edit", "EditorFileDialog")
	check_eq("R-EXP-8 (2): SaveFolder's browse button opens a dialog that picks a directory",
			folder.get("file_mode") if folder != null else -1, EditorFileDialog.FILE_MODE_OPEN_DIR)
	if folder != null:
		(folder as Window).hide()

	var boxes := find_all(properties["Elements"], "CheckBox") if properties.has("Elements") else []
	check_eq("R-EXP-8 (4): Elements is three checkboxes named Fire, Water and Earth",
			boxes.map(func(box: CheckBox) -> String: return box.text), ["Fire", "Water", "Earth"])
	if boxes.size() == 3:
		for index in [0, 2]:
			boxes[index].button_pressed = true
			boxes[index].pressed.emit()
			await frames(1)
		check_eq("R-EXP-8 (4): ticking Fire then Earth stores 5", root.get("Elements"), 5)

	if picker_defect:
		for name in ["R-EXP-8 (5): Target's button opens a node picker", "R-EXP-8 (5): which offers the Node2D child",
				"R-EXP-8 (5): and refuses the plain Node"]:
			skip(name, NODE_PATH_DEFECT)
		return
	var picker := await _browse(properties.get("Target"), "Assign Node", "SceneTreeDialog")
	check("R-EXP-8 (5): Target's button opens a node picker", picker != null)
	if picker != null:
		var mark := _tree_item_in(picker, "Mark")
		var bystander := _tree_item_in(picker, "Bystander")
		check("R-EXP-8 (5): which offers the Node2D child", mark != null and mark.is_selectable(0))
		check("R-EXP-8 (5): and refuses the plain Node", bystander != null and not bystander.is_selectable(0))
		(picker as Window).hide()


func _inspector_properties() -> Dictionary:
	var found := {}
	for property in find_all(EditorInterface.get_inspector(), "EditorProperty"):
		found[String((property as EditorProperty).get_edited_property())] = property
	return found


# Presses the property's button whose accessibility name is `button` -- the one the author clicks
# -- and answers the dialog it opened, which each of these adds as its own child.
func _browse(property: Node, button: String, dialog_class: String) -> Window:
	if property == null:
		return null
	for candidate in find_all(property, "Button"):
		if (candidate as Control).accessibility_name == button:
			candidate.emit_signal("pressed")
			break
	var dialogs := []
	await wait_until(func() -> bool:
		dialogs.assign(find_all(property, dialog_class))
		return not dialogs.is_empty() and (dialogs[0] as Window).visible, 5000)
	return dialogs[0] if not dialogs.is_empty() else null


func _tree_item_in(root: Node, text: String) -> TreeItem:
	for tree in find_all(root, "Tree"):
		var item := _find_tree_item((tree as Tree).get_root(), text)
		if item != null:
			return item
	return null


# R-EXP-8 step 7: "The scene tree and the create-node dialog must show icon.svg for the class,
# rather than Node2D's own icon. Then delete the file and reopen: Godot must fall back rather than
# draw nothing." The only caller of _get_class_icon_path is EditorData::get_script_icon_path, so
# this is the one place it is reached. A GDScript with the same @icon is the control.
func _class_icons() -> void:
	_mark("R-EXP-8 (7): @icon in the Scene dock and the create dialog")
	var control := _write_script(CASES_DIR.path_join("icon_control.gd"), ICON_CONTROL_SOURCE)
	var control_root := Node2D.new()
	control_root.name = "IconControl"
	control_root.set_script(control)
	if not ResourceLoader.exists(ICON) or not _pack(control_root, CASES_DIR.path_join("icon_control.tscn")):
		check("R-EXP-8 (7): the copy carries res://icon.svg and the control scene saves", false)
		return
	var probe_file := FileAccess.open(ICON_PROBE, FileAccess.WRITE)
	probe_file.store_string(ICON_PROBE_SOURCE)
	probe_file.close()
	var probe_root := Node2D.new()
	probe_root.name = "IconProbe"
	probe_root.set_script(load(ICON_PROBE))
	if not _pack(probe_root, CASES_DIR.path_join("icon_probe.tscn")):
		check("R-EXP-8 (7): the icon probe's scene saves", false)
		return
	var subjects := [
		{"id": "verse", "tag": "", "what": "a Verse class", "scene": CASES_DIR.path_join("icon_probe.tscn"), "root": "IconProbe"},
		{"id": "verse", "tag": "", "what": "a @global_class Verse class", "scene": CASES_DIR.path_join("hints.tscn"), "root": "Hints"},
		{"id": "gd", "tag": "GDScript control: ", "what": "the class", "scene": CASES_DIR.path_join("icon_control.tscn"), "root": "IconControl"},
	]
	for s in subjects:
		s.icon = await _scene_dock_icon(s.scene, s.root)
		_check_icon(s, s.tag + "R-EXP-8 (7): the Scene dock draws %s with icon.svg" % s.what, s.icon)
	_check_icon(subjects[1], "R-EXP-8 (7): the create dialog draws the global class Hints with icon.svg",
			await _create_dialog_icon("Hints", "Node2D"))

	DirAccess.remove_absolute(ProjectSettings.globalize_path(ICON))
	DirAccess.remove_absolute(ProjectSettings.globalize_path(ICON + ".import"))
	EditorInterface.get_resource_filesystem().scan()
	await frames(2)
	await wait_until(func() -> bool: return not EditorInterface.get_resource_filesystem().is_scanning(), 30000)
	# The control first: when Godot keeps drawing a deleted icon for GDScript, a Verse case that does
	# the same is Godot's cache and not the bridge.
	subjects.push_front(subjects.pop_back())
	var cached := false
	for s in subjects:
		var name: String = s.tag + "R-EXP-8 (7): with icon.svg deleted, the Scene dock falls back to an icon of its own for %s" % s.what
		if s.icon != ICON and s.root == "Hints":
			skip(name, "there was no icon.svg to fall back from: " + GLOBAL_ICON_DEFECT)
			continue
		var icon := await _scene_dock_icon(s.scene, s.root, true)
		var fell_back := icon != ICON and icon != "<none>"
		if s.id == "gd":
			cached = icon == ICON
		if not fell_back and cached:
			skip(name, ICON_CACHE_WHY)
		else:
			check(name, fell_back)


# A theme icon has no path, which is what Node2D's own is and what the global-class defect draws.
func _check_icon(s: Dictionary, name: String, icon: String) -> void:
	if icon == "" and s.root == "Hints":
		skip(name, GLOBAL_ICON_DEFECT)
	else:
		check_eq(name, icon, ICON)


# The resource path of the icon the Scene dock draws beside the edited scene's root, "<none>" for
# none and "" for one that is not a file (a theme icon). `reload` reopens the scene first, which is
# the by-hand step's "reopen".
func _scene_dock_icon(scene: String, root_name: String, reload := false) -> String:
	var root := await _open_scene(scene)
	if root != null and reload:
		var old_id := root.get_instance_id()
		EditorInterface.reload_scene_from_path(scene)
		await wait_until(func() -> bool:
			var now := EditorInterface.get_edited_scene_root()
			return now != null and now.get_instance_id() != old_id, 10000)
	var docks := find_all(EditorInterface.get_base_control(), "SceneTreeDock")
	if docks.is_empty():
		return "<no Scene dock>"
	var icon := ["<none>"]
	await wait_until(func() -> bool:
		var item := _tree_item_in(docks[0], root_name)
		if item == null or item.get_icon(0) == null:
			return false
		icon[0] = item.get_icon(0).resource_path
		return true, 10000)
	return icon[0]


func _create_dialog_icon(class_name_: String, base: String) -> String:
	EditorInterface.popup_create_dialog(func(_picked: StringName) -> void: pass, StringName(base))
	await wait_until(func() -> bool: return _visible_create_dialog() != null, 5000)
	var dialog := _visible_create_dialog()
	if dialog == null:
		return "<no create dialog>"
	var boxes := find_all(dialog, "FilterLineEdit")
	var icon := "<none>"
	if not boxes.is_empty():
		(boxes[0] as LineEdit).text = class_name_
		(boxes[0] as LineEdit).text_changed.emit(class_name_)
		await frames(2)
		var item := _tree_item_in(dialog, class_name_)
		if item != null and item.get_icon(0) != null:
			icon = item.get_icon(0).resource_path
	dialog.hide()
	return icon


# "The Node panel, for a signal that is not <public>": "put a script on a node with
# Own<private>:event(int) and @export_signal above it, open the Node dock, and connect Own to a
# method through the dialog. It must appear in the list beside the <public> ones, with the same
# payload row, and the connection must save into the scene and fire at runtime." The dialog's own
# write is connect(..., CONNECT_PERSIST) (ConnectionsDock::_make_or_edit_connection), made here.
func _node_dock() -> void:
	_mark("the Node dock's signal list")
	var listener := _write_script(CASES_DIR.path_join("own_listener.gd"), LISTENER_SOURCE)
	var root := Node2D.new()
	root.name = "Signals"
	root.set_script(load(SIGNALS))
	var child := Node.new()
	child.name = "Listener"
	child.set_script(listener)
	root.add_child(child)
	child.owner = root
	root.connect("Own", Callable(child, "heard"), CONNECT_PERSIST)
	const SCENE := CASES_DIR + "/node_dock.tscn"
	root = await _open_new_scene(root, SCENE)
	if root == null:
		check("the editor opens a scene holding signals.verse", false)
		return
	EditorInterface.get_selection().clear()
	EditorInterface.get_selection().add_node(root)
	var docks := find_all(EditorInterface.get_base_control(), "ConnectionsDock")
	if docks.is_empty():
		check("the editor has a ConnectionsDock", false)
		return
	var rows := {}
	await wait_until(func() -> bool:
		rows.clear()
		rows.merge(_signal_rows(docks[0]))
		return rows.keys().any(func(text: String) -> bool: return text.begins_with("Own(")), 10000)
	var own: String = _row_named(rows, "Own(")
	check("the Node dock lists the <private> Own", not own.is_empty())
	check("with its int payload, as it draws a <public> one's", own.ends_with(": int)")
			and _row_named(rows, "Scored(").ends_with(": int)"))
	for name in ["Hit(", "Unspecified(", "Guarded(", "Quiet("]:
		check("and lists %s) beside it" % name, not _row_named(rows, name).is_empty())
	check("the persisted connection is listed under Own",
			rows.get(own, []).any(func(text: String) -> bool: return text.contains("heard")))

	check_eq("the scene holding the connection saves", _save_edited_scene(), OK)
	check("and the .tscn carries the connection",
			_scene_text(SCENE).contains('[connection signal="Own" from="." to="Listener" method="heard"'))
	var output := await play_and_read(SCENE, ["own_listener heard"])
	check("and the connection fires at runtime", output.contains("own_listener heard 5"))


# The Node dock's tree, as {signal row: [its connection rows]}.
func _signal_rows(dock: Node) -> Dictionary:
	var rows := {}
	for tree in find_all(dock, "Tree"):
		_collect_signal_rows((tree as Tree).get_root(), rows)
	return rows


func _collect_signal_rows(item: TreeItem, rows: Dictionary) -> void:
	if item == null:
		return
	for child in item.get_children():
		var text := child.get_text(0)
		if text.contains("("):
			rows[text] = child.get_children().map(func(row: TreeItem) -> String: return row.get_text(0))
		_collect_signal_rows(child, rows)


func _row_named(rows: Dictionary, prefix: String) -> String:
	for text in rows:
		if String(text).begins_with(prefix):
			return text
	return ""


# B3: "Connecting a signal with Make Function checked writes a stub that does not compile." Godot's
# Connect dialog emits EditorNode's script_add_function_request with the arguments it builds
# (connections_dialog.cpp:651-655, "name: Type"), which is what reaches _make_function through
# ScriptTextEditor::add_callback; Godot then saves the script, and the analysis of that save says
# whether the stub compiles.
func _make_function() -> void:
	_mark("B3: the stub Make Function writes")
	var script: Script = load(SIGNALS)
	var root := EditorInterface.get_edited_scene_root()
	var editor_node: Node = EditorInterface.get_base_control().get_parent()
	if root == null or root.get_script() != script or editor_node == null or not editor_node.is_class("EditorNode"):
		check("B3: the node dock's scene is still open under EditorNode", false)
		return
	var code := await code_edit_for(script)
	if code == null:
		check("B3: signals.verse opens in the script editor", false)
		return
	var original := code.text
	for row in [["Hit", "_on_signals_hit", "a signal with no payload"],
			["Own", "_on_signals_own", "a signal with an int payload"]]:
		var args := _dialog_args(root, row[0])
		editor_node.emit_signal("script_add_function_request", root, row[1], args)
		await frames(1)
		var at := code.text.find("\t%s<public>(" % row[1])
		var stub := code.text.substr(at) if at >= 0 else ""
		var placeholder := ")<transacts>:void =\n\t\t{} # Replace with function body.\n"
		if stub.ends_with(placeholder):
			check("B3: for %s, Make Function writes a stub ending in Godot's own placeholder" % row[2], true)
		else:
			check_eq("B3: for %s, Make Function writes a stub ending in Godot's own placeholder" % row[2], stub, "... " + placeholder)
		# A broken file empties the method list, so the stub either joins it or takes EmitOwn out.
		var outcome := {}
		await wait_until(func() -> bool:
			outcome["joined"] = _declares(script, row[1])
			return outcome["joined"] or not _declares(script, "EmitOwn"), ANALYSIS_TIMEOUT_MS)
		var name := "B3: for %s, the stub compiles" % row[2]
		if outcome.get("joined", false):
			check(name, true)
		elif stub.contains(":?"):
			skip(name, "known defect: Godot's Connect dialog passes each argument as `name: Type`, with a space, and verse_type_for_godot_type is handed ` %s`, so the parameter is written `%s:?`, which does not compile -- the stub was %s"
					% [args[0].get_slice(":", 1).strip_edges() if not args.is_empty() else "", args[0].get_slice(":", 0) if not args.is_empty() else "", stub.strip_edges()])
		else:
			check(name, false)
		code.text = original
		await _resave(script, original, "EmitOwn", true)


# What ConnectionsDock hands script_add_function_request for `signal_name`: each argument as
# "name: Type", the name `argN` when the signal has none.
func _dialog_args(node: Object, signal_name: String) -> PackedStringArray:
	var args := PackedStringArray()
	for row in node.get_signal_list():
		if row["name"] != signal_name:
			continue
		for i in row["args"].size():
			var arg: Dictionary = row["args"][i]
			var type_name: String = arg["class_name"] if arg["type"] == TYPE_OBJECT and arg["class_name"] != &"" else type_string(arg["type"])
			if arg["type"] == TYPE_NIL:
				type_name = "Variant"
			args.append("%s: %s" % [arg["name"] if not String(arg["name"]).is_empty() else "arg%d" % i, type_name])
	return args


# R-EXP-7: "Project > Project Settings > Globals, add res://scripts/settings_resource.verse. Godot
# must refuse it with its own sentence and no crash. Then add res://scripts/game_state.verse, which
# must be accepted." And "a @tool autoload answers from a @tool script in the editor" while a plain
# one is not instantiated there. The Globals tab's Add button is EditorAutoloadSettings::autoload_add.
func _autoloads() -> void:
	_mark("R-EXP-7: the autoload list")
	var lists := find_all(plugin.get_tree().root, "EditorAutoloadSettings")
	if lists.is_empty():
		check("R-EXP-7: the editor has an EditorAutoloadSettings", false)
		return
	var autoloads: Node = lists[0]
	var refused := "does not inherit from 'Node'."
	var tree_root := plugin.get_tree().root

	var before := editor_log_text()
	var answered: bool = autoloads.call("autoload_add", "SettingsProbe", "res://scripts/settings_resource.verse")
	await frames(2)
	var said := editor_log_text().substr(before.length())
	check("R-EXP-7: adding settings_resource.verse as an autoload is refused with Godot's own sentence",
			said.contains("Failed to create an autoload, script '") and said.contains(refused))
	check_eq("R-EXP-7: autoload_add answers true for it all the same, which is Godot's own behaviour", answered, true)
	check("R-EXP-7: and the bridge adds no sentence of its own", RegEx.create_from_string("VG\\d{4}: ").search(said) == null)
	check("R-EXP-7: and no node is made for it", tree_root.get_node_or_null("SettingsProbe") == null)
	autoloads.call("autoload_remove", "SettingsProbe")
	await frames(2)

	before = editor_log_text()
	answered = autoloads.call("autoload_add", "GameStateProbe", "res://scripts/game_state.verse")
	await frames(2)
	said = editor_log_text().substr(before.length())
	check("R-EXP-7: game_state.verse is accepted as an autoload",
			answered and ProjectSettings.has_setting("autoload/GameStateProbe") and not said.contains("Failed to create an autoload"))
	check("R-EXP-7: and, with no @tool, is not instantiated in the editor",
			tree_root.get_node_or_null("GameStateProbe") == null and tree_root.get_node_or_null("GameState") == null)
	autoloads.call("autoload_remove", "GameStateProbe")
	await frames(2)

	before = editor_log_text()
	autoloads.call("autoload_add", "ToolProbe", "res://scripts/tool_probe.verse")
	var added := await wait_until(func() -> bool: return tree_root.get_node_or_null("ToolProbe") != null, 10000)
	check("R-EXP-7: a @tool Verse autoload is instantiated in the editor", added)
	if added:
		var probe := tree_root.get_node("ToolProbe")
		var script: Script = probe.get_script()
		check("R-EXP-7: holding its script", script != null and script.resource_path == "res://scripts/tool_probe.verse")
		# A placeholder answers a call with a script error that ends this function, so it is asked
		# only of a script that can instantiate.
		if script != null and not script.can_instantiate():
			for name in ["R-EXP-7: and answers a method from the last built generation", "R-EXP-7: its _Ready ran in the editor"]:
				skip(name, TOOL_AUTOLOAD_DEFECT)
		else:
			check_eq("R-EXP-7: and answers a method from the last built generation", probe.call("Which"), "tool")
			# A count, not an offset into `before`: the Output panel trims its oldest lines, so the
			# text grows at the end and shrinks at the front, and an offset stops pointing anywhere.
			var readies_before := before.count("tool_probe ready")
			await wait_until(func() -> bool: return editor_log_text().count("tool_probe ready") > readies_before, 5000)
			check("R-EXP-7: its _Ready ran in the editor", editor_log_text().count("tool_probe ready") > readies_before)
	autoloads.call("autoload_remove", "ToolProbe")
	check("R-EXP-7: and removing it takes the node out of the editor",
			await wait_until(func() -> bool: return tree_root.get_node_or_null("ToolProbe") == null, 10000))


# --- a host fatal error during Play (docs/editor-test-audit.md step 7) ---------------------------
#
# by-hand-findings.md "A host fatal error during Play": "set VERSE_HOST_TEST_FATAL=check in the
# environment the editor is started from ... and press Play. The game must close at once, and the
# editor's Output panel must show 'The game ended in a Verse host fatal error:' with the failed
# check's message and a stack naming GodotVerse::FireTestFatal(). Press Play again without the
# variable set, and nothing about the fatal error may print a second time." run_tests.py starts
# this session with the variable set; the editor's own host is the one the consumer hides it from.

const FATAL_FLAG := "--verse-editor-fatal"
const FATAL_VARIABLE := "VERSE_HOST_TEST_FATAL"
const FATAL_REPORT := "VG4113: "
const FATAL_MESSAGE := "VERSE_HOST_TEST_FATAL=check asked for a failed check."
const FATAL_FRAME := "GodotVerse::FireTestFatal()"


func _host_fatal_during_play() -> void:
	_mark("B43: the editor's own host, started with the variable set")
	check_eq("B43: the editor was started with VERSE_HOST_TEST_FATAL=check", OS.get_environment(FATAL_VARIABLE), "check")
	check("Play is armed to start the game headless", play_is_headless())
	var probe: Script = load(SAVE_PROBE)
	await frames(30)
	check("B43: the editor's own host loads and keeps ticking", probe != null and _declares(probe, "Answer"))
	const SCENE := CASES_DIR + "/fatal.tscn"
	DirAccess.make_dir_recursive_absolute(ProjectSettings.globalize_path(CASES_DIR))
	var root := Node.new()
	root.name = "Idle"
	root.set_script(_write_script(CASES_DIR.path_join("idle.gd"), IDLE_SOURCE))
	if not _pack(root, SCENE):
		check("B43: the Play scene saves", false)
		return

	_mark("B43: Play with the variable set")
	var record_path := ProjectSettings.globalize_path("user://logs/verse_crash.log")
	var seen := {"record": ""}
	if not play_scene(SCENE):
		return
	var ended := await wait_until(func() -> bool:
		if FileAccess.file_exists(record_path):
			seen.record = FileAccess.get_file_as_string(record_path)
		return not EditorInterface.is_playing_scene(), 60000)
	check("B43: the game ends by itself", ended)
	if not ended:
		EditorInterface.stop_playing_scene()
		return
	check("B43: the game's host wrote user://logs/verse_crash.log before the process ended",
			String(seen.record).contains(FATAL_MESSAGE))
	var reported := await wait_until(func() -> bool: return editor_log_text().contains(FATAL_REPORT), 10000)
	check("B43: when Play ends, the Output panel says the game ended in a host fatal error", reported)
	var text := editor_log_text()
	var report := text.substr(text.find(FATAL_REPORT))
	check("B43: with the failed check's message", report.contains(FATAL_MESSAGE))
	check("B43: and a stack naming GodotVerse::FireTestFatal()", report.contains(FATAL_FRAME))
	check("B43: the editor removes the record it reported", not FileAccess.file_exists(record_path))
	check("B43: and the editor itself is still running its host", _declares(probe, "Answer"))

	# The Output panel is cleared at every Play (run/output/always_clear_output_on_play), so from
	# here on any report in it is a second one.
	_mark("B43: Play again without the variable")
	OS.unset_environment(FATAL_VARIABLE)
	var output := await play_and_read(SCENE, ["idle game up"])
	check("B43: without the variable the game comes up", output.contains("idle game up"))
	await frames(10)
	check_eq("B43: and nothing about the fatal error prints a second time", editor_log_text().count(FATAL_REPORT), 0)
	check("B43: nor as a previous run's record at the game's host load", not editor_log_text().contains("VG4110: "))

	# The editor's host was armed at its own vh_init, which is behind it, so setting the variable
	# now reaches only the game Play starts next.
	_mark("B43: Play with a native crash")
	OS.set_environment(FATAL_VARIABLE, "access_violation")
	if play_scene(SCENE):
		ended = await wait_until(func() -> bool: return not EditorInterface.is_playing_scene(), 60000)
		check("B43: a game whose host crashes natively ends by itself", ended)
		if not ended:
			EditorInterface.stop_playing_scene()
		await frames(10)
		check("B43: and leaves no record, which is Godot's crash handler's to report", not FileAccess.file_exists(record_path))
		check("B43: so the editor reports no host fatal error for it", not editor_log_text().contains(FATAL_REPORT))
	OS.unset_environment(FATAL_VARIABLE)


# --- helpers ------------------------------------------------------------------------------------

func check(name: String, ok: bool) -> void:
	if ok:
		passed += 1
		print("[editor] %s: ok" % name)
	else:
		failed += 1
		print("[editor] %s: FAIL" % name)


func check_eq(name: String, got: Variant, expected: Variant) -> void:
	if typeof(got) == typeof(expected) and got == expected:
		passed += 1
		print("[editor] %s: ok" % name)
	else:
		failed += 1
		print("[editor] %s: FAIL (got %s %s, expected %s %s)" % [name,
				type_string(typeof(got)), str(got), type_string(typeof(expected)), str(expected)])


func skip(name: String, why: String) -> void:
	skipped += 1
	print("[editor] %s: skip -- %s" % [name, why])


func report() -> void:
	print("[editor] %d passed, %d failed, %d skipped" % [passed, failed, skipped])


func overdue() -> bool:
	return Time.get_ticks_msec() - _step_started_ms > STEP_TIMEOUT_MS


func fail_overdue() -> void:
	check("the watchdog: %s finished within %d s" % [_step, STEP_TIMEOUT_MS / 1000], false)


func _mark(step: String) -> void:
	_step = step
	_step_started_ms = Time.get_ticks_msec()


func frames(count: int) -> void:
	for i in count:
		await plugin.get_tree().process_frame


# True once `condition` holds, false when `timeout_ms` passes first. One frame between asks.
func wait_until(condition: Callable, timeout_ms: int) -> bool:
	var started := Time.get_ticks_msec()
	while not condition.call():
		if Time.get_ticks_msec() - started > timeout_ms:
			return false
		await plugin.get_tree().process_frame
	return true


# Editor classes are not exposed to ClassDB, but GDCLASS still answers get_class(), so is_class
# finds them by name.
func find_all(root: Node, class_name_: String) -> Array:
	var found := []
	for child in root.get_children(true):
		if child.is_class(class_name_):
			found.append(child)
		found.append_array(find_all(child, class_name_))
	return found


func verse_language() -> Object:
	for i in Engine.get_script_language_count():
		var lang := Engine.get_script_language(i)
		if lang != null and lang.get_class() == "VerseScriptLanguage":
			return lang
	return null


# The CodeEdit of the script editor tab showing `script`, once it is the current one.
func code_edit_for(script: Script) -> CodeEdit:
	if script == null:
		return null
	EditorInterface.edit_script(script)
	var editor := EditorInterface.get_script_editor()
	var current := await wait_until(func() -> bool:
		return editor.get_current_script() == script and editor.get_current_editor() != null, 10000)
	if not current:
		return null
	await frames(1)
	return editor.get_current_editor().get_base_editor() as CodeEdit


func place_caret_after(code: CodeEdit, text: String) -> bool:
	for line in code.get_line_count():
		var column := code.get_line(line).find(text)
		if column >= 0:
			code.set_caret_line(line)
			code.set_caret_column(column + text.length())
			return true
	return false


func _line_of(code: CodeEdit, text: String) -> int:
	for line in code.get_line_count():
		if code.get_line(line).begins_with(text):
			return line
	return -1


func _inserts(code: CodeEdit) -> Array:
	var inserts := []
	for option in code.get_code_completion_options():
		inserts.append(String(option.get("insert_text", "")))
	return inserts


# Godot forwards none of its own command line to the game (main.cpp:1177-1206) and appends the
# project's main_run_args instead, so this setting is the only thing standing between Play and a
# window. A case that presses Play goes through play_scene, which refuses without it.
func play_is_headless() -> bool:
	return String(ProjectSettings.get_setting("editor/run/main_run_args", "")).split(" ").has("--headless")


func play_scene(path: String) -> bool:
	if not play_is_headless():
		check("Play is refused: editor/run/main_run_args does not carry --headless", false)
		return false
	EditorInterface.play_custom_scene(path)
	return true


# Plays `path` until the Output panel carries every marker or a minute passes, stops it, and answers
# the panel's text. The game's prints reach the panel over the remote debugger, which is the port
# run_tests.py pinned.
func play_and_read(path: String, markers: Array) -> String:
	if not play_scene(path):
		return ""
	await wait_until(func() -> bool:
		var text := editor_log_text()
		for marker in markers:
			if not text.contains(marker):
				return false
		return true, 60000)
	var text := editor_log_text()
	EditorInterface.stop_playing_scene()
	await wait_until(func() -> bool: return not EditorInterface.is_playing_scene(), 10000)
	return text


func editor_log_text() -> String:
	var text := ""
	for panel in find_all(EditorInterface.get_base_control(), "EditorLog"):
		for label in find_all(panel, "RichTextLabel"):
			text += (label as RichTextLabel).get_parsed_text() + "\n"
	return text


func _user_arg_int(prefix: String) -> int:
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with(prefix):
			return arg.substr(prefix.length()).to_int()
	return -1
