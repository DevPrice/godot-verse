using { /Godot.org/Godot }

# T1.6 (docs/web-vm/spec/objects.md): an object built while a module initializes, from a class
# that has a `var` member. The source says this raises at the InitializeVar op; this runs it.
#
#   bin/verse_probe.exe <engine>/Engine/Binaries/Win64/verse_host.dll <engine>/Engine \
#       tests/verse_probe/vm_objects_global_probe.verse --class vm_objects_global_probe
#
# What it answered: the module fails to evaluate (error 9003) after
#   ErrRuntime_UnimplementedGlobalVariable: Allocating a global var is not yet implemented.
#   (Can't allocate mutable var field (/user@localhost/glob_var:)N while initializing module.)
# raised from glob_var's constructor. Without <computes>/<allocates> both definitions are
# glitch 3512 at compile time instead.

glob_plain := class<computes>:
	N:int = 3

glob_var := class<allocates>:
	var N:int = 3

GPlain:glob_plain = glob_plain{}
GVar:glob_var = glob_var{}

vm_objects_global_probe := class(object):

	G1<public>()<transacts>:void =
		Print("G1 GPlain.N={GPlain.N}")

	G2<public>()<transacts>:void =
		Print("G2 GVar.N={GVar.N}")
