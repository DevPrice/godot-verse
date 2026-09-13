using { /Godot.org/Godot }

# Phase 4.5 spike S-2 (docs/phase-4.5-design.md 2), and the *positive* half of the record: every
# declaration in here compiles, and the probe calls `Answer` to show a `<reads>` chain runs.
#
#   bin/verse_probe.exe <engine>/Engine/Binaries/Win64/verse_host.dll <engine>/Engine \
#       tests/verse_probe/effects_probe.verse --class effects_probe
#
# S-1's answer is in effects_reject.verse, because it is an answer the compiler gives by refusing.

effects_probe := class(object):

	# The widening direction, which is the one narrowing needs: a `<transacts>` caller reaching a
	# `<reads>` callee. Were this refused, narrowing 6766 declarations would break every script.
	PureRead<public>()<reads>:int = 7

	FromTransacts<public>()<transacts>:int = PureRead()

	# One level deeper: a `<reads>` body calling another `<reads>` function, which is what a library
	# of read-only helpers is made of.
	ReadsCallsReads<public>()<reads>:int = PureRead() + 1

	# `<decides>` composes with the narrower effect, so a failable read-only helper is spellable.
	FallibleRead<public>()<decides><reads>:int =
		X := PureRead()
		X > 0
		X

	# A failure context inside a `<reads>` body, which is wall 8's own shape with the effect
	# narrowed one step further than wall 8 could go.
	GuardsInReads<public>()<reads>:int =
		if (X := FallibleRead[]) then X else 0

	Answer<public>()<transacts>:int = FromTransacts() + ReadsCallsReads() + GuardsInReads()
