@tool
extends EditorPlugin

# The editor layer's driver (docs/editor-test-audit.md step 2): `tools/run_tests.py --only editor`
# copies tests/integration, enables this plugin in the copy and opens it with
# `godot --headless --editor -- --verse-editor-cases`. The cases are editor_cases.gd's.
#
# Inert unless both hold, so a copy opened in a windowed editor -- or this project opened by hand --
# runs nothing and quits nothing: the display server is the headless one, and the flag is on the
# command line.

const Cases = preload("res://addons/verse_editor_cases/editor_cases.gd")
const FLAG := "--verse-editor-cases"

var _cases: Cases
var _done := false


func _enter_tree() -> void:
	# Declaring _process is what turns it on, so it has to be turned off before the guard.
	set_process(false)
	if DisplayServer.get_name() != "headless" or not OS.get_cmdline_user_args().has(FLAG):
		return
	_cases = Cases.new()
	_cases.plugin = self
	set_process(true)
	_cases.run()


func _process(_delta: float) -> void:
	if _done:
		return
	if not _cases.finished and not _cases.overdue():
		return
	_done = true
	if not _cases.finished:
		_cases.fail_overdue()
	_cases.report()
	get_tree().quit(0 if _cases.failed == 0 else 1)
