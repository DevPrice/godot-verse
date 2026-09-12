extends SceneTree

# Not part of the game. The game is five `.verse` files and four scenes, and this is the only way
# to watch them work without a window:
#
#     godot --headless --fixed-fps 60 --path dodge-the-creeps -s res://headless_check.gd
#
# --fixed-fps matters. Headless, the main loop runs as fast as it can and a Timer counts real
# seconds, so without it two seconds of StartTimer never elapse and nothing after "Get Ready"
# happens.
#
# It is GDScript because a Verse script cannot drive the tree from outside a node, and because
# what it asserts is what *Godot* sees of the Verse scripts -- the same reason
# tests/integration/test_main.gd is GDScript. One line per case, like every other test here.
#
# It is not wired into tools/run_tests.py: the port is a yardstick, and a yardstick that gates the
# build stops being an honest measure of how far the bridge has got.

var main: Node
var player: Node
var hud: Node
var frame := 0
var mobs_seen := 0
var moved_x := 0.0
var failures := 0

func check(name: String, ok: bool, detail := "") -> void:
	print(("ok   " if ok else "FAIL ") + name + ("" if detail == "" else "  -- " + detail))
	if not ok:
		failures += 1

func _initialize() -> void:
	main = load("res://main.tscn").instantiate()
	root.add_child(main)
	player = main.get_node("Player")
	hud = main.get_node("HUD")
	check("main.tscn instantiates with a Verse script", main.get_script() != null)
	check("an export slot typed as another script's class resolves",
		main.get("Player") == player and main.get("Hud") == hud)
	check("an export slot typed as a Resource resolves", main.get("MobScene") != null)
	check("a Verse method is visible to Godot", main.has_method("NewGame"))

func _process(_delta: float) -> bool:
	frame += 1
	match frame:
		1:
			# Late by one frame here, and so is a GDScript _ready: a MainLoop script adds its
			# children before the tree's own root is inside the tree.
			check("the player's Ready ran and hid it", not player.visible)
		5:
			hud.get_node("StartButton").pressed.emit()
			check("the button's pressed signal reached the HUD's Verse handler",
				not hud.get_node("StartButton").visible)
			check("the same signal reached main's Verse handler, which started the round",
				hud.get_node("MessageLabel").text == "Get Ready"
				and hud.get_node("ScoreLabel").text == "0",
				hud.get_node("MessageLabel").text)
			check("main called the player script's own Start method",
				player.visible and player.position == Vector2(240, 450), str(player.position))
			moved_x = player.position.x
			Input.action_press("move_right")
		40:
			check("Process reads Input and moves the player", player.position.x > moved_x + 50.0,
				"x = " + str(player.position.x))
			check("the walk animation plays", player.get_node("AnimatedSprite2D").is_playing())
			check("the sprite faces right",
				player.get_node("AnimatedSprite2D").animation == StringName("right")
				and not player.get_node("AnimatedSprite2D").flip_h)
			Input.action_release("move_right")
			Input.action_press("move_left")
		70:
			check("the sprite flips when moving left", player.get_node("AnimatedSprite2D").flip_h)
			Input.action_release("move_left")
		90:
			check("the animation stops when the player is still",
				not player.get_node("AnimatedSprite2D").is_playing())
			check("get_viewport_rect clamped the player inside the window",
				player.position.x >= 0.0 and player.position.x <= 480.0, str(player.position))
		200:
			check("StartTimer's timeout reached a Verse method",
				not (main.get_node("MobTimer") as Timer).is_stopped())
			check("ScoreTimer's timeout reached a Verse method",
				int(hud.get_node("ScoreLabel").text) > 0, hud.get_node("ScoreLabel").text)
			check("mobs spawn", get_nodes_in_group("mobs").size() > 0)
			var mob = get_nodes_in_group("mobs").front()
			if mob != null:
				check("Object.set placed the mob on the spawn path",
					mob.position != Vector2.ZERO, str(mob.position))
				check("Object.set gave the mob a velocity",
					mob.linear_velocity.length() > 100.0, str(mob.linear_velocity))
				check("Object.set rotated the mob", mob.rotation != 0.0, str(mob.rotation))
				check("mob.Ready picked one of the three animations",
					mob.get_node("AnimatedSprite2D").animation in
						[StringName("walk"), StringName("swim"), StringName("fly")],
					str(mob.get_node("AnimatedSprite2D").animation))
			mobs_seen = get_nodes_in_group("mobs").size()
		202:
			# Emitted rather than waited for: VisibleOnScreenNotifier2D wants a drawn viewport, and
			# what is under test is the Verse handler, not Godot's visibility system.
			get_nodes_in_group("mobs").front().get_node("VisibleOnScreenNotifier2D").screen_exited.emit()
		204:
			check("screen_exited reached mob's Verse handler, which freed it",
				get_nodes_in_group("mobs").size() == mobs_seen - 1,
				str(get_nodes_in_group("mobs").size()) + " of " + str(mobs_seen))
		205:
			player.body_entered.emit(get_nodes_in_group("mobs").front())
		207:
			check("body_entered reached the player's Verse handler", not player.visible)
			check("body_entered reached main's Verse handler too, which stopped the round",
				(main.get_node("ScoreTimer") as Timer).is_stopped()
				and (main.get_node("MobTimer") as Timer).is_stopped())
			check("the game-over message shows",
				hud.get_node("MessageLabel").text == "Game Over",
				hud.get_node("MessageLabel").text)
		275:
			check("MessageTimer advanced the HUD's state machine to the title",
				hud.get_node("MessageLabel").text == "Dodge the\nCreeps",
				hud.get_node("MessageLabel").text)
		345:
			check("TitleTimer showed the Start button again",
				hud.get_node("StartButton").visible)
			mobs_seen = get_nodes_in_group("mobs").size()
			hud.get_node("StartButton").pressed.emit()
		348:
			check("a second round walks the mobs group and frees them",
				mobs_seen > 0 and get_nodes_in_group("mobs").is_empty(),
				str(mobs_seen) + " cleared to " + str(get_nodes_in_group("mobs").size()))
			check("the player is back at the start position", player.visible
				and player.position == Vector2(240, 450), str(player.position))
			print("headless_check: " + ("all checks passed" if failures == 0
				else str(failures) + " checks FAILED"))
	return frame >= 349
