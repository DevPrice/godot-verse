extends SceneTree

# The editor-side driver for `test_cases.gd`, run by tools/run_tests.py:
#
#     godot --headless --path tests/integration --script res://test_main.gd
#
# Everything it used to hold is in the library now, so that the exported run and this one cannot
# disagree about what passing means. `export_check.gd` is the other driver.

const Cases = preload("res://test_cases.gd")

var _cases: Cases


func _init() -> void:
	_cases = Cases.new()
	_cases.tree = self
	_cases.begin()
	if _cases.fatal:
		_report()
		quit(1)


func _process(_delta: float) -> bool:
	if _cases.fatal or _cases.step():
		_report()
		quit(1 if _cases._failed > 0 or _cases.fatal else 0)
		return true
	return false


func _report() -> void:
	print("[integration] %d passed, %d failed, %d skipped"
		% [_cases._passed, _cases._failed, _cases._skipped])
