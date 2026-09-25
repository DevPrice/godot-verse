using { /Godot.org/Godot }

# T1.5 (docs/web-vm/spec/failure.md): what a failure undoes, what it keeps, and what a runtime
# error does, run on Epic's VerseVM through the UE host. Every public zero-argument method is called
# by the probe driver in declaration order, each as its own VM entry with a tick between, and every
# printed line is the record.
#
#   bin/verse_probe.exe <engine>/Engine/Binaries/Win64/verse_host.dll <engine>/Engine \
#       <abs path>/tests/verse_probe/vm_failure_probe.verse --class vm_failure_probe
#
# Run it from bin/, where build_host.py leaves tbbmalloc.dll.
#
# Print is this repository's native and defers its output to the commit of the outermost
# transaction, so a line printed inside a failed condition never appears: that absence is itself
# one of the readings below.

failure_box := class:
	var V:int = 0

vm_failure_probe := class(object):

	var Counter:int = 0
	var Survivor:int = 0

	# --- 1. A var written in a condition that then fails is restored; one that succeeds is kept.
	VarOnFailureAndSuccess<public>()<transacts>:void =
		var X:int = 0
		if (set X = 1, X > 5) {}
		Print("VarOnFailure: X = {X}")
		if (set X = 2, X > 0) {}
		Print("VarOnSuccess: X = {X}")

	# --- 2. Nesting. An inner context that fails inside an outer one that succeeds loses only its
	# own writes; an inner one that succeeds inside an outer one that fails loses everything.
	Nesting<public>()<transacts>:void =
		var X:int = 0
		if (set X = 1, Inner := option{set X = 2, X > 5}, true?) {}
		Print("Nesting, inner fails, outer succeeds: X = {X}")
		var Y:int = 0
		if (set Y = 1, Inner := option{set Y = 2}, Y > 5) {}
		Print("Nesting, inner succeeds, outer fails: Y = {Y}")

	# --- 3. A field of an object, an element and an append of a mutable array, a map insert and a
	# map replace, each written in a failing condition.
	HeapWrites<public>()<transacts>:void =
		B := failure_box{}
		if (set B.V = 7, B.V > 100) {}
		Print("HeapWrites: field after failure = {B.V}")
		var A:[]int = array{1, 2, 3}
		if (set A[0] = 9, A[0] > 100) {}
		Print("HeapWrites: element after failure = {A[0] or -1}")
		if (set A += array{4, 5}, A.Length > 100) {}
		Print("HeapWrites: length after failed append = {A.Length}")
		var M:[int]int = map{1 => 10}
		if (set M[2] = 20, M.Length > 100) {}
		Print("HeapWrites: map length after failed insert = {M.Length}")
		if (set M[1] = 99, M.Length > 100) {}
		Print("HeapWrites: map value after failed replace = {M[1] or -1}")
		# Not probed: a struct field. `set P.A = 5` on a `var P` of a struct type is error 3509
		# unless A is `var`, and a struct may not declare a `var` member (error 3607).

	# --- 4. A deferred native effect inside a failing condition never happens; inside a succeeding
	# one it happens at commit, in program order with the lines around it.
	DeferredEffects<public>()<transacts>:void =
		Print("DeferredEffects: before")
		if (Print("DeferredEffects: inside a condition that fails"), 1 = 2) {}
		if (Print("DeferredEffects: inside a condition that succeeds"), 1 = 1) {}
		Print("DeferredEffects: after")

	# --- 5. Failure crosses a call: the callee's writes are undone with the caller's context.
	FailureAcrossCall<public>()<transacts>:void =
		set Counter = 0
		if (Bump[-1]) {}
		Print("FailureAcrossCall: Counter after a failed call = {Counter}")
		if (Bump[1]) {}
		Print("FailureAcrossCall: Counter after a succeeding call = {Counter}")

	Bump(N:int)<transacts><decides>:void =
		set Counter += 1
		N > 0

	# --- 6. `or`, `not`, option{}, and a for-with-filter, each a failure context of its own.
	FailureForms<public>()<transacts>:void =
		A := array{10, 20, 30}
		Missing := A[7] or -1
		Print("FailureForms: A[7] or -1 = {Missing}")
		var X:int = 0
		if (not (set X = 5, X > 100)) {}
		Print("FailureForms: X after `not` of a failing write = {X}")
		O := option{set X = 6, X > 100}
		Print("FailureForms: option of a failing write is empty {if (O?) then "no" else "yes"}, X = {X}")
		Kept := for (E : A, E <> 20) { E }
		Print("FailureForms: for with a filter kept {Kept.Length} of 3")

	# --- 7. A <decides> method the embedder calls directly, which declines: the root context fails,
	# so its write and its output are gone and the call answers VH_ERR_FAILED (status 7).
	DeclinesAtTop<public>()<transacts><decides>:void =
		set Survivor = 5
		Print("DeclinesAtTop: printed before declining")
		Survivor > 100

	DeclinesAtTopAfter<public>()<transacts>:void =
		Print("DeclinesAtTopAfter: Survivor = {Survivor}")

	# --- 8. A runtime error rolls back everything this entry did, including deferred output, and
	# a context around it does not catch it.
	RuntimeErrorWrites<public>()<transacts>:void =
		set Survivor = 1
		Print("RuntimeErrorWrites: printed before the error")
		set Survivor = 2
		Err("probe runtime error")

	RuntimeErrorAfter<public>()<transacts>:void =
		Print("RuntimeErrorAfter: Survivor = {Survivor}")

	RuntimeErrorInCondition<public>()<transacts>:void =
		if (RaiseInside[]):
			Print("RuntimeErrorInCondition: then ran")
		else:
			Print("RuntimeErrorInCondition: else ran")

	RaiseInside()<transacts><decides>:void =
		Err("probe runtime error inside a failure context")

	RuntimeErrorNextEntry<public>()<transacts>:void =
		Print("RuntimeErrorNextEntry: the next entry runs")

	# The message of an Err with an empty string: the parenthesised part is still present.
	RuntimeErrorEmptyMessage<public>()<transacts>:void =
		Err("")
