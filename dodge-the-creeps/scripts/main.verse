using { /Godot.org/Godot }

# main.gd, ported. One thing in it is still not like the original, and it is not a missing feature:
#
# `get_tree().call_group(&"mobs", &"queue_free")` is a vararg, and a vararg is not mirrored, so the
# group is walked instead: GetNodesInGroup returns a typed_array(node) whose elements are objects
# with methods, which is the thing Phase 2 set out to make true.
#
# Everything else reads as the GDScript does. The nine `$Name` lookups are lookups again rather than
# inspector slots; the mob is configured through its own typed properties rather than through
# `Object.set` with string names; and the two signals this file listens for -- the player's `hit`
# and the HUD's `start_game` -- are the other scripts' own, subscribed to here rather than wired
# twice in the scene file.
@global_class
main := class(node):

	# Still an `@export`, and rightly: a PackedScene is a *resource* the designer chooses, not a
	# child node the script can look up.
	@export
	var MobScene<public>:?packed_scene = false

	var Player:?player = false
	var Hud:?hud = false
	var StartPosition:?marker2d = false
	var MobSpawnLocation:?path_follow2d = false
	var MobTimer:?timer = false
	var ScoreTimer:?timer = false
	var StartTimer:?timer = false
	var Music:?audio_stream_player = false
	var DeathSound:?audio_stream_player = false

	var Score:int = 0

	_Ready<override>():void =
		if (Found := GetNode["Player"], Typed := player[Found]):
			set Player = option{Typed}
		if (FoundHud := GetNode["HUD"], TypedHud := hud[FoundHud]):
			set Hud = option{TypedHud}
		if (FoundStart := GetNode["StartPosition"], TypedStart := marker2d[FoundStart]):
			set StartPosition = option{TypedStart}
		if (FoundSpawn := GetNode["MobPath/MobSpawnLocation"], TypedSpawn := path_follow2d[FoundSpawn]):
			set MobSpawnLocation = option{TypedSpawn}
		if (FoundMob := GetNode["MobTimer"], TypedMob := timer[FoundMob]):
			set MobTimer = option{TypedMob}
		if (FoundScore := GetNode["ScoreTimer"], TypedScore := timer[FoundScore]):
			set ScoreTimer = option{TypedScore}
		if (FoundStartTimer := GetNode["StartTimer"], TypedStartTimer := timer[FoundStartTimer]):
			set StartTimer = option{TypedStartTimer}
		if (FoundMusic := GetNode["Music"], TypedMusic := audio_stream_player[FoundMusic]):
			set Music = option{TypedMusic}
		if (FoundDeath := GetNode["DeathSound"], TypedDeath := audio_stream_player[FoundDeath]):
			set DeathSound = option{TypedDeath}

		# The other scripts' own signals, subscribed to from code. The port used to wire
		# Area2D.body_entered to both the player's handler and this one, and StartButton.pressed to
		# both the HUD's and this one, because neither script could declare a signal of its own --
		# which is why GameOver used to take a body it never looked at.
		if (Hero := Player?):
			Hero.Hit.Subscribe(GameOver)
		if (Overlay := Hud?):
			Overlay.StartGame.Subscribe(NewGame)

	# The player's own `hit`, not Godot's body_entered.
	#
	# No effect specifier, where every other handler in this port carries `<transacts>`, and the
	# difference is load-bearing: the HUD's game-over sequence awaits two timers, an awaiting body
	# cannot be narrowed, and `spawn` of one may only happen from a caller that is not narrowed
	# either. A signal handler is such a caller since Phase 5 widened `Subscribe`'s callback to
	# match Verse's own `subscribable`. Everything this calls is narrower, which is allowed --
	# narrowing is contagious downward, not upward.
	GameOver<public>():void =
		if (Countdown := ScoreTimer?):
			Countdown.Stop()
		if (Countdown := MobTimer?):
			Countdown.Stop()
		if (Overlay := Hud?):
			spawn{Overlay.ShowGameOver()}
		if (Track := Music?):
			Track.Stop()
		if (Sound := DeathSound?):
			Sound.Play()

	# The HUD's own `start_game`.
	NewGame<public>()<transacts>:void =
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
			set Spawn.ProgressRatio = GodotStatics.RandfRange(0.0, 1.0)

			# Perpendicular to the path direction, with some randomness added.
			Heading := Spawn.Rotation + PiFloat * 0.5 + GodotStatics.RandfRange(-PiFloat * 0.25, PiFloat * 0.25)
			Velocity := vector2{X := GodotStatics.RandfRange(150.0, 250.0), Y := 0.0}.Rotated(Heading)

			# `PackedScene.instantiate` answers a `node`, and the cast is what turns it into the
			# rigid_body2d whose LinearVelocity this wants. Before it, the three lines below were
			# `Object.set` with string names and a hand-built variant apiece.
			if (Spawned := Scene.Instantiate[packed_scene_gen_edit_state.Disabled], Mob := rigid_body2d[Spawned]):
				set Mob.Position = Spawn.Position
				set Mob.Rotation = Heading
				set Mob.LinearVelocity = Velocity
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
