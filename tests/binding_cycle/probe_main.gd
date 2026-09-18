extends SceneTree

# Loads the one GDScript that closes the loop, and quits. Loading it is the whole test: its
# annotated `Widget` sends Godot into `widget.verse`, that load builds the project, the build
# generates a binding per `class_name` script, and the generator is asked for this very file.
#
# What the loop costs is printed rather than returned, so tools/run_tests.py is where the
# assertions are -- and they are refutations. A working run says nothing at all.


func _init() -> void:
	var script: Script = load("res://cycle_probe.gd")
	print("[cycle] loaded cycle_probe.gd: %s" % ("yes" if script != null else "no"))
	print("[cycle] done")
	quit(0)
