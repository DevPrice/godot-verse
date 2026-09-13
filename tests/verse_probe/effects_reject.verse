using { /Godot.org/Godot }

# Phase 4.5 spikes S-1 and S-2, the half whose answer is a refusal. **This file is expected not to
# compile** -- it is kept so the two refusals that shaped the phase can be re-run rather than taken
# on trust, and it is deliberately not named after a class, so nothing tries to instantiate it.
#
#   bin/verse_probe.exe <engine>/Engine/Binaries/Win64/verse_host.dll <engine>/Engine \
#       tests/verse_probe/effects_reject.verse
#
# What it is expected to say, verbatim as of 2026-09-13:
#
#   `Score`'s accessors contain the following errors:
#   Incorrect definitions:
#       ScoreGetter(:accessor)<reads>:int
#           - needs the <transacts> effect
#           - the signature of this accessor should be: ScoreGetter(:accessor)<transacts>:int
#
# That is S-1, and it is a refusal: the `<getter(...)>` protocol matches an exact signature rather
# than accepting a narrower effect, so the 3232 property getters cannot become `<reads>`. The
# `<setter(...)>` half is accepted at `<transacts>`, which is why only the getters are named.
#
# The `mirror_read` case below is the consequence, and the reason it is worth recording: with the
# getters pinned at `<transacts>`, a `<reads>` function may not read a Godot property, so a
# read-only helper narrowed by stage 1 can still call only *methods*.

effects_reject_accessors := class(object):

	var Score<public><getter(ScoreGetter)><setter(ScoreSetter)>:int
	ScoreGetter<epic_internal>(Accessor:accessor)<reads>:int = 3
	ScoreSetter<epic_internal>(Accessor:accessor, Value:int)<transacts>:void = {}

	var Where<public><getter(WhereGetter)><setter(WhereSetter)>:vector2
	WhereGetter<epic_internal>(Accessor:accessor)<reads>:vector2 = vector2{}
	WhereSetter<epic_internal>(Accessor:accessor, Value:vector2)<transacts>:void = {}
	WhereGetter<epic_internal>(Accessor:accessor, Field:string)<reads>:float =
		Current := vector2{}
		if (Field = "X") then Current.X else Current.Y
	WhereSetter<epic_internal>(Accessor:accessor, Field:string, Value:float)<transacts>:void = {}

# The narrowing direction, which must be refused for the lattice to mean anything: a `<reads>` body
# calling a `<transacts>` mirror method, and a `<reads>` body reading a mirror property.
effects_reject_narrowing := class(node2d):

	MirrorCall<public>()<reads>:string = GetName()

	MirrorRead<public>()<reads>:vector2 = Position
