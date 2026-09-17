using { /Godot.org/Godot }

# Whether a subclass may declare a member whose name a base class already uses `<private>`.
#
#   bin/verse_probe.exe <engine>/Engine/Binaries/Win64/verse_host.dll <engine>/Engine \
#       tests/verse_probe/signal_shadow_probe.verse --class signal_shadow_probe
#
# This is the one declaration that dropping VH_SIGNAL_NOT_PUBLIC makes reachable. Signal binding is
# keyed by name -- FindShapeField walks the object's class chain for `(<class>:)<Name>` -- and
# GetClassSignalsLive walks the script chain base-first, so two same-named members would produce two
# descriptors of one name and Godot holds one signal per name. A public member cannot reach it:
# the compiler answers "Instance data member `X` is already defined in `base`, did you mean to add
# the `<override>` specifier?". Whether that refusal also covers a `<private>` base member is what
# this file asks, and the *text* of the refusal is its result -- a file that does not compile is the
# answer "no de-duplication pass is needed".
#
# Measured: it does not compile, and says so twice.
#
#   3593: definition Hidden cannot override an inaccessible parent definition
#   3532: Instance data member `(/user@localhost/signal_shadow_probe:)Hidden` is already defined in
#         `signal_shadow_base`, did you mean to add the <override> specifier?
#
# So the name-keyed binding cannot meet two members of one name, and dropping the access check
# needed no pass to make that true. Re-run this on an engine drop: 3593 is the refusal that is
# specifically about accessibility, and it is the one holding the guarantee up.

signal_shadow_base := class(object):

	@export_signal
	Hidden<private>:event(int) = event(int){}

signal_shadow_probe := class(signal_shadow_base):

	@export_signal
	Hidden<public>:event(int) = event(int){}

	Answer<public>():void = Print("signal_shadow_probe compiled")
