# The dodge-the-creeps checks, as a library two drivers share.
#
# `headless_check.gd` runs them against the editor binary, where the project is compiled at
# startup; `export_check.gd` runs the same lines inside an exported game, where it was cooked
# instead. One set of lines, so the two cannot disagree about what passing means -- which is the
# whole point of running them twice.
#
# --fixed-fps matters to both. Headless, the main loop runs as fast as it can and a Timer counts
# real seconds, so without it two seconds of StartTimer never elapse and nothing after "Get Ready"
# happens.
extends RefCounted

# Whose tree these run against. The SceneTree driver is one; an autoload's get_tree() is the other.
var tree: SceneTree

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

func begin() -> void:
	main = load("res://main.tscn").instantiate()
	tree.root.add_child(main)
	player = main.get_node("Player")
	hud = main.get_node("HUD")
	check("main.tscn instantiates with a Verse script", main.get_script() != null)
	# The nine node references main used to expose as inspector slots are lookups again: `GetNode`
	# and a cast (R-SCN-6). What is left as an export is the one thing that should be -- a
	# PackedScene is a resource the designer chooses, not a child the script can look up.
	#
	# The property *list* is the question, and asking `get("Player")` instead was only ever a proxy
	# for it: an instance answers a read of any member it declares, exported or not, and whether
	# that particular read succeeds turned out to differ between a compiled host and a cooked one.
	var slots: Array = main.get_property_list().map(func(p: Dictionary) -> String: return p.name)
	check("the node references are gone from the inspector", not slots.has("Player"), str(slots))
	check("an export slot typed as a Resource still resolves", main.get("MobScene") != null)
	check("a script-declared signal is visible to Godot",
		player.has_signal("Hit") and hud.has_signal("StartGame"))
	check("a Verse method is visible to Godot", main.has_method("NewGame"))

func step() -> bool:
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
			# The HUD's own `start_game`, which main subscribed to: `pressed` reaches only the HUD
			# now, where the port used to wire that one engine signal to two scripts.
			check("the HUD's own StartGame signal reached main, which started the round",
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
			check("mobs spawn", tree.get_nodes_in_group("mobs").size() > 0)
			var mob = tree.get_nodes_in_group("mobs").front()
			if mob != null:
				# Typed properties on a cast node, where these were `Object.set` with string
				# names and a hand-built variant apiece.
				check("the cast mob was placed on the spawn path",
					mob.position != Vector2.ZERO, str(mob.position))
				check("and given a velocity through its own property",
					mob.linear_velocity.length() > 100.0, str(mob.linear_velocity))
				check("and rotated", mob.rotation != 0.0, str(mob.rotation))
				check("mob.Ready picked one of the three animations",
					mob.get_node("AnimatedSprite2D").animation in
						[StringName("walk"), StringName("swim"), StringName("fly")],
					str(mob.get_node("AnimatedSprite2D").animation))
			mobs_seen = tree.get_nodes_in_group("mobs").size()
		202:
			# Emitted rather than waited for: VisibleOnScreenNotifier2D wants a drawn viewport, and
			# what is under test is the Verse handler, not Godot's visibility system.
			tree.get_nodes_in_group("mobs").front().get_node("VisibleOnScreenNotifier2D").screen_exited.emit()
		204:
			check("screen_exited reached mob's Verse handler, which freed it",
				tree.get_nodes_in_group("mobs").size() == mobs_seen - 1,
				str(tree.get_nodes_in_group("mobs").size()) + " of " + str(mobs_seen))
		205:
			player.body_entered.emit(tree.get_nodes_in_group("mobs").front())
		207:
			check("body_entered reached the player's Verse handler", not player.visible)
			# main subscribed to the *player's* own `Hit`, rather than the scene wiring one engine
			# signal to two scripts.
			check("the player's own Hit signal reached main, which stopped the round",
				(main.get_node("ScoreTimer") as Timer).is_stopped()
				and (main.get_node("MobTimer") as Timer).is_stopped())
			check("the game-over message shows",
				hud.get_node("MessageLabel").text == "Game Over",
				hud.get_node("MessageLabel").text)
		275:
			# The first `await` in the HUD's game-over sequence: MessageTimer's timeout resumed
			# the task, which set the title. Nothing reads a phase enum any more -- there is none.
			check("awaiting MessageTimer.timeout resumed the sequence at the title",
				hud.get_node("MessageLabel").text == "Dodge the\nCreeps",
				hud.get_node("MessageLabel").text)
		345:
			# And the second: `get_tree().create_timer(1.0).timeout`, which had no spelling before
			# Phase 5 and is why this scene used to carry a Timer node the original does not.
			check("and awaiting a SceneTreeTimer showed the Start button",
				hud.get_node("StartButton").visible)
			mobs_seen = tree.get_nodes_in_group("mobs").size()
			hud.get_node("StartButton").pressed.emit()
		348:
			check("a second round walks the mobs group and frees them",
				mobs_seen > 0 and tree.get_nodes_in_group("mobs").is_empty(),
				str(mobs_seen) + " cleared to " + str(tree.get_nodes_in_group("mobs").size()))
			check("the player is back at the start position", player.visible
				and player.position == Vector2(240, 450), str(player.position))
			print("headless_check: " + ("all checks passed" if failures == 0
				else str(failures) + " checks FAILED"))
	return frame >= 349
