using { /Godot.org/Godot }

# The class body test_cases.gd types a new member into, through `probe_complete_code`, to ask for
# completion the way the script editor does (by-hand-findings.md B13). Nothing here overrides
# `_Ready` or `_Process`, so both are still candidates, and `_Draw` already is -- which is the
# one that must not be offered again.
override_complete_probe := class(node2d):

	Speed:float = 1.0

	_Draw<override>():void =
		set Visible = true
