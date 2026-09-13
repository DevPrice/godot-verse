using { /Godot.org/Godot }
using { /Verse.org/Random }

# mob.gd, ported. `mob_types.pick_random()` is an Array method Godot's Array has and
# `godot_array` does not, so the animation names come back as a Verse `[]string` from
# SpriteFrames.get_animation_names and the pick is Verse's own GetRandomInt -- which is the naming
# rule R-AUD-2 already set: where Verse's stdlib has a counterpart, Verse's spelling wins.
@global_class
mob := class(rigid_body2d):

	@export
	var Sprite<public>:?animated_sprite2d = false

	_Ready<override>():void =
		if (Animation := Sprite?, Frames := Animation.GetSpriteFrames[]):
			Names := Frames.GetAnimationNames()
			if (Picked := Names[GetRandomInt(0, Names.Length - 1)]):
				Animation.SetAnimation(Picked)
			Animation.Play()

	# Connected in mob.tscn to VisibleOnScreenNotifier2D.screen_exited.
	OnScreenExited<public>():void =
		QueueFree()
