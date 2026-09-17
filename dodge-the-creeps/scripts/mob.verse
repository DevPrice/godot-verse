using { /Godot.org/Godot }

# `godot_array` has no `pick_random()`, so `GetAnimationNames()` comes back as a Verse `[]string`
# and the pick uses Godot's own `RandiRange` rather than a Verse RNG -- a Verse-side RNG would
# silently ignore `seed()`/`randomize()`, so a project that seeds for a replay would get a
# different game (R-AUD-2's one exception to Verse's spelling otherwise winning).
@global_class
mob := class(rigid_body2d):

	_Ready<override>():void =
		if (Found := GetNode["AnimatedSprite2D"], Animation := animated_sprite2d[Found], Frames := Animation.GetSpriteFrames[]):
			Names := Frames.GetAnimationNames()
			if (Picked := Names[GodotStatics.RandiRange(0, Names.Length - 1)]):
				Animation.SetAnimation(Picked)
			Animation.Play()

	# Connected in mob.tscn to VisibleOnScreenNotifier2D.screen_exited.
	OnScreenExited<public>():void =
		QueueFree()
