class_name CycleProbe
extends Node2D

# The script the cycle runs through, and it takes both halves to be one.
#
# `class_name` puts it in the global class list, which is what makes the binding generator ask for
# it. The annotated `Widget` is what makes Godot resolve a Verse class while loading it, which is
# what puts the generator on this file's own load stack. Either alone is harmless.
#
# Nothing here knows about Verse beyond that one type name, which is the point: this is what an
# author writes when they want a Verse node in the inspector.

@export var widget: Widget


# Named without a leading underscore, so the binding actually carries it -- a Godot virtual is
# skipped by the generator and would leave `cycle_probe` an empty class, which is legal and would
# prove nothing. `scripts/caller.verse` names this member, so a generation that could not describe
# this script fails the build at that call.
func doubled(n: int) -> int:
	return n * 2
