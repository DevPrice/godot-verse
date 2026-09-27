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


func _user_arg_int(prefix: String) -> int:
	for arg in OS.get_cmdline_user_args():
		if arg.begins_with(prefix):
			return arg.substr(prefix.length()).to_int()
	return -1
