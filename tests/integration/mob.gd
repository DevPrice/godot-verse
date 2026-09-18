# R-INT-7: a GDScript class with a `class_name`, which is what a generated binding is *for*.
#
# Nothing in this file knows about Verse. That is the requirement: the binding is generated from
# what Godot already reports about this class -- `ProjectSettings.get_global_class_list()` names it,
# and `get_script_method_list()` describes it -- so an author writes ordinary GDScript and gets a
# declared type in Verse for free.
#
# The types are annotated deliberately. An unannotated GDScript declaration reports `NIL` with
# `PROPERTY_USAGE_NIL_IS_VARIANT`, which the generator cannot turn into a Verse type and therefore
# leaves out; these have to be typed or the binding is empty and the test proves nothing.
class_name Mob
extends RigidBody2D


func hit(power: int) -> int:
	return power * 2


func label() -> String:
	return "mob"


func heavy() -> bool:
	return true
