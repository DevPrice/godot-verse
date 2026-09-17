using { /Godot.org/Godot }

@global_class
main := class(node):

	# A PackedScene is a resource the designer picks, not a child node to look up with GetNode.
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

		if (Hero := Player?):
			Hero.Hit.Subscribe(GameOver)
		if (Overlay := Hud?):
			Overlay.StartGame.Subscribe(NewGame)

	# No effect specifier, unlike this file's other handlers: `spawn{Overlay.ShowGameOver()}` below
	# awaits an event, an awaiting body cannot be narrowed, and `spawn` may only be called from a
	# caller that is not narrowed either. A signal handler is such a caller. Narrowing is contagious
	# downward, not upward, so everything else this calls may still carry `<transacts>`.
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
			# `call_group` is a vararg, which the mirror does not carry, so the group is walked
			# instead: `GetNodesInGroup` answers a `typed_array(node)` whose elements have methods.
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
			# Read back after being set: Position and Rotation are where the PathFollow2D now is,
			# not where it was.
			set Spawn.ProgressRatio = GodotStatics.RandfRange(0.0, 1.0)

			Heading := Spawn.Rotation + PiFloat * 0.5 + GodotStatics.RandfRange(-PiFloat * 0.25, PiFloat * 0.25)
			Velocity := vector2{X := GodotStatics.RandfRange(150.0, 250.0), Y := 0.0}.Rotated(Heading)

			# `Instantiate` answers a `node`; the cast to `rigid_body2d` is what allows setting
			# LinearVelocity below.
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
