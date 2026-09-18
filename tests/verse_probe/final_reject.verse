using { /Godot.org/Godot }

# The half of `final_probe.verse` whose answer is a refusal. **This file is expected not to
# compile**, and is deliberately not named after a class, so nothing tries to instantiate it.
#
#   bin/verse_probe.exe <engine>/Engine/Binaries/Win64/verse_host.dll <engine>/Engine \
#       tests/verse_probe/final_reject.verse
#
# Measured, verbatim as of 2026-09-17:
#
#   error 3569: Class `final_derived` cannot be a subclass of the class `final_base` which has the
#   `final` attribute.
#
# The glitch fires at the *subclass's* declaration, so a generated binding hierarchy made final
# would report the error against the generated file rather than against anything the author wrote.

final_base<public> := class<final>(object):
	Ask<public>():int = 7

final_derived<public> := class(final_base):
	Ask2<public>():int = 8
