using { /Godot.org/Godot }

# What phase of the game-over sequence the HUD is in.
#
# hud.gd spells that sequence with `await`: show "Game Over", await MessageTimer.timeout, show the
# title, await a one-second tree timer, show the button. A Verse script cannot await a Godot signal
# (R-SIG-5), and Verse's own concurrency has no meeting point with one yet, so the sequence stays
# what it was before `await` existed: a state machine, one timer node per wait, driven by the
# timeouts the scene connects. It behaves identically and it is four times the code.
#
# **This is the one wall Phase 4 deliberately left standing**, and Phase 5 is where it falls.
hud_phase := enum{Idle, Message, GameOver, Title}

@global_class
hud := class(canvas_layer):

	# `signal start_game` in the GDScript. main subscribes to it, where the port used to have
	# main.tscn connect StartButton.pressed to main's own NewGame -- one engine signal wired to two
	# scripts, because this one had nothing to emit.
	StartGame<public>:godot_signal() = godot_signal(){}

	# The three methods main calls carry `<transacts>` and that is wall 8, not decoration: a
	# `Subscribe` handler must be `<transacts>`, an explicit specifier *replaces* the default set
	# rather than adding to it, and the default set is wider -- it contains `no_rollback`. So a
	# handler cannot call a specifier-less method, and narrowing one function pulls the specifier
	# onto everything beneath it. The compiler says so at the *call* site, which is a file away from
	# the declaration that needs changing.

	var ScoreLabel:?label = false
	var MessageLabel:?label = false
	var StartButton:?button = false
	var MessageTimer:?timer = false

	# The one node this scene has that the GDScript version does not: the second wait in
	# show_game_over was `get_tree().create_timer(1).timeout`, and a SceneTreeTimer's timeout is a
	# signal this script has no way to wait on. It goes when `await` arrives.
	var TitleTimer:?timer = false

	var Phase:hud_phase = hud_phase.Idle

	_Ready<override>():void =
		if (Found := GetNode["ScoreLabel"], Typed := label[Found]):
			set ScoreLabel = option{Typed}
		if (FoundMessage := GetNode["MessageLabel"], TypedMessage := label[FoundMessage]):
			set MessageLabel = option{TypedMessage}
		if (FoundButton := GetNode["StartButton"], TypedButton := button[FoundButton]):
			set StartButton = option{TypedButton}
		if (FoundTimer := GetNode["MessageTimer"], TypedTimer := timer[FoundTimer]):
			set MessageTimer = option{TypedTimer}
		if (FoundTitle := GetNode["TitleTimer"], TypedTitle := timer[FoundTitle]):
			set TitleTimer = option{TypedTitle}

	ShowMessage<public>(Text:string)<transacts>:void =
		set Phase = hud_phase.Message
		if (Message := MessageLabel?):
			Message.SetText(Text)
			Message.Show()
		if (Countdown := MessageTimer?):
			Countdown.Start()

	ShowGameOver<public>()<transacts>:void =
		ShowMessage("Game Over")
		set Phase = hud_phase.GameOver

	UpdateScore<public>(Score:int)<transacts>:void =
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

	# Connected in hud.tscn to StartButton.pressed.
	OnStartPressed<public>():void =
		if (Button := StartButton?):
			Button.Hide()
		StartGame.Signal(())
