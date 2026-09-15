using { /Godot.org/Godot }

# R-NODE-3's spike: can a Verse class derived from a mirrored Godot class be constructed by an
# ordinary archetype -- `helper{}` -- and can that construction fill the inherited native `Handle`
# that every call on the object dispatches through?
#
#   bin/verse_probe.exe <engine>/Engine/Binaries/Win64/verse_host.dll <engine>/Engine \
#       tests/verse_probe/construct_probe.verse --class construct_probe
#
# `Godot.native.verse:125` records that `node{Handle := ...}` builds the class the *signature*
# named rather than the class the object *is*, which says the archetype runs. What it does not say
# is where a Handle could come from, and a Handle of 0 is a dead object that every call misdirects.
#
# Four questions, each a member below:
#
#   C1  does `ref_counted{}` compile at all, on a mirrored class?
#   C2  can a subclass's archetype write the inherited native `Handle`?
#   C3  can a `<constructor>` on a Godot-derived class run and compute `Handle` from a call?
#   C4  what effect does the construction carry -- `helper{}` in a specifier-less body?
#
# **What it answered.** C1, C3 and C4 compile: the archetype on a mirrored base is accepted, a
# constructor on a Godot-derived class is accepted, and neither is refused for its effect in a
# `<transacts>` body. C2 is glitch **3593**, "Invalid access of internal data
# `(/Godot.org/Godot:)vh_object:Handle` from control scope `/user@localhost/...`".
#
# So the obstacle to `helper{}` minting a Godot object is **access, not construction** -- R-TYPE-7
# working as designed. `ctor_delegate_probe.verse` then closed the two ways the mirror might have
# filled the field on the user's behalf, which is why R-NODE-3's spelling adopts a constructed
# object rather than relying on the archetype alone.

# C1/C2: the plain subclass, constructed two ways.
helper<public> := class(ref_counted):
	Tag<public>:int = 0

	Answer<public>()<reads>:int = Tag

# C3: the same class with a constructor that computes the inherited field.
helper_ctor<public> := class(ref_counted):
	Tag<public>:int = 0

MakeHelperCtor<public><constructor>(Tag:int)<transacts> := helper_ctor:
	Tag := Tag

construct_probe := class(object):

	# C1 -- the bare archetype on a class derived from a mirrored one.
	C1<public>()<transacts>:int =
		H := helper{}
		H.Answer()

	# C2 -- writing the inherited native Handle from the archetype.
	C2<public>()<transacts>:int =
		H := helper{Tag := 7, Handle := 1234}
		H.Handle

	# C3 -- a constructor on a Godot-derived class.
	C3<public>()<transacts>:int =
		H := MakeHelperCtor(9)
		H.Tag

	# C4 -- the effect an archetype instantiation carries here. A mirrored class descends from the
	# native vh_object, which CLAUDE.md records as refusing construction from a narrowed body; this
	# asks whether a specifier-less body is refused too.
	C4<public>()<transacts>:int =
		H := helper{Tag := 3}
		H.Tag

	Answer<public>()<transacts>:int = 42
