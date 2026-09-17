using { /Godot.org/Godot }

# Can `@signal` be the attribute's name? The attribute package shares `/Godot.org/Godot`'s verse
# path, so an attribute class named `signal` lands in the same module namespace as GodotApi's
# `signal(t)` and its `signal() := signal(tuple())` alias. A bare marker attribute *is* a class, so
# there is no second spelling to fall back on -- `@export_category` can be a constructor function
# beside `export_category_attribute` only because it takes an argument.
#
#   bin/verse_probe.exe <engine>/Engine/Binaries/Win64/verse_host.dll <engine>/Engine \
#       tests/verse_probe/attribute_name_probe.verse --class attribute_name_probe
#
# This asks the same-package shape, which is the lower bound: `class(attribute)` cannot be declared
# in a user package at all (authorship is granted to /Godot.org/ only), so the attribute itself
# cannot be spelled here. What can be spelled is the collision -- a parametric class, its
# zero-argument alias, and a third plain class of the same name.

# The shape GodotApi already has, renamed so this file does not fight the real one.
sig<public>(t:type) := class:
	Payload:int = 0

sig<public>() := sig(tuple())

# The shape a bare marker attribute would add. If this is refused, `@signal` is unavailable while
# `signal(t)` lives in that package and the attribute needs another name -- or `signal(t)` needs
# renaming first, which is Stage 4's job and not Stage 2's.
sig<public> := class {}

attribute_name_probe := class(object):

	Answer<public>():void =
		Print("three definitions of one name compiled")
