using { /Godot.org/Godot }

# The vararg spike: Godot has 33 vararg entry points (15 class methods, 12 utility functions, 6 on
# the builtin types) and the mirror skips every one of them. Generating them needs to know what
# Verse will accept when one Godot name has to become several Verse signatures.
#
#   bin/verse_probe.exe <engine>/Engine/Binaries/Win64/verse_host.dll <engine>/Engine \
#       tests/verse_probe/overload_probe.verse --class overload_probe
#
# CLAUDE.md records that Verse's overloading is "far narrower than phase-2-design.md 1 claimed",
# without saying where the edge is. Four questions:
#
#   O1  two class methods of one name, differing in arity
#   O2  two class methods of one name, differing in parameter type at equal arity
#   O3  a method with an array parameter beside one with a scalar of the element type
#   O4  one signature with an optional `?Args` parameter, which needs no overloading at all

overload_probe := class(object):

	# O1 -- arity.
	CallA<public>(Name:string)<transacts>:int = 1
	CallA<public>(Name:string, A:variant)<transacts>:int = 2

	# O2 -- type at equal arity.
	CallB<public>(A:int)<transacts>:int = 1
	CallB<public>(A:string)<transacts>:int = 2

	# O3 -- the shape a generated vararg would want: scalar beside array.
	CallC<public>(Name:string, A:variant)<transacts>:int = 1
	CallC<public>(Name:string, Args:[]variant)<transacts>:int = 2

	# O4 -- the fallback that needs no overloading: one signature, optional trailing arguments.
	CallD<public>(Name:string, ?Args:[]variant = array{})<transacts>:int = 0

	UseD<public>()<transacts>:int =
		X := CallD("one")
		Y := CallD("two", ?Args := array{})
		X + Y

	Answer<public>()<transacts>:int = 42
