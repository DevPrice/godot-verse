using { /Godot.org/Godot }

# T1.6 (docs/web-vm/spec/objects.md): a native-represented field holds a 64-bit integer, and an
# archetype that stores a wider one raises. Kept apart from vm_objects_probe.verse because at
# engine commit 203d764 the raise is followed by a fatal assertion in the editor host
# ("Interpreter.EffectToken.Get(Context).IsPlaceholder()"), which ends the process.
#
#   bin/verse_probe.exe <engine>/Engine/Binaries/Win64/verse_host.dll <engine>/Engine \
#       tests/verse_probe/vm_objects_wideint_probe.verse --class vm_objects_wideint_probe
#
# What it printed: "ErrRuntime_GeneratedNativeInternal: An internal runtime error occurred in
# (generated) native code that was called from Verse. There is no other information available.
# (Value exceeds the range of a 64 bit integer.)", then the fatal assertion. The raise carries no
# Verse call stack in the log; the only native conversion on the path is the store into I0.

vm_objects_wideint_probe := class(object):

	W1<public>()<transacts>:void =
		var Big:int = 1
		for (I := 1..70):
			set Big = Big * 2
		Print("W1 building")
		V := VariantInt(Big)
		Print("W1 built, reads back {V.AsInt[] or -1}")
