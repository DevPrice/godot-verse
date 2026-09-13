extends Node2D

@export var mover: Mover

func _ready() -> void:
	print("Wow " + mover.Greeting)
	for method: Dictionary in mover.get_method_list():
		print(method.name)

enum MyEnum {
	X
}
