using { /Godot.org/Godot }

# Phase 5 spike S-0, and the *positive* half of it: every declaration in here compiles. The answers
# that come as refusals are in async_reject.verse, kept separate for the reason
# tests/coverage_diagnostic is its own project.
#
#   bin/verse_probe.exe <engine>/Engine/Binaries/Win64/verse_host.dll <engine>/Engine \
#       tests/verse_probe/async_probe.verse --class async_probe
#
# What it establishes, in docs/phase-5-design.md 2's numbering: F1, F3, F4.

async_probe := class(object):

	# A narrowed suspending body. <suspends> is a second axis rather than a level, so it composes
	# with <transacts> -- in either order -- and with <reads>. Neither of these ever actually
	# blocks, so every question here is about what the compiler allows rather than what resumes.
	WaitsNarrow<public>()<suspends><transacts>:void =
		Print("WaitsNarrow ran")

	WaitsSwapped<public>()<transacts><suspends>:void =
		Print("WaitsSwapped ran")

	WaitsReads<public>()<suspends><reads>:int = 7

	# The wide one -- <suspends> alone, which is the default effect set plus suspends. This is the
	# shape anything that *awaits* must take (F5), because awaitable.Await is no_rollback.
	WaitsWide()<suspends>:void =
		Print("WaitsWide ran")

	# F1, the load-bearing one: may a <transacts> body -- which is what Subscribe fixes a signal
	# handler at today -- start a task? SemanticAnalyzer.cpp:18655 requires `Transacts & ~Dictates`
	# and adds NoRollback only when the package cannot access epic_internal, which
	# EVerseScope::InternalUser can (SemanticScope.cpp:807-811, HostScript.cpp:295). Yes -- provided
	# the spawned body is narrow enough for the context, which is what makes F5 bite.
	SpawnFromTransacts<public>()<transacts>:void =
		spawn{WaitsNarrow()}

	SpawnSwappedFromTransacts<public>()<transacts>:void =
		spawn{WaitsSwapped()}

	# The same from a specifier-less body, which is what every mirrored virtual override is, and the
	# only context a *wide* body can be spawned from.
	SpawnWideFromDefault<public>():void =
		spawn{WaitsWide()}

	# F3: `spawn` takes a block with as many expressions as you like. The "Currently, `spawn` expects
	# a single coroutine call" refusal is on the non-VerseVM path only.
	SpawnBlock<public>()<transacts>:void =
		spawn:
			Print("first")
			WaitsNarrow()

	# F4: the structured forms all survive narrowing.
	RaceAndSync<public>()<suspends><transacts>:void =
		sync:
			WaitsNarrow()
			WaitsSwapped()
		race:
			WaitsNarrow()
			WaitsSwapped()
		rush:
			WaitsNarrow()
			WaitsSwapped()

	Branches<public>()<suspends><transacts>:void =
		branch:
			WaitsNarrow()
		WaitsSwapped()

	# What the driver calls: zero-argument and not itself suspending.
	Answer<public>()<transacts>:void =
		SpawnFromTransacts()
		SpawnBlock()
