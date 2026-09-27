extends Node

# The GDScript control beside scripts/debug_play.verse in debug_play.tscn: the same shape, so a
# debugger case that fails for Verse beside one that passes here is the bridge's, and one that fails
# for both is Godot's. The cases find its lines by their text.


func _ready() -> void:
	var first := 10
	var second := work()
	print("debug_control ready: ", first + second)


func work() -> int:
	var inner := 21
	return inner


func _process(_delta: float) -> void:
	work()
