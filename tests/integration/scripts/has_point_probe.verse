using { /Godot.org/Godot }

# `_HasPoint`, which decides whether a click is this control's at all.
#
# It was owed by hand alongside `_CanDropData`, both recorded in `phase-4-gaps.md` G19 as
# having no public caller and so no way for a headless run to make the engine ask. The first half
# is true and the conclusion was wrong: nothing has to *call* `_HasPoint`, because the engine asks
# it on its own as soon as a click exists, and `Input.parse_input_event` manufactures one under
# `--headless`. `_CanDropData` really is unreachable and is owed by hand still
# (`by-hand-findings.md` B10 and B11).
#
# What makes this a test of the virtual rather than of the plumbing is that both answers are
# exercised in one run: `false` must send the click past this control, and `true` must keep it. A
# virtual that never ran answers the generated default, `false`, and would pass half of that.
@global_class
has_point_probe := class(control):

	# What `_HasPoint` answers. Written from the test between the two clicks.
	var Solid<public>:logic = false

	# Clicks the engine decided belonged to this control.
	var Clicks<public>:int = 0

	_HasPoint<override>(Point:vector2):logic = Solid

	# `IsPressed()?`, not `IsPressed()`. The cast already makes the clause list failable, so a bare
	# `logic` beside it is accepted -- and then evaluated and thrown away, so the body runs on the
	# release as well as the press. Measured, not guessed (`by-hand-findings.md` B12).
	_GuiInput<override>(Event:input_event):void =
		if (Button := input_event_mouse_button[Event], Button.IsPressed()?):
			set Clicks = Clicks + 1
