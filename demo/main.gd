class_name MainScript extends Node2D

@export var mover: Mover

func _ready() -> void:
	print("Greeting from GDScript: %s" % mover.Greeting)
