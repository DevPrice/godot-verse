using { /Godot.org/Godot }

# R-SIG-1's access rule, asked of the analysis half: which access levels reach the signal list, and
# what `DerivedAccessLevel()` answers for a member that carries no specifier at all.
#
#   bin/verse_probe.exe <engine>/Engine/Binaries/Win64/verse_host.dll <engine>/Engine \
#       tests/verse_probe/signal_access_probe.verse --class signal_access_probe
#
# The bridge used to refuse every member here but `Open` with VH_SIGNAL_NOT_PUBLIC (2), on the
# reading that connecting is done from outside the class. It is not done from Verse at all -- a
# designer connects in the Node panel and GDScript connects by string name, and neither consults a
# Verse specifier -- so the check was dropped and every member below answers reject=0.
#
# Measured before the change, which is what it is kept for: `Open` read 0 and the other four read 2.
# So all three of `<protected>`, `<private>` and no specifier at all derive to something other than
# Public, and the last of those is the whole reason an author had to write `<public>` on a signal.

signal_access_probe := class(object):

	@export_signal
	Open<public>:event(int) = event(int){}

	@export_signal
	Inherited<protected>:event(int) = event(int){}

	@export_signal
	Mine<private>:event(int) = event(int){}

	@export_signal
	Unspecified:event(int) = event(int){}

	# The `signal(t)` spelling takes the same path, and its `Id` field is written by the host rather
	# than read by Verse, so there is no access question on the write itself.
	@export_signal
	Legacy<private>:signal(int) = signal(int){}

	Answer<public>():void = Print("signal_access_probe compiled")
