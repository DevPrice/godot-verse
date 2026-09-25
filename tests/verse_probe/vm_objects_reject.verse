using { /Godot.org/Godot }

# T1.6 (docs/web-vm/spec/objects.md): the refusals. This file compiles nothing on purpose; the text
# of each diagnostic is its result, the way async_reject.verse's are.
#
#   bin/verse_probe.exe <engine>/Engine/Binaries/Win64/verse_host.dll <engine>/Engine \
#       tests/verse_probe/vm_objects_reject.verse --class vm_objects_reject
#
#   R1  `union` as a type definition -- whether ordinary source can reach the union-variant ops
#   R2  a dynamic cast to a type that is not a class or an interface
#   R3  a block clause in an interface
#   R4  a block clause in a struct
#   R5  a dynamic cast to a struct
#   R6  `set` of one field of a struct held in a `var`, the source form SetField was built for

rej_union := union:
	A:int
	B:string

rej_iface := interface:
	block:
		Print("interface block")

rej_struct := struct:
	X:int = 0
	block:
		Print("struct block")

rej_point := struct:
	X:int = 0

vm_objects_reject := class(object):

	R2<public>()<transacts>:void =
		V:any = 3
		if (I := int[V]):
			Print("R2 {I}")

	# R6 -- setting one field of a struct held in a var
	R6<public>()<transacts>:void =
		var P:rej_point = rej_point{X := 1}
		set P.X = 9

	# R5 -- casting a class instance up or across without a castable relationship
	R5<public>()<transacts>:void =
		V:any = rej_struct{}
		if (S := rej_struct[V]):
			Print("R5")
