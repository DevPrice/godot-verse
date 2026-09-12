using { /Godot.org/Godot }

# player.gd, ported. Two differences from the GDScript are not stylistic, and both are recorded in
# docs/dodge-the-creeps.md:
#
# `$AnimatedSprite2D` becomes an `@export`-ed slot the scene fills in. A Verse script can ask for a
# child by path -- GetNode returns a `node` -- but nothing can turn that `node` into the
# `animated_sprite2d` whose Play() the next line wants, because `is`/`as` (R-SCN-6) is specified and
# not built. An inspector slot is typed, so it is the spelling that works today.
#
# `signal hit` has no Verse spelling at all (R-SIG-1). So the scene wires Godot's own
# Area2D.body_entered to *both* this script and main's, instead of this script emitting a signal of
# its own that main listens for. Two connections from one engine signal, rather than one
# script-declared signal -- which is also why main's handler takes a body it does not use.
@global_class
player := class(area2d):

	# How fast the player will move (pixels/sec).
	@export
	var Speed<public>:float = 400.0

	@export
	@export_group("Nodes")
	var Sprite<public>:?animated_sprite2d = false

	@export
	var Hitbox<public>:?collision_shape2d = false

	@export
	var Trail<public>:?gpu_particles2d = false

	# Size of the game window.
	var ScreenSize:vector2 = vector2{}

	Ready<override>():void =
		set ScreenSize = GetViewportRect().Size
		Hide()

	Process<override>(Delta:float):void =
		# The player's movement vector.
		var Velocity:vector2 = vector2{}
		if (Keys := GetInputSingleton[]):
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

		if (Direction := V2Normalized[Velocity]):
			set Velocity = V2Scale(Direction, Speed)
			if (Animation := Sprite?):
				Animation.Play()
		else:
			if (Animation := Sprite?):
				Animation.Stop()

		set Position = V2Clamp(V2Add(Position, V2Scale(Velocity, Delta)), vector2{}, ScreenSize)

		if (Animation := Sprite?):
			if (Velocity.X <> 0.0):
				Animation.SetAnimation("right")
				set Animation.FlipV = false
				set Animation.FlipH = if (Velocity.X < 0.0) then true else false
				if (Particles := Trail?):
					set Particles.Rotation = 0.0
			else if (Velocity.Y <> 0.0):
				Animation.SetAnimation("up")
				set Rotation = if (Velocity.Y > 0.0) then Pi else 0.0

	# Called by main when a round starts.
	Start<public>(Pos:vector2):void =
		set Position = Pos
		set Rotation = 0.0
		Show()
		if (Shape := Hitbox?):
			set Shape.Disabled = false

	# Connected in player.tscn to this node's own body_entered.
	OnBodyEntered<public>(Body:node2d):void =
		# The player disappears after being hit.
		Hide()
		# Must be deferred, as a physics property cannot change inside a physics callback. The value
		# crosses as a variant, which a script can build for a bool but not read back -- R-TYPE-7 as
		# Phase 2 amended it.
		if (Shape := Hitbox?):
			Shape.SetDeferred("disabled", VariantFromBool(true))
