using { /Godot.org/Godot }

# mob.gd, ported. `mob_types.pick_random()` is an Array method Godot's Array has and `godot_array`
# does not, so the animation names come back as a Verse `[]string` from
# SpriteFrames.get_animation_names and the pick is Godot's own `randi_range`.
#
# Godot's rather than Verse's `GetRandomInt`, and that is R-AUD-2's one exception: a Verse-side RNG
# would silently ignore `seed()` and `randomize()`, so a project that seeds for a replay would get a
# different game. Everything else still follows the rule that Verse's spelling wins.
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
