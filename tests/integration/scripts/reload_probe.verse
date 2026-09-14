using { /Godot.org/Godot }

# An instance keeps the generation it was made against for life -- the constraint in CLAUDE.md, and
# deliberate. So a source change reaches an attached node only if something *replaces* the instance,
# which is what `Script.reload()` does now and did not before (`by-hand-findings.md` B8).
#
# The test rewrites this file's `Answer` body, builds, and reloads. Nothing else edits it, and it is
# restored afterwards, so what is checked in here is the pre-edit state.
@global_class
reload_probe := class(node2d):

	# Exported on purpose. The value an inspector or a `.tscn` put here lives in the instance, and
	# re-attaching destroys that instance -- so without the carry-over in reload_instances a reload
	# would silently reset every scene-set property to its declared default.
	@export
	var Tag<public>:int = 0

	Answer<public>()<transacts>:int = 1
