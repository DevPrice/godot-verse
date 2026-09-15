extends Node

# The same checks, run from inside an exported game.
#
#     dodge-the-creeps.exe --headless --fixed-fps 60 -- --verse-check
#
# An autoload rather than a `-s` script, because `--script` is inside TOOLS_ENABLED
# (main.cpp:4116-4120) and an export template has no such thing: the only code an exported game
# runs that its scenes did not ask for is an autoload. `--verse-check` comes after `--`, which is
# what OS.get_cmdline_user_args() answers, so the flag cannot collide with one of Godot's.
#
# Without the flag this does nothing at all and the game plays normally, which is the point: the
# autoload ships in every export, including the one a person plays.

const Checks = preload("res://checks.gd")

var checks: Checks


func _ready() -> void:
	# Declaring _process is what enables it, so without this it is already running against a null
	# `checks` in every ordinary play of the game.
	set_process(false)
	if not OS.get_cmdline_user_args().has("--verse-check"):
		return
	# Deferred, because an autoload's _ready runs while the root is still adding the main scene:
	# remove_child() there is "Parent node is busy adding/removing children" and add_child() is
	# "Parent node is busy setting up children", so begin() silently got no scene and every check
	# after the first failed. Measured on the first exported run of this game.
	_begin.call_deferred()


func _begin() -> void:
	# The main scene is already loading; these checks build their own copy of it, so the one the
	# game opened has to go first or two rounds run at once.
	for child in get_tree().root.get_children():
		if child != self:
			get_tree().root.remove_child(child)
			child.queue_free()

	checks = Checks.new()
	checks.tree = get_tree()
	checks.begin()
	set_process(true)


func _process(_delta: float) -> void:
	if checks == null:
		return
	if checks.step():
		get_tree().quit(1 if checks.failures > 0 else 0)
