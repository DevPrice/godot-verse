using { /Godot.org/Godot }

# Phase 5 spike S-0, round two. Round one found that `spawn` itself is allowed from a <transacts>
# body -- the glitch landed on the *invocation* inside it, not on the macro -- but that a plain
# `()<suspends>:void` carries the default effect set, no_rollback included, so a <transacts> caller
# may not spawn it. This asks whether <suspends> can be narrowed the way every other effect can.
#
#   bin/verse_probe.exe <engine>/Engine/Binaries/Win64/verse_host.dll <engine>/Engine \
#       tests/verse_probe/narrow_suspends.verse --class narrow_suspends

narrow_suspends := class(object):

	# Q1: two specifiers on one declaration, suspends first.
	WaitsNarrow<public>()<suspends><transacts>:void =
		Print("WaitsNarrow ran")

	# Q2: the other order, in case the parser cares.
	WaitsNarrowSwapped<public>()<transacts><suspends>:void =
		Print("WaitsNarrowSwapped ran")

	# Q3: narrowed one step further, to what 4.5 made a const Godot read.
	WaitsReads<public>()<suspends><reads>:int = 7

	# Q4: the one that matters -- a <transacts> Subscribe handler starting a task.
	SpawnNarrowFromTransacts<public>()<transacts>:void =
		spawn{WaitsNarrow()}

	# Q5: and the swapped spelling, from the same place.
	SpawnSwappedFromTransacts<public>()<transacts>:void =
		spawn{WaitsNarrowSwapped()}

	# Q6: do the structured forms survive narrowing? race and sync are what make a narrowed
	# <suspends> worth having rather than merely legal.
	RacesNarrow<public>()<suspends><transacts>:void =
		race:
			WaitsNarrow()
			WaitsNarrowSwapped()

	# Q7 -- a narrowed suspends calling a wide one -- is in async_reject.verse, because it is
	# refused: 3512 on the invocation, naming no_rollback. Narrowing is contagious downward here
	# exactly as it is for <transacts>.

	Answer<public>()<transacts>:void =
		SpawnNarrowFromTransacts()
		SpawnSwappedFromTransacts()
