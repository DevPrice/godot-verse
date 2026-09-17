using { /Godot.org/Godot }

# The half of `decides_virtual_probe.verse` whose answer is a refusal. **This file is expected not to
# compile** -- it is kept so the refusals that shaped the virtuals' spelling can be re-run rather
# than taken on trust, and it is deliberately not named after a class, so nothing tries to
# instantiate it.
#
#   bin/verse_probe.exe <engine>/Engine/Binaries/Win64/verse_host.dll <engine>/Engine \
#       tests/verse_probe/decides_virtual_reject.verse
#
# Read the probe file first. `<decides>` alone keeps the default effect set, which is what lets an
# override go on calling specifier-less helpers -- and the *same* fact, that it keeps `no_rollback`,
# is what cases 1 and 2 run into.
#
# Four cases, and **only three of them are refused**. Case 4 compiles, which is the finding.

# ---- 1. A `<decides>` function that kept `no_rollback` cannot be called from Verse -------------
#
# This is the price of the spelling the mirror uses, and it is not obvious: a `<decides>` function is
# only ever invoked from a failure context, and a failure context refuses `no_rollback` outright. So
# there is no context anywhere -- narrow or wide -- that can call one. Godot calls them through
# vh_instance_call, which is not a Verse failure context and does not care.
#
# Measured, verbatim as of 2026-09-16:
#
#   error 3512: This invocation calls a function
#   (`(/user@localhost/decides_reject_base:)Handled`) that has the 'no_rollback' effect, which is
#   not allowed by its context.

decides_reject_base := class(object):

	Handled<public>(Value:int)<decides>:void = false?

decides_reject_caller := class(decides_reject_base):

	Ask<public>():void =
		if (Handled[1]):
			Print("handled")

# ---- 2. The alternative spelling, and why it was not taken ------------------------------------
#
# `<decides><transacts>` buys case 1 back: `transacts`'s descriptor rescinds
# `reads | writes | allocates | no_rollback`, so the function loses `no_rollback` and a failure
# context will admit it. What it costs is the thing the specifier-less declarations exist to avoid
# -- the override may no longer call a specifier-less helper, which is `docs/dodge-the-creeps.md`
# wall 8, reported at the *call* rather than at the declaration that needs changing:
#
#   error 3512: This invocation calls a function (`(/user@localhost/decides_narrow_probe:)Bump`)
#   that has the 'no_rollback' effect, which is not allowed by its context.
#
# One line of cascade per helper, per file, for every script that overrides a bool virtual. That is
# the trade, and it is why the mirror takes `<decides>` alone and gives up the Verse-side call.

decides_narrow_base := class(object):

	Handled<public>(Value:int)<decides><transacts>:void = false?

decides_narrow_probe := class(decides_narrow_base):

	var Solid<public>:logic = true

	var Seen<public>:int = 0

	Bump():void =
		set Seen = Seen + 1

	Handled<override>(Value:int)<decides><transacts>:void =
		Bump()
		Solid?

# ---- 3. What an author with an unmigrated override sees ---------------------------------------
#
# Three errors for one declaration, and **none of them names the effect**:
#
#   error 3532: Function (/user@localhost/decides_reject_logic:)Handled must have a distinct domain
#   from these other functions with the same name: ...
#   error 3523: Function ...Handled has an <override> attribute, but could not find a parent
#   function to override (perhaps the parent function's access specifiers are too restrictive?).
#   error 3532: The function ...Handled(:int) is ambiguous with this definition: ...
#
# The same shape CLAUDE.md records for `_Ready<override>()<suspends>` -- the specifier makes it a
# different function, and 3523 blames the access specifiers. It is at least *loud*: three errors at
# the declaration that needs changing, not at some call site a file away.

decides_reject_logic := class(decides_reject_base):

	Handled<override>(Value:int):logic = false

# ---- 4. The one that is NOT refused, and the hazard in it -------------------------------------
#
# An override may declare **fewer** effects than the base: effects are contravariant, so a total
# override satisfies a `<decides>` declaration -- it simply never exercises the failure. This
# compiles clean, with no diagnostic of any kind.
#
# For a Godot bool virtual that means "always true". An author migrating `:logic = false` who fixes
# the return type but drops the specifier gets a virtual that answers *handled, every time*, where
# the old body answered "not handled". It is not silent in the source -- `{}` plainly says "do
# nothing and succeed" -- but it is silent in the compiler, and it inverts the answer.

decides_reject_effect := class(decides_reject_base):

	Handled<override>(Value:int):void = {}
