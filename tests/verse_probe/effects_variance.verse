using { /Godot.org/Godot }

# Is a function type contravariant in its effects? `typed_array(t)` carries its element packer as a
# *typed member* -- `Unpack:type{_(:variant)<decides><transacts>:t}` -- so if narrowing the packers
# to `<reads>` stops them satisfying that member, Stage 1 has to narrow the member types too, and
# every wrapper built on them with it.

holder := class:
	Take<public>:type{_(:int)<transacts>:int}
	TakeDecides<public>:type{_(:int)<decides><transacts>:int}

effects_variance := class(object):

	Narrow<public>(X:int)<reads>:int = X + 1

	NarrowDecides<public>(X:int)<decides><reads>:int =
		X > 0
		X

	# A `<reads>` function where a `<transacts>` one is declared. If effects are contravariant this
	# is legal, because anything that may call a `<transacts>` function may call a `<reads>` one.
	Build<public>()<transacts>:int =
		H := holder{Take := Narrow, TakeDecides := NarrowDecides}
		(H.Take)(1)

	Answer<public>()<transacts>:int = Build()
