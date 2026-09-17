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
# exercised in one run: declining must send the click past this control, and succeeding must keep
# it. A virtual that never ran answers the generated default, which also declines, and would pass
# half of that.
#
# It is also the only automated test of a `<decides>` virtual end to end -- the override declines,
# the host answers VH_ERR_FAILED, and the consumer writes `false` for Godot to read. The decorated
# name is tested here too, silently: it is what `instance_has_function` keys on, and if `<decides>`
# had changed it the override would read as unimplemented and both halves would answer the default.
@global_class
has_point_probe := class(control):

	# What `_HasPoint` answers. Written from the test between the two clicks.
	var Solid<public>:logic = false

	# Clicks the engine decided belonged to this control.
	var Clicks<public>:int = 0

	# `<decides>` alone, matching the mirror: it keeps the wide default effect set, so this body
	# could still call a specifier-less helper. `<decides><transacts>` could not -- measured in
	# `tests/verse_probe/decides_virtual_reject.verse` case 2.
	_HasPoint<override>(Point:vector2)<decides>:void = Solid?

	# `IsPressed[]`, in brackets, because a mirrored predicate is `<decides>` and there is no other
	# way to write it. That is the point of the spelling: as a `logic`-returning method this read
	# `IsPressed()` just as happily, and a bare `logic` beside a failable clause is evaluated and
	# thrown away -- so the body ran on the release as well as the press, measured rather than
	# guessed (`by-hand-findings.md` B12). The trap is still there for a `logic` that comes from
	# somewhere else; what changed is that Godot's predicates no longer hand you one.
	_GuiInput<override>(Event:input_event):void =
		if (Button := input_event_mouse_button[Event], Button.IsPressed[]):
			set Clicks = Clicks + 1
