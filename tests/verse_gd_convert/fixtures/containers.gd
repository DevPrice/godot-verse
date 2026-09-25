extends Node
## Godot's Dictionary and the conversions GDScript spells as constructors.

var inventory := {}
var labels: Dictionary = {}


func add_item(item: String, count: int) -> void:
	inventory[item] = count
	labels[item] = "x%d" % count


func count_of(item: String) -> int:
	if inventory.has(item):
		return inventory.get(item, 0)
	return 0


func summary() -> String:
	var names := PackedStringArray(["a", "b"])
	var tag := StringName("items")
	return str(tag, ": ", inventory.size(), " kinds, first ", names[0])


func kind_of(value) -> bool:
	return typeof(value) == TYPE_INT
