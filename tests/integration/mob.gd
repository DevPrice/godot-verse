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

# R-INT-9's other four. A constant and an enum are data on a type, which Verse has no spelling for,
# so both land in the class's `MobStatics` module; a static is dispatched without an object and goes
# there too. A `var` is the one that becomes an ordinary member: `set M.Speed = 3.0`.
const LIMIT := 7
const TITLE := "mob"

enum State { IDLE, BUSY, DONE }

# Deliberately not `mass`, `scale` or any other name RigidBody2D already has: a member that shadows
# an inherited mirrored one is glitch 3532 at the generated declaration, and the generator drops
# such a name rather than emitting a package that cannot compile.
var speed: float = 1.5
var tag: String = "m"
var mode: State = State.IDLE
# An inline accessor compiles to a method named `@armor_setter`, which a binding emitting it
# spelled as an attribute and so refused every Verse script in the project.
var armor: int = 0:
	set(value):
		armor = value
# A struct-typed member needs field-named accessor overloads, without which the package is refused.
var heading: Vector2 = Vector2.ZERO


static func spawn_cost(n: int) -> int:
	return n * LIMIT


func hit(power: int) -> int:
	return power * 2


func shift(to: State) -> State:
	return to


func label() -> String:
	return "mob"


func heavy() -> bool:
	return true


# An object, in both directions and at both kinds of class: `Mob` is this script's own, which no
# mirror carries and only the binding beside it declares, and `Node2D` is one the mirror has. A
# method whose signature the generator cannot type is left out of the binding entirely, so these
# four are what say it can type an object at all.
func mate(other: Mob) -> Mob:
	return other


func place(where: Node2D) -> Node2D:
	return where
