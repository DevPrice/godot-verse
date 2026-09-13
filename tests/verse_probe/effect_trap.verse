using { /Godot.org/Godot }

# `dodge-the-creeps.md` wall 8, reduced. **Expected not to compile** -- it is here so the exact
# wording of the diagnostic the R-SCN-2 machinery recognises can be re-read rather than remembered.
#
#   bin/verse_probe.exe <engine>/Engine/Binaries/Win64/verse_host.dll <engine>/Engine \
#       tests/verse_probe/effect_trap.verse
#
# Two shapes, and the fix is in a different place for each:
#
#   - the author's own helper, written with no specifier, so it carries the *default* effect set --
#     which is wider than `<transacts>` because it contains `no_rollback`. The fix is on the
#     helper's declaration, which is not where the compiler points;
#   - a Godot method, which is `<transacts>` and genuinely is. The fix is on the caller.

effect_trap := class(node2d):

	# No specifier: the default set, which includes no_rollback.
	Helper():int = 7

	# Refused, and reported here rather than on Helper.
	CallsHelper()<transacts>:int = Helper()

	# The same trap through a failure context, which is the shape wall 8 actually hit: `if` forces
	# `<transacts>` on the body, and from there onto everything it calls.
	Guarded():int =
		if (X := Maybe[]) then X else 0

	Maybe()<decides>:int =
		Helper() > 0
		Helper()

	# The other direction: a `<reads>` caller reaching a `<transacts>` Godot method. Here the fix is
	# on the caller, because the callee is honest.
	ReadsGodot()<reads>:void = QueueFree()
