using { /Godot.org/Godot }

# Does a Verse class need at least one member? The bindings emitter said it did, and wrote a
# `Bound<public>()<reads>:logic = true` filler into every binding that had nothing to carry:
#
#   bin/verse_probe.exe <engine>/Engine/Binaries/Win64/verse_host.dll <engine>/Engine \
#       tests/verse_probe/empty_class_probe.verse --class probe_main
#
# It does not. What needs a member is the *indented* form -- `class(node2d):` followed by nothing is
# a parse error -- and the brace form has no such requirement. A clean compile of this file answers
# three things, which are the three a binding has to be able to do:
#
#   - `class(node2d) {}` declares;
#   - it serves as a parameter type;
#   - it serves as a downcast target over a mirrored object, which is R-SCN-6's cast.
#
# The filler was not harmless. A binding is a mirror of a class the author wrote, so a member that
# is in the binding and not in their `.gd` reads as something they are meant to recognise -- and a
# GDScript declaring only Godot virtuals and `@export` variables has nothing else, because a leading
# underscore is skipped and properties are R-INT-9's remainder. That is an ordinary file, not a
# corner case.

# A binding with nothing callable, as the emitter now writes one.
main_script<public> := class(node2d) {}

# And one with a member, so both forms are proven to sit in the one generated file.
other_binding<public> := class(node2d):
	Greet<public>()<transacts>:string = "hi"

probe_main<public> := class:
	Take<public>(InScript:main_script)<transacts>:void =
		Print("took one")

	Run<public>(InNode:node)<transacts>:void =
		if (Cast := main_script[InNode]):
			Take(Cast)
		if (Other := other_binding[InNode]):
			Print("{Other.Greet()}")
