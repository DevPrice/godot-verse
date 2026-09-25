using { /Godot.org/Godot }

# T1.5 (docs/web-vm/spec/unification.md §11.1): the file of refusals. It compiles nothing on
# purpose; the text of each refusal is the result. Each is a source spelling that would have made
# compiled code read a variable before its definition, which is the only way a placeholder -- and so
# a runtime park -- could arise in a function body, a class or module data.
#
#   bin/verse_probe.exe <engine>/Engine/Binaries/Win64/verse_host.dll <engine>/Engine \
#       <abs path>/tests/verse_probe/vm_unification_reject.verse
#
# Expected, at this commit:
#   line 21: error 3601, Data definitions at this scope must be initialized with a value.
#   line 27: error 3502, Accessing instance member `A` from this scope is not yet implemented.
#   line 31: error 3502, Accessing instance member `A` from this scope is not yet implemented.
#   line 34: error 3502, Accessing a variable from the initializer of a variable that precedes it
#            in the same snippet, or is located in a different snippet, isn't implemented yet. ...

reject_local := class:
	# A local declared without a value, bound later.
	Declare()<transacts>:void =
		X:int
		X = 5 or Err("unreachable")

# A field initializer reading an earlier field of the same object.
reject_field_after := class:
	A:int = 1
	B:int = A + 1

# The same, reading a later field.
reject_field_before := class:
	B:int = A + 1
	A:int = 1

ModuleLater:int = ModuleEarlier + 1
ModuleEarlier:int = 2
