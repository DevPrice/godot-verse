using { /Godot.org/Godot }

@global_class
player := class(area2d):

	@export
	var Speed:float = 400.0

	# The member's type declares it as a signal; its name is the signal's name. main subscribes to
	# this instead of the scene wiring Area2D.body_entered directly.
	@export_signal
	Hit:event() = event(){}

	var Sprite:?animated_sprite2d = false
	var Hitbox:?collision_shape2d = false
	var Trail:?gpu_particles2d = false

	var ScreenSize:vector2 = vector2{}

	_Ready<override>():void =
		if (Found := GetNode["AnimatedSprite2D"], Typed := animated_sprite2d[Found]):
			set Sprite = option{Typed}
		if (FoundShape := GetNode["CollisionShape2D"], TypedShape := collision_shape2d[FoundShape]):
			set Hitbox = option{TypedShape}
		if (FoundTrail := GetNode["Trail"], TypedTrail := gpu_particles2d[FoundTrail]):
			set Trail = option{TypedTrail}

		set ScreenSize = GetViewportRect().Size
		Hide()

	_Process<override>(Delta:float):void =
		var Velocity:vector2 = vector2{}
		Keys := GetInputSingleton()
		var Dx:float = 0.0
		var Dy:float = 0.0
		if (Keys.IsActionPressed["move_right"]):
			set Dx = Dx + 1.0
		if (Keys.IsActionPressed["move_left"]):
			set Dx = Dx - 1.0
		if (Keys.IsActionPressed["move_down"]):
			set Dy = Dy + 1.0
		if (Keys.IsActionPressed["move_up"]):
			set Dy = Dy - 1.0
		set Velocity = vector2{X := Dx, Y := Dy}

		if (Velocity.Length() > 0.0):
			set Velocity = Velocity.Normalized() * Speed
			if (Animation := Sprite?):
				Animation.Play()
		else:
			if (Animation := Sprite?):
				Animation.Stop()

		set Position = (Position + Velocity * Delta).Clamp(Vector2Statics.Zero, ScreenSize)

		if (Animation := Sprite?):
			if (Velocity.X <> 0.0):
				Animation.SetAnimation("right")
				set Animation.FlipV = false
				set Animation.FlipH = if (Velocity.X < 0.0) then true else false
				if (Particles := Trail?):
					set Particles.Rotation = 0.0
			else if (Velocity.Y <> 0.0):
				Animation.SetAnimation("up")
				set Rotation = if (Velocity.Y > 0.0) then PiFloat else 0.0

	# Called by main when a round starts.
	Start(Pos:vector2)<transacts>:void =
		set Position = Pos
		set Rotation = 0.0
		Show()
		if (Shape := Hitbox?):
			set Shape.Disabled = false

	# Connected in player.tscn to this node's own body_entered signal, wired via the Node panel.
	OnBodyEntered(Body:node2d):void =
		Hide()
		Hit.Emit(())
		# Deferred: a physics property cannot change inside a physics callback. The value crosses
		# as a variant, which a script can build for a bool but not read back (R-TYPE-7).
		if (Shape := Hitbox?):
			Shape.SetDeferred("disabled", VariantBool(true))
