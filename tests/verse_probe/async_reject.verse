using { /Godot.org/Godot }

# Phase 5 spike S-0's negative half: the answers the compiler gives by refusing. Nothing in here
# compiles, on purpose, and the *text* of each refusal is the result -- it is what an author would
# see. Kept in its own file for the reason tests/coverage_diagnostic is its own project: one
# refusal should not take async_probe.verse's other answers down with it.
#
#   bin/verse_probe.exe <engine>/Engine/Binaries/Win64/verse_host.dll <engine>/Engine \
#       tests/verse_probe/async_reject.verse
#
# What it establishes, in docs/phase-5-design.md 2's numbering: F2, F5, F7, and half of F12.
# Expected: ten glitches -- three on the _Ready declaration, one per refusal below, and two on
# the <reads> spawn, which is refused both as a macro and at its callee.

async_reject := class(node2d):

	Ev:event(int) = event(int){}

	WaitsWide()<suspends>:void = Print("wide")

	# F7. May an override of a mirrored virtual be <suspends>? The mirror declares
	# `_Ready<public>():void = {}` (GodotClasses.native.verse:28976) with no specifier, so the base
	# carries the default effect set. Adding one does not narrow the override -- it makes a
	# *different function*, and the three glitches say so:
	#
	#   3532 "must have a distinct domain from these other functions with the same name:
	#         function (/Godot.org/Godot/node:)_Ready() in package Engine/VerseHost"
	#   3523 "has an <override> attribute, but could not find a parent function to override
	#         (perhaps the parent function's access specifiers are too restrictive?)"
	#   3532 "The function ... in package GodotScripts_1 is ambiguous with this definition"
	#
	# This is what retired D14: the bridge never gets the chance to reject a suspending virtual,
	# because the compiler has already refused it as a failed override two steps earlier.
	_Ready<override>()<suspends>:void =
		Print("ready")

	# F5, the finding that shaped the phase. An *awaiting* body cannot be narrowed:
	#
	#   3512 "calls a function (`(/Verse.org/Verse/event:)Await`) that has the 'no_rollback'
	#         effect, which is not allowed by its context"
	#
	# awaitable.Await is declared with no specifier (Awaitable.native.verse), exactly as
	# signalable.Signal is. So anything that awaits is wide, and F1's permission does not reach it.
	WaitsNarrow()<suspends><transacts>:void =
		Ev.Await()

	# F2. A <reads> body may not spawn at all -- and the glitch lands on the *macro* rather than on
	# the callee, which is how this differs from the <transacts> case:
	#
	#   3512 "This 'spawn' macro has effects that are not allowed by its context: writes, allocates"
	#
	# This is the one place Phase 4.5's narrowing and Phase 5's concurrency genuinely collide.
	SpawnFromReads<public>()<reads>:void =
		spawn{WaitsWide()}

	# F5's consequence, and wall 3's actual shape: a <transacts> handler cannot spawn a wide body.
	#
	#   3512 "calls a function ... that has effects that are not allowed by its context:
	#         transacts, no_rollback"
	#
	# Widening Subscribe's callback (D20) is what removes this from the yardstick's path.
	SpawnWideFromTransacts<public>()<transacts>:void =
		spawn{WaitsWide()}

	# Narrowing stays contagious downward, for <suspends> as for everything else.
	CallsWideFromNarrow<public>()<suspends><transacts>:void =
		WaitsWide()

	# And a <suspends> function may not be called directly from a non-suspending one at all, which
	# is why `spawn` is the entry rather than a plain call.
	CallsWaitsDirectly<public>()<transacts>:void =
		WaitsWide()

	# F12. `await` is a VerseVM + epic_internal macro (SemanticAnalyzer.cpp:18385-18399) and it does
	# resolve for our package -- the glitch is about effects, not an unknown identifier, which is
	# what the non-epic_internal path would give. Recorded because a future reader will wonder;
	# nothing in this design uses it.
	AwaitsSomething<public>()<suspends><transacts>:void =
		await{WaitsWide()}
