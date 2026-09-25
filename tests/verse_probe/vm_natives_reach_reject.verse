using { /Godot.org/Godot }
using { /Verse.org/Colors }
using { /Verse.org/SpatialMath }
using { /Verse.org/Simulation }

# T1.8: which Epic packages a script can name at all. Compiled for its diagnostics; every line
# below the class header is a question, and an "unknown identifier" is its answer.
#
#   bin/verse_probe.exe <engine>/Engine/Binaries/Win64/verse_host.dll <engine>/Engine \
#       <absolute path>/tests/verse_probe/vm_natives_reach_reject.verse --class vm_natives_reach_reject

vm_natives_reach_reject := class(object):

	# /Verse.org/Colors: the named colors and the hex constructor.
	C01<public>()<transacts>:void =
		X := NamedColors.White
		Y := MakeColorFromHex("#FF0000")

	# /Verse.org/Verse's own Print, which carries Duration and Color named parameters.
	C02<public>()<transacts>:void =
		Print("x", ?Duration := 1.0)

	# /Verse.org/SpatialMath: an opaque native struct and its constructor.
	C03<public>()<transacts>:void =
		R := IdentityRotation()
		V := vector3{Left := 1.0}

	# /Verse.org/Simulation: Sleep and the session type.
	C04<public>()<suspends>:void =
		Sleep(0.0)
