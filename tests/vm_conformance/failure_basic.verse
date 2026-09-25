using { /Godot.org/Godot }

# spec/failure.md: full/fast failure contexts, the undo log, nesting, runtime errors.
# Each risky method (a decline or a raise at the top level) is its own, because a raise or a
# top-level decline drops every Print the call made (facts.md, "a raise erases the whole call's
# Prints"; failure.md §4, a <decides> entry the host called that declines answers VH_ERR_FAILED).

failure_basic := class(object):

	var X:int = 0
	var Y:int = 0
	var Counter:int = 0
	var Survivor:int = 0

	Bump(N:int)<decides><transacts>:void =
		set Counter += 1
		N > 0

	VarOnFailure<public>():void =
		set X = 0
		if (set X = 1, X > 5) {}
		Print("after failing condition, X={X}")

	VarOnSuccess<public>():void =
		set X = 0
		if (set X = 1, X > 0) {}
		Print("after succeeding condition, X={X}")

	NestedCommitThenOuterSucceeds<public>():void =
		set Y = 0
		if (set Y = 1, Inner := option{set Y = 2}, Y > 0):
			Print("outer succeeded, Y={Y}")

	NestedCommitThenOuterFails<public>():void =
		set Y = 0
		if (set Y = 1, Inner := option{set Y = 2}, Y > 5) {}
		Print("outer failed, Y={Y}")

	FailureAcrossCall<public>():void =
		set Counter = 0
		if (Bump[-1]) {}
		Print("after failed call, Counter={Counter}")
		if (Bump[1]) {}
		Print("after succeeding call, Counter={Counter}")

	ArrayAppendRollback<public>():void =
		var A:[]int = array{1, 2, 3}
		if (set A += array{4, 5}, A.Length > 10) {}
		Print("after failed append, Length={A.Length}")

	MapInsertRollback<public>():void =
		var M:[int]int = map{1 => 10}
		if (set M[2] = 20, M.Length > 10) {}
		Print("after failed insert, Length={M.Length}")

	MapReplaceRollback<public>():void =
		var M:[int]int = map{1 => 10}
		if (set M[1] = 99, M.Length > 10) {}
		if (V := M[1]) then Print("after failed replace, M[1]={V}")

	DeferredEffectsOrder<public>():void =
		Print("before")
		if (Print("inside a condition that fails"), false?) {}
		if (Print("inside a condition that succeeds"), true?):
			Print("still inside")
		Print("after")

	DeclinesAtTop<public>()<decides>:void =
		set Survivor = 1
		Print("printed before decline")
		set Survivor = 2
		false?

	DeclinesAtTopAfter<public>():void =
		Print("Survivor={Survivor}")

	RuntimeErrorWrites<public>():void =
		set Survivor = 1
		Print("printed before Err")
		set Survivor = 2
		Err("probe runtime error")

	RuntimeErrorAfter<public>():void =
		Print("Survivor={Survivor}")

	RuntimeErrorEmptyMessage<public>():void =
		Err("")
