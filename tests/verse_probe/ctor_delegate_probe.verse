using { /Godot.org/Godot }

# R-NODE-3, second spike. `construct_probe.verse` established that `helper{}` on a mirrored base
# compiles and that writing the inherited `Handle` from user scope is glitch 3593 -- the field is
# internal to /Godot.org/Godot. This asks whether anything the *mirror* could declare would fill it
# on a construction the user writes, so that `helper{}` needs no new spelling.
#
#   bin/verse_probe.exe <engine>/Engine/Binaries/Win64/verse_host.dll <engine>/Engine \
#       tests/verse_probe/ctor_delegate_probe.verse --class ctor_delegate_probe
#
#   D1  a derived class's constructor setting an inherited field the user *can* reach -- baseline
#   D2  a derived constructor invoking a base constructor (delegation)
#   D3  a base class's field *default* evaluated by a derived archetype the user writes
#
# D3 is the one that would have decided it: if a base default runs on `derived{}`, then the mirror
# giving `vh_object.Handle` a computed default fills every construction without the user naming the
# field, and the internal access never arises.
#
# **What it answered.** D1 compiles. D2 does not -- `MakeBase(9)` in a constructor body is glitch
# **3552**, "Unsupported argument to archetype instantiation", so Verse has no constructor
# delegation and a user constructor cannot reach a base one that writes an internal field. D3 does
# not either, for an ordinary function: a data member's default may not be a call, glitch **3582**,
# "Divergent calls (calls that might not complete) cannot be used to define data-members".
#
# The two errors this file still reports are its result, the way `async_reject.verse`'s are.
#
# **What it left open, and why it was not taken *here*.** 3582 names *divergent* calls, and glitch
# 3565 below says `<converges>` is native-only -- so a `<converges><native>` default may well be
# accepted, which would make a bare `helper{}` mint its own Godot peer. It was not pursued for
# `vh_object` because the effect would be a lie in the direction that matters there: minting a Godot
# object is `<transacts>`, and a `<converges>` native claiming otherwise puts an object outside the
# transaction that created it, so a `helper{}` inside a failing computation would leak its peer.
# Phase 4.5 measured what a failure undoes; this would be the one construction it could not.
#
# **It is accepted, and a container is the case that reason does not reach.** `godot_array{}` mints
# its own empty Godot Array from `Ref<override>:int = VhRefNewDefault(TagArray)`, which is this
# spelling. What a dropped container leaks is a table entry, and a table entry is released by the
# collection that finds the Verse value unreachable -- which an aborted transaction produces --
# where a Verse-minted node is deliberately never freed. `ref_block_probe.verse` is the rest of
# that measurement, including the two refusals that ruled out doing it with a `block:` clause.

Minted<public>()<converges>:int = 77

probe_base<public> := class:
	Plain<public>:int = 0
	# The base default that D3 asks about: a call rather than a literal.
	Computed<public>:int = Minted()

probe_derived<public> := class(probe_base):
	Own<public>:int = 0

# D1 -- a constructor on the derived class writing the inherited (public) field.
MakeDerived<public><constructor>(Own:int)<transacts> := probe_derived:
	Own := Own
	Plain := 5

# D2 -- a second constructor that tries to delegate to the base rather than restate its fields.
MakeBase<public><constructor>(Plain:int)<transacts> := probe_base:
	Plain := Plain

MakeDelegating<public><constructor>(Own:int)<transacts> := probe_derived:
	MakeBase(9)
	Own := Own

ctor_delegate_probe := class(object):

	D1<public>()<transacts>:int =
		V := MakeDerived(3)
		V.Plain

	# D3 -- the archetype the user writes, reading the base's computed default.
	D3<public>()<transacts>:int =
		V := probe_derived{Own := 1}
		V.Computed

	Answer<public>()<transacts>:int = 42
