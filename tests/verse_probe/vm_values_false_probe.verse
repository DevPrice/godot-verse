using { /Godot.org/Godot }

# T1.3: `false` standing in for an empty array or map, reached through a path that indexes it
# outside an `if` condition. Kept apart from vm_values_probe.verse because the outcome might be a
# host abort rather than a failure or a runtime error, and an abort would take every later method
# in the same file down with it. Measured: it is an abort. IndexFalseArrayInDecides takes the whole
# process down with a fatal assertion and prints nothing, so this file is the record of that and
# nothing else runs after it.

vm_values_false_probe := class(object):

	FirstOf(A:[]int)<decides><transacts>:int = A[0]

	LookUp(M:[int]int, K:int)<decides><transacts>:int = M[K]

	IndexFalseArrayInDecides<public>()<transacts>:void =
		F:[]int = false
		if (X := FirstOf[F]) then Print("fa: FirstOf[false] succeeds") else Print("fa: FirstOf[false] fails")
		if (X := FirstOf[array{4}]) then Print("fa: FirstOf[array 4] = {X}")

	# `F:[int]int = false` is refused: error 3509, "This variable expects to be initialized with a
	# value of type [int]int, but this initializer is an incompatible value of type true." -- the
	# compiler's own wording, `true` included. Only arrays (and so strings) accept `false`.
