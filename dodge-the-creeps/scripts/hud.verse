using { /Godot.org/Godot }

# What phase of the game-over sequence the HUD is in.
#
# hud.gd spells that sequence with `await`: show "Game Over", await MessageTimer.timeout, show the
# title, await a one-second tree timer, show the button. A Verse script cannot await a Godot signal
# (R-SIG-5), and Verse's own concurrency has no meeting point with one yet, so the sequence becomes
# what it was before `await` existed: a state machine, one timer node per wait, driven by the
# timeouts the scene connects. It behaves identically and it is four times the code.
hud_phase := enum{Idle, Message, GameOver, Title}

@global_class
hud := class(canvas_layer):

	@export
	var ScoreLabel<public>:?label = false

	@export
	var MessageLabel<public>:?label = false

	@export
	var StartButton<public>:?button = false

	@export
	var MessageTimer<public>:?timer = false

	# The one node this scene has that the GDScript version does not: the second wait in
	# show_game_over was `get_tree().create_timer(1).timeout`, and a SceneTreeTimer's timeout is a
	# signal with nothing to connect it to from here.
	@export
	var TitleTimer<public>:?timer = false

	var Phase:hud_phase = hud_phase.Idle

	ShowMessage<public>(Text:string):void =
		set Phase = hud_phase.Message
		if (Message := MessageLabel?):
			Message.SetText(Text)
			Message.Show()
		if (Countdown := MessageTimer?):
			Countdown.Start()

	ShowGameOver<public>():void =
		ShowMessage("Game Over")
		set Phase = hud_phase.GameOver

	UpdateScore<public>(Score:int):void =
		if (Display := ScoreLabel?):
			Display.SetText("{Score}")

	# Connected in hud.tscn to MessageTimer.timeout.
	OnMessageTimeout<public>():void =
		if (Phase = hud_phase.GameOver):
			set Phase = hud_phase.Title
			if (Message := MessageLabel?):
				Message.SetText("Dodge the\nCreeps")
				Message.Show()
			if (Countdown := TitleTimer?):
				Countdown.Start()
		else:
			set Phase = hud_phase.Idle
			if (Message := MessageLabel?):
				Message.Hide()

	# Connected in hud.tscn to TitleTimer.timeout.
	OnTitleTimeout<public>():void =
		set Phase = hud_phase.Idle
		if (Button := StartButton?):
			Button.Show()

	# Connected in hud.tscn to StartButton.pressed. main.tscn connects the same signal to main's
	# NewGame, because `signal start_game` has no Verse spelling (R-SIG-1) and so this script has
	# nothing to emit.
	OnStartPressed<public>():void =
		if (Button := StartButton?):
			Button.Hide()
