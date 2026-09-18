using { /Godot.org/Godot }

@global_class
hud := class(canvas_layer):

	@export_signal
	StartGame<public>:event() = event(){}

	var ScoreLabel:?label = false
	var MessageLabel:?label = false
	var StartButton:?button = false
	var MessageTimer:?timer = false

	_Ready<override>():void =
		if (Found := GetNode["ScoreLabel"], Typed := label[Found]):
			set ScoreLabel = option{Typed}
		if (FoundMessage := GetNode["MessageLabel"], TypedMessage := label[FoundMessage]):
			set MessageLabel = option{TypedMessage}
		if (FoundButton := GetNode["StartButton"], TypedButton := button[FoundButton]):
			set StartButton = option{TypedButton}
		if (FoundTimer := GetNode["MessageTimer"], TypedTimer := timer[FoundTimer]):
			set MessageTimer = option{TypedTimer}

	ShowMessage<public>(Text:string)<transacts>:void =
		if (Message := MessageLabel?):
			Message.SetText(Text)
			Message.Show()
		if (Countdown := MessageTimer?):
			Countdown.Start()

	# `<suspends>` and no other specifier: `Await` is `no_rollback`, so an explicit effect like
	# `<transacts>` would refuse to compile.
	ShowGameOver<public>()<suspends>:void =
		ShowMessage("Game Over")
		if (Countdown := MessageTimer?):
			Countdown.Timeout().Await()
		if (Message := MessageLabel?):
			Message.SetText("Dodge the\nCreeps")
			Message.Show()
		# `CreateTimer` is total: Godot's own API marks its result as one that cannot be null, so
		# the mirror declares it that way and only `GetTree` is left to fail here.
		if (Tree := GetTree[]):
			Tree.CreateTimer(1.0).Timeout().Await()
		if (Button := StartButton?):
			Button.Show()

	UpdateScore<public>(Score:int)<transacts>:void =
		if (Display := ScoreLabel?):
			Display.SetText("{Score}")

	# Connected in hud.tscn to MessageTimer.timeout.
	OnMessageTimeout<public>()<transacts>:void =
		if (Message := MessageLabel?):
			Message.Hide()

	# Connected in hud.tscn to StartButton.pressed.
	OnStartPressed<public>():void =
		if (Button := StartButton?):
			Button.Hide()
		StartGame.Emit(())
