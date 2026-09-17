using { /Godot.org/Godot }

# hud.gd, ported. The game-over sequence is the reason this file is worth reading: it is the one
# thing the port could not spell until Phase 5, and it now reads the way the GDScript does.
#
#     func show_game_over() -> void:
#         show_message("Game Over")
#         await $MessageTimer.timeout
#         ...
#         await get_tree().create_timer(1.0).timeout
#         ...
#
# What it used to be was a state machine: a `hud_phase` enum, a second Timer node the original
# scene does not have (because a SceneTreeTimer's timeout had no spelling), and two timeout
# handlers whose whole job was to read the phase and decide which half of the sequence they were
# in. All four are gone.
#
# **`spawn` is how a script starts one of these**, and it is not decoration. An awaiting body
# cannot carry an effect specifier -- `event.Await` is `no_rollback`, so `<suspends><transacts>`
# would not compile -- and a caller of one cannot either. A mirrored virtual override is
# specifier-less, and so is a signal handler, so both are contexts a `spawn` may happen in. What is
# *not* a spelling is `_Ready<override>()<suspends>`: the specifier makes it a different function,
# so it overrides nothing, and the compiler says so at that line.

@global_class
hud := class(canvas_layer):

	# `signal start_game` in the GDScript. main subscribes to it, where the port used to have
	# main.tscn connect StartButton.pressed to main's own NewGame -- one engine signal wired to two
	# scripts, because this one had nothing to emit.
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

	# Line for line what show_game_over is in the GDScript. `<suspends>` and no other specifier,
	# because `Await` is `no_rollback` and an explicit effect would narrow this below it.
	ShowGameOver<public>()<suspends>:void =
		ShowMessage("Game Over")
		if (Countdown := MessageTimer?):
			Countdown.Timeout().Await()
		if (Message := MessageLabel?):
			Message.SetText("Dodge the\nCreeps")
			Message.Show()
		if (Tree := GetTree[], Wait := Tree.CreateTimer[1.0]):
			Wait.Timeout().Await()
		if (Button := StartButton?):
			Button.Show()

	UpdateScore<public>(Score:int)<transacts>:void =
		if (Display := ScoreLabel?):
			Display.SetText("{Score}")

	# The other half of the sequence the state machine used to carry: MessageTimer's timeout when
	# the message was "Get Ready" rather than "Game Over". Nothing decides which case it is any
	# more -- the game-over path awaits the same timeout directly, and this is only the plain one.
	#
	# Still connected in hud.tscn, because a message that hides itself after a second is a property
	# of the label rather than of a sequence.
	OnMessageTimeout<public>()<transacts>:void =
		if (Message := MessageLabel?):
			Message.Hide()

	# Connected in hud.tscn to StartButton.pressed.
	OnStartPressed<public>():void =
		if (Button := StartButton?):
			Button.Hide()
		StartGame.Emit(())
