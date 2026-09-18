using { /Godot.org/Godot }

# How `godot_array{}` came to be an empty Godot Array rather than a reference of 0.
#
#   bin/verse_probe.exe <engine>/Engine/Binaries/Win64/verse_host.dll <engine>/Engine \
#       tests/verse_probe/ref_block_probe.verse --class ref_block_probe
#
# `class_block_probe.verse` settled that a `block:` clause runs per instance, runs for a derived
# class the archetype names, and sees a field the archetype supplied. So the obvious shape for a
# container that mints when it names nothing is a block on `godot_array` that writes `Ref`.
#
# **It cannot be written, and the two refusals are this file's first result.** Measured with the
# first draft of this fixture -- a `<computes>` base holding `var Id`, and a `<reads>` derived class
# whose block wrote it:
#
#   3512  "This mutable data definition has the 'allocates' effect, which is not allowed by its
#         context."   -- a `var` member forces `allocates`, which `<computes>` does not carry, so
#         `godot_ref.Ref` cannot become a `var` while the class stays `<computes>`.
#   3512  "This assignment has the 'transacts' effect, which is not allowed by its context."
#         -- `set` is `transacts` outright. A `<reads>` class can never write a member from its
#         block, so a container with a minting block is a `transacts` class.
#
# The second is what prices the block clause out. `godot_ref` is `<computes>` on purpose: every
# wrapper is built by a `<reads>` converter, and **379 mirror methods answer a container from a
# `<reads>` signature** against the 144 that are `<transacts>` already. A `transacts` container drags
# every one of those across, which is wall 8's cascade at 379 call sites.
#
# So this file asks the other shape, the one `ctor_delegate_probe.verse` left open when D3 refused
# an ordinary call in a data-member default with glitch 3582, *"Divergent calls (calls that might not
# complete) cannot be used to define data-members"*: a **default** rather than a block. A default
# needs no `var` and writes nothing, and 3582 bans only a *divergent* call -- `<converges>` being
# native-only (3565) is what makes this the mirror's to spell and not a script's.
#
#   O1  may a derived class override an inherited data member's default at all?
#   O2  does an archetype that supplies the field still win over the overridden default -- which is
#       what stops `VhToArray` allocating a throwaway Array on every crossing?
#   O3  may the base stay `<computes>` while the derived class widens?
#   O4  may a block clause that only *calls* be narrower than `transacts`?
#
# **What it answered: yes to all four.** Minted = 9, Adopted = 5, both from a `<reads>` caller.
#
#   O1/O3  `Id<override>:int = 9` on a `<reads>` class deriving a `<computes>` one compiles, so the
#          mirror's `Ref<override>:int = VhRefNewDefault(TagArray)` has somewhere to live and
#          `godot_ref` keeps `<computes>` for `callable` and `signal_ref`.
#   O2     the supplied value wins: `probe_ref_array{Id := 5}` never evaluates the default. That is
#          what makes `VhToArray` free -- a converter names the `Ref` it was handed and pays for no
#          mint.
#   O4     a block calling a `<reads>` function in a `<reads>` class compiles, which is how adoption
#          happens for a container no converter ever saw.
#
# The last question this file cannot ask is whether the *native* spelling is accepted, because a
# user package may not declare one: `<converges>` is native-only. **The mirror answers it** -- the
# host built, `example.verse` compiled against it, and `tests/integration` asserts the behaviour
# through Godot, where a reference of 0 arrives as null.

# Stands in for `VhAdoptRef`: a `<reads>` call that takes the object and answers nothing, which is
# the whole of what adoption is.
Adopt<public>(Value:probe_ref)<reads>:void = {}

probe_ref<public> := class<computes>:
	Id<public>:int = 0

probe_ref_array<public> := class<reads>(probe_ref):
	# The inherited default, restated. A literal here stands in for the `VhRefNewDefault(TagArray)`
	# the mirror writes, which a probe cannot spell.
	Id<override>:int = 9

	# A block that adopts and writes nothing. `set` is `transacts` and that is what prices the
	# minting block out; a bare call is only as wide as the call.
	block:
		Adopt(Self)

ref_block_probe := class(object):

	# O1/O3/O4 -- nothing supplied, so the overridden default is what lands.
	Minted<public>()<reads>:int =
		V := probe_ref_array{}
		V.Id

	# O2 -- the archetype named something. 5 means the supplied value wins; 9 would mean every
	# crossing pays for a mint it immediately overwrites.
	Adopted<public>()<reads>:int =
		V := probe_ref_array{Id := 5}
		V.Id

	Answer<public>()<transacts>:int = 42
