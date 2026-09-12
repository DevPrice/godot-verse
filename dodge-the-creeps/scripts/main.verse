using { /Godot.org/Godot }
using { /Verse.org/Random }

# main.gd, ported. Three things in it are where the port is least like the original, and all three
# are in docs/dodge-the-creeps.md:
#
# `$Player` and `$HUD` are `@export`-ed slots typed as the other *scripts'* classes, which is how
# this file reaches Start, UpdateScore and ShowGameOver at all. Typed as the Godot classes they
# derive from -- `?area2d`, `?canvas_layer` -- those methods would not be on the type.
#
# `get_tree().call_group(&"mobs", &"queue_free")` is a vararg, and a vararg is not mirrored, so the
# group is walked instead: GetNodesInGroup returns a typed_array(node) whose elements are objects
# with methods, which is the thing Phase 2 set out to make true.
#
# The mob's position, rotation and velocity are set through Object.set with a variant, not as
# properties. PackedScene.instantiate returns a `node`, and turning that into the rigid_body2d whose
# LinearVelocity this wants needs `is`/`as` (R-SCN-6), which is specified and not built.
@global_class
main := class(node):

	@export
	var MobScene<public>:?packed_scene = false

	@export
	@export_group("Nodes")
	var Player<public>:?player = false

	@export
	var Hud<public>:?hud = false

	@export
	var StartPosition<public>:?marker2d = false

	@export
	var MobSpawnLocation<public>:?path_follow2d = false

	@export
	@export_group("Timers")
	var MobTimer<public>:?timer = false

	@export
	var ScoreTimer<public>:?timer = false

	@export
	var StartTimer<public>:?timer = false

	@export
	@export_group("Audio")
	var Music<public>:?audio_stream_player = false

	@export
	var DeathSound<public>:?audio_stream_player = false

	var Score:int = 0

	# Connected in main.tscn to Player.body_entered. It takes a body it does not use because the
	# signal it is standing in for -- the player's own `hit` -- cannot be declared in Verse.
	GameOver<public>(Body:node2d):void =
		if (Countdown := ScoreTimer?):
			Countdown.Stop()
		if (Countdown := MobTimer?):
			Countdown.Stop()
		if (Overlay := Hud?):
			Overlay.ShowGameOver()
		if (Track := Music?):
			Track.Stop()
		if (Sound := DeathSound?):
			Sound.Play()

	# Connected in main.tscn to HUD/StartButton.pressed.
	NewGame<public>():void =
		if (Tree := GetTree[]):
			for (Mob : Tree.GetNodesInGroup("mobs").ToArray()):
				Mob.QueueFree()
		set Score = 0
		if (Hero := Player?, Start := StartPosition?):
			Hero.Start(Start.Position)
		if (Countdown := StartTimer?):
			Countdown.Start()
		if (Overlay := Hud?):
			Overlay.UpdateScore(Score)
			Overlay.ShowMessage("Get Ready")
		if (Track := Music?):
			Track.Play()

	# Connected in main.tscn to MobTimer.timeout.
	OnMobTimerTimeout<public>():void =
		if (Scene := MobScene?, Spawn := MobSpawnLocation?):
			# A random location on the Path2D, read back after it is set: Position and Rotation are
			# where the PathFollow2D now is, not where it was.
			set Spawn.ProgressRatio = GetRandomFloat(0.0, 1.0)

			# Perpendicular to the path direction, with some randomness added.
			Direction := Spawn.Rotation + HalfPi + GetRandomFloat(-QuarterPi, QuarterPi)
			Velocity := V2Rotated(vector2{X := GetRandomFloat(150.0, 250.0), Y := 0.0}, Direction)

			if (Mob := Scene.Instantiate[packed_scene_gen_edit_state.Disabled]):
				Mob.Set("position", VariantFromVector2(Spawn.Position))
				Mob.Set("rotation", VariantFromFloat(Direction))
				Mob.Set("linear_velocity", VariantFromVector2(Velocity))
				AddChild(Mob, false, node_internal_mode.Disabled)

	# Connected in main.tscn to ScoreTimer.timeout.
	OnScoreTimerTimeout<public>():void =
		set Score = Score + 1
		if (Overlay := Hud?):
			Overlay.UpdateScore(Score)

	# Connected in main.tscn to StartTimer.timeout.
	OnStartTimerTimeout<public>():void =
		if (Countdown := MobTimer?):
			Countdown.Start()
		if (Countdown := ScoreTimer?):
			Countdown.Start()
