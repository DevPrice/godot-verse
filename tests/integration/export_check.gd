extends Node

# The same cases, run from inside an exported game.
#
#     integration.exe --headless --fixed-fps 60 -- --verse-check
#
# An autoload rather than a `-s` script, because `--script` is inside TOOLS_ENABLED
# (main.cpp:4116-4120) and an export template has no such thing: the only code an exported game runs
# that its scenes did not ask for is an autoload. `--verse-check` comes after `--`, which is what
# OS.get_cmdline_user_args() answers, so the flag cannot collide with one of Godot's.
#
# Without the flag this does nothing at all, which is the point: the autoload ships in every export,
# including one a person plays.

const Cases = preload("res://test_cases.gd")

var _cases: Cases


func _ready() -> void:
	# Declaring _process is what enables it, so it is already on and running against a null `_cases`
	# -- including in the editor-side run, where this autoload is loaded and does nothing else.
	set_process(false)
	if not OS.get_cmdline_user_args().has("--verse-check"):
		return
	# Deferred, because an autoload's _ready runs while the root is still adding the main scene:
	# add_child() there is "Parent node is busy setting up children", and these cases do nothing but
	# add children. Measured on dodge-the-creeps' first exported run.
	_begin.call_deferred()


func _begin() -> void:
	_cases = Cases.new()
	_cases.tree = get_tree()
	# The compiler-side entry points are refused in an exported game and res:// is a read-only pack.
	_cases.editor = false
	_cases.begin()
	if _cases.fatal:
		_finish()
		return
	set_process(true)


func _process(_delta: float) -> void:
	if _cases.fatal or _cases.step():
		_finish()


func _finish() -> void:
	print("[integration] %d passed, %d failed, %d skipped"
		% [_cases._passed, _cases._failed, _cases._skipped])
	get_tree().quit(1 if _cases._failed > 0 or _cases.fatal else 0)
