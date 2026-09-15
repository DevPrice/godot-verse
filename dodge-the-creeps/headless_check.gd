extends SceneTree

# Not part of the game. The game is five `.verse` files and four scenes, and this is the only way
# to watch them work without a window:
#
#     godot --headless --fixed-fps 60 --path dodge-the-creeps -s res://headless_check.gd
#
# --fixed-fps matters. Headless, the main loop runs as fast as it can and a Timer counts real
# seconds, so without it two seconds of StartTimer never elapse and nothing after "Get Ready"
# happens.
#
# The checks themselves are in `checks.gd`, which `export_check.gd` runs too: the same lines have
# to pass against a project the editor compiled at startup and against one the cooker built into
# an export, and two copies of them would let those drift.
#
# It is GDScript because a Verse script cannot drive the tree from outside a node, and because
# what it asserts is what *Godot* sees of the Verse scripts -- the same reason
# tests/integration/test_main.gd is GDScript. One line per case, like every other test here.
#
# It is not wired into tools/run_tests.py's first three layers: the port is a yardstick, and a
# yardstick that gates the build stops being an honest measure of how far the bridge has got. The
# export layer does run it, because there the export is what is under test.

const Checks = preload("res://checks.gd")

var checks: Checks


func _initialize() -> void:
	checks = Checks.new()
	checks.tree = self
	checks.begin()


func _process(_delta: float) -> bool:
	return checks.step()
