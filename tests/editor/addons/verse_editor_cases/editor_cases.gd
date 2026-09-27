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
	await _harness()
	await _placeholders_and_saving()
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
