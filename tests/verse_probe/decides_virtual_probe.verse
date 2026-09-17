using { /Godot.org/Godot }

# Whether a Godot `bool` virtual can become `<decides>:void`, asked of the compiler rather than of
# Effects.h:
#
#   bin/verse_probe.exe <engine>/Engine/Binaries/Win64/verse_host.dll <engine>/Engine \
#       tests/verse_probe/decides_virtual_probe.verse --class decides_virtual_probe
#
# The mirror declares 160 bool virtuals as `:logic = false`, and CLAUDE.md records that they carry
# **no effect specifier on purpose**: the default set is wider than `<transacts>` -- it contains
# `no_rollback` -- so an overriding body may call whatever it likes, where narrowing the declaration
# would refuse a body that called a specifier-less helper, one file at a time. That is
# `docs/dodge-the-creeps.md` wall 8, and it is what had to be measured before moving them.
#
# **`<decides>` written alone does not narrow.** `ConvertEffectClassesToEffectSet` does not replace
# the default with the specifier: it starts from the default, rescinds what the specifier's
# descriptor names, then unions the specifier's own bits. `decides`'s descriptor is
#
#     _EffectDescriptorTable.Insert(_decidesClass, { EffectSets::Decides, EEffect::decides, {} });
#
# -- it rescinds only `decides`, which the default does not have, and its mutual-exclusion list is
# empty. So `<decides>` alone is `FunctionDefault | decides`: the wide set kept, one bit added.
# `Bump` below is the proof, and it is the same shape as `hooks_probe.verse` finding 4 -- it has no
# specifier and it writes, so under a narrowed declaration the call to it would be glitch 3512 at the
# call site. A clean compile is the answer.
#
# The price is in `decides_virtual_reject.verse` and it is real: keeping `no_rollback` makes these
# functions **uncallable from Verse**, because the only context that admits a failure refuses
# `no_rollback`. That is the same position the five specifier-less hooks are already in -- Godot is
# the caller, through vh_instance_call rather than through a Verse failure context -- and the
# alternative spelling, `<decides><transacts>`, buys the Verse-side call at the price of the cascade.
# That trade is measured in the reject file; this one is the spelling chosen.
#
# `AlwaysDeclines` and `AlwaysSucceeds` take no arguments, so the probe calls them: they are here to
# put a `no_rollback`-and-`decides` function through the *host's* invoke path, which is the one that
# matters and which no amount of reading the effect table answers.

decides_virtual_base := class(object):

	# What the mirror's declaration would become. `false?` is the default body: an unoverridden Godot
	# virtual answers "not handled", and for a failable one that is a failure rather than a value.
	Handled<public>(Value:int)<decides>:void = false?

# A class that does not override it, so the default body is what a script leaving the virtual alone
# gets.
decides_virtual_inherits := class(decides_virtual_base):

	Answer<public>():int = 7

decides_virtual_probe := class(decides_virtual_base):

	var Solid<public>:logic = true

	var Seen<public>:int = 0

	# No specifier, exactly as a helper an author writes alongside an override has none.
	Bump():void =
		set Seen = Seen + 1

	# The override. It repeats `<decides>`, calls the specifier-less helper, and answers through the
	# query operator -- which is what the generated body `VhToLogic(...)?` already does for the 568
	# mirrored predicates.
	Handled<override>(Value:int)<decides>:void =
		Bump()
		Solid?

	AlwaysDeclines<public>()<decides>:void = false?

	AlwaysSucceeds<public>()<decides>:void = true?

	Answer<public>():int = Seen
