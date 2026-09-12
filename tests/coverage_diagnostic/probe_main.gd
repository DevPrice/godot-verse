extends SceneTree

# Loads the one deliberately broken script and quits. Loading it is what compiles the project, and
# compiling it is what prints the diagnostics tools/run_tests.py then asserts on -- the assertions
# live there rather than here because ScriptLanguage exposes nothing a script can ask: `_validate` is
# an extension virtual with no bound counterpart, so the only way to read what the author would see
# is to read what the editor prints.


func _init() -> void:
	var script: Script = load("res://scripts/probe.verse")
	print("[coverage] loaded: %s" % ("yes" if script != null else "no"))
	print("[coverage] done")
	quit(0)
