using { /Godot.org/Godot }

# player.gd, ported. It is the GDScript with Verse's types on it now: `$AnimatedSprite2D` is a
# lookup and a cast, `velocity.normalized() * speed` is arithmetic, and `signal hit` is a member.
#
# The three node references are cached in `_Ready`, which is what GDScript's
# `@onready var sprite = $AnimatedSprite2D` is. They used to be `@export` slots the scene filled in,
# because `GetNode` answers a `node` and nothing turned that into the `animated_sprite2d` whose
# `Play()` the next line wants. R-SCN-6's cast is what took the wiring back out of the scene file
# and put it in the script, where reading the script tells you what it touches.
@global_class
player := class(area2d):

	# How fast the player will move (pixels/sec).
	@export
	var Speed<public>:float = 400.0

	# `signal hit` in the GDScript. The member's *type* is the declaration and its name is the
	# signal's name; main subscribes to it rather than the scene wiring Area2D.body_entered to two
	# scripts at once, which is what the port did before a script could declare one.
	@export_signal
	Hit<public>:signal() = signal(){}

	var Sprite:?animated_sprite2d = false
	var Hitbox:?collision_shape2d = false
	var Trail:?gpu_particles2d = false

	# Size of the game window.
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
		# The player's movement vector.
		var Velocity:vector2 = vector2{}
		Keys := GetInputSingleton()
		var Dx:float = 0.0
		var Dy:float = 0.0
		if (Keys.IsActionPressed("move_right")?):
			set Dx = Dx + 1.0
		if (Keys.IsActionPressed("move_left")?):
			set Dx = Dx - 1.0
		if (Keys.IsActionPressed("move_down")?):
			set Dy = Dy + 1.0
		if (Keys.IsActionPressed("move_up")?):
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
	Start<public>(Pos:vector2)<transacts>:void =
		set Position = Pos
		set Rotation = 0.0
		Show()
		if (Shape := Hitbox?):
			set Shape.Disabled = false

	# Connected in player.tscn to this node's own body_entered, which is how a Godot author wires a
	# node's own signal to its own script -- the Node panel, not a line of code.
	OnBodyEntered<public>(Body:node2d):void =
		# The player disappears after being hit.
		Hide()
		Hit.Signal(())
		# Must be deferred, as a physics property cannot change inside a physics callback. The value
		# crosses as a variant, which a script can build for a bool but not read back -- R-TYPE-7 as
		# Phase 2 amended it.
		if (Shape := Hitbox?):
			Shape.SetDeferred("disabled", VariantBool(true))
