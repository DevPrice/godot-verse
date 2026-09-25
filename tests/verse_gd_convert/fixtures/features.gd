@tool
@icon("res://icon.svg")
class_name FeatureTour
extends Node2D
## A tour of what the converter translates.

signal health_changed(new_health: int)
signal died
signal scored(points: int, bonus: float)

enum State { IDLE, RUNNING, JUMPING }

const MAX_HEALTH := 100
const GRAVITY = 9.8
const MOB = preload("res://mob.tscn")

@export var speed: float = 200
@export_range(0, 10) var level: int = 1
@export_file("*.json") var config_path := ""
@export_multiline var description := "Tour"
@export_group("Colours")
@export var tint := Color(1, 0.5, 0.25)
@export var target: Node2D
@export_enum("Small", "Large") var size_kind: int

@onready var sprite: Sprite2D = $Sprite
@onready var label := $UI/Label as Label
@onready var timer = $Timer

var state := State.IDLE
var health: int = MAX_HEALTH:
	set(value):
		health = clampi(value, 0, MAX_HEALTH)
		health_changed.emit(health)
var velocity := Vector2.ZERO
var names: Array[String] = []
var scores := {"a": 1, "b": 2}
var spare_timer := Timer.new()
var untyped


func _ready() -> void:
	super._ready()
	add_child(spare_timer)
	died.connect(_on_died)
	scored.connect(func(points, bonus): print("scored ", points, " with ", bonus))
	timer.timeout.connect(_on_timer_timeout)
	if Engine.is_editor_hint():
		return
	start_moving(3)


func _physics_process(delta: float) -> void:
	velocity.y += GRAVITY * delta
	position += velocity * delta
	if position.y > 600 and state != State.JUMPING:
		position.y = 600
		velocity.y = 0
	match state:
		State.IDLE:
			sprite.modulate = tint
		State.RUNNING, State.JUMPING:
			rotation += delta
		_:
			pass


func _input(event: InputEvent) -> void:
	if event is InputEventKey and event.pressed:
		if event.keycode == KEY_SPACE:
			jump()


func _has_point(point: Vector2) -> bool:
	return point.length() < 10.0


func jump(height = 2.5):
	if state == State.JUMPING:
		return
	state = State.JUMPING
	velocity.y = -height * 100


func start_moving(times: int) -> void:
	for i in range(times):
		names.append("step %d" % i)
	for n in names:
		print(n)
	for key in scores:
		print(key)
	var countdown := 3
	while countdown > 0:
		countdown -= 1
	var average := total_score() / 2
	var ratio := 7 % 3
	var power := 2 ** 3
	var flags := 1 | 4
	print("average: ", average, " ratio: ", ratio, " power: ", power, " flags: ", flags)


func total_score() -> int:
	var total = 0
	for key in scores:
		total += scores[key]
	return total


func describe() -> String:
	var parts := "health " + str(health) + "/" + str(MAX_HEALTH)
	return parts if health > 0 else "dead"


func take_damage(amount: int) -> void:
	health -= amount
	if health == 0:
		died.emit()
	scored.emit(amount, 1.5)


func find_target():
	if target != null:
		return target.global_position
	return Vector2.ZERO


func spawn() -> void:
	var mob = MOB.instantiate()
	add_child(mob)


func wait_a_bit() -> void:
	await get_tree().create_timer(0.5).timeout
	label.text = "done"


func _on_died() -> void:
	queue_free()


func _on_timer_timeout() -> void:
	wait_a_bit()


static func double(value: int) -> int:
	return value * 2


class Helper:
	var count := 0

	func bump() -> void:
		count += 1
