using { /Godot.org/Godot }

# T1.2 (docs/phase-7.5-design.md 6.1): three facts a spec line needs settled before it is written,
# run rather than argued, plus the adjacent float/int facts a value-semantics spec needs. Every
# method below is public, takes no argument and does not suspend, so the probe driver
# (tests/verse_probe/verse_probe.cpp) calls each automatically, in declaration order, and every
# line it prints is the record. The readings and the exact command are in docs/web-vm/facts.md.
#
#   bin/verse_probe.exe <engine>/Engine/Binaries/Win64/verse_host.dll <engine>/Engine \
#       tests/verse_probe/vm_facts_probe.verse --class vm_facts_probe
#
# Run it from bin/, where build_host.py leaves tbbmalloc.dll.

vm_facts_probe := class(object):

	# --- 1. NaN under all six comparison operators, against itself and against 1.0 -----------------
	ReportNanComparisons<public>()<transacts>:void =
		Zero := 0.0
		Nan := Zero / Zero
		One := 1.0
		Print("NaN <= NaN: {if (Nan <= Nan) then "succeeds" else "fails"}")
		Print("NaN >= NaN: {if (Nan >= Nan) then "succeeds" else "fails"}")
		Print("NaN <  NaN: {if (Nan < Nan) then "succeeds" else "fails"}")
		Print("NaN >  NaN: {if (Nan > Nan) then "succeeds" else "fails"}")
		Print("NaN =  NaN: {if (Nan = Nan) then "succeeds" else "fails"}")
		Print("NaN <> NaN: {if (Nan <> Nan) then "succeeds" else "fails"}")
		Print("NaN <= 1.0: {if (Nan <= One) then "succeeds" else "fails"}")
		Print("NaN >= 1.0: {if (Nan >= One) then "succeeds" else "fails"}")
		Print("NaN <  1.0: {if (Nan < One) then "succeeds" else "fails"}")
		Print("NaN >  1.0: {if (Nan > One) then "succeeds" else "fails"}")
		Print("NaN =  1.0: {if (Nan = One) then "succeeds" else "fails"}")
		Print("NaN <> 1.0: {if (Nan <> One) then "succeeds" else "fails"}")

	# --- 2. Integer Mod[] and Quotient[] on all four sign combinations, and by zero -----------------
	ReportModQuotient<public>()<transacts>:void =
		if (M := Mod[7, 2]) then Print("Mod[7, 2] = {M}") else Print("Mod[7, 2] fails")
		if (M := Mod[-7, 2]) then Print("Mod[-7, 2] = {M}") else Print("Mod[-7, 2] fails")
		if (M := Mod[7, -2]) then Print("Mod[7, -2] = {M}") else Print("Mod[7, -2] fails")
		if (M := Mod[-7, -2]) then Print("Mod[-7, -2] = {M}") else Print("Mod[-7, -2] fails")
		if (M := Mod[7, 0]) then Print("Mod[7, 0] = {M}") else Print("Mod[7, 0] fails")
		if (Q := Quotient[7, 2]) then Print("Quotient[7, 2] = {Q}") else Print("Quotient[7, 2] fails")
		if (Q := Quotient[-7, 2]) then Print("Quotient[-7, 2] = {Q}") else Print("Quotient[-7, 2] fails")
		if (Q := Quotient[7, -2]) then Print("Quotient[7, -2] = {Q}") else Print("Quotient[7, -2] fails")
		if (Q := Quotient[-7, -2]) then Print("Quotient[-7, -2] = {Q}") else Print("Quotient[-7, -2] fails")
		if (Q := Quotient[7, 0]) then Print("Quotient[7, 0] = {Q}") else Print("Quotient[7, 0] fails")

	# --- 3. Float printing: ToString(X), the free function (`X.ToString()` is refused: "Unknown
	# member `ToString` in `float`" -- unlike a user class, float has no extension-method form),
	# against "{X}" interpolation. ------------------------------------------------------------------
	ReportFloatToString<public>()<transacts>:void =
		ReportOneFloat(1.0, "1.0")
		ReportOneFloat(0.1, "0.1")
		ReportOneFloat(1.5, "1.5")
		ReportOneFloat(-0.0, "-0.0")
		ReportOneFloat(1.0e20, "1.0e20")
		ReportOneFloat(123456789.125, "123456789.125")
		ReportOneFloat(1.0 / 3.0, "1.0/3.0")
		ReportOneFloat(1.0 / 0.0, "Inf")
		ReportOneFloat(-1.0 / 0.0, "-Inf")
		ReportOneFloat(0.0 / 0.0, "NaN")

	ReportOneFloat(Value:float, Label:string)<transacts>:void =
		Print("{Label}: ToString() = {ToString(Value)}, interpolation = {Value}")

	# --- 4. Int printing, and arithmetic crossing 2^63 --------------------------------------------
	# Kept as three separate zero-argument methods rather than one: a runtime error rolls back
	# every deferred write a `<transacts>` call made, Print's output included -- so a later failing
	# line in the same call would have silently erased the Prints that came before it in that same
	# call. Splitting means the driver's tick between top-level calls is what separates a working
	# read from a failing one.
	ReportIntBasic<public>()<transacts>:void =
		ReportOneInt(0, "0")
		ReportOneInt(-5, "-5")
		ReportOneInt(4611686018427387904, "2^62")
		ReportOneInt(9223372036854775807, "2^63-1 (int64 max)")

	ReportOneInt(Value:int, Label:string)<transacts>:void =
		Print("{Label}: ToString() = {ToString(Value)}, interpolation = {Value}")

	# Does the arithmetic itself overflow/wrap at 2^63, judged with comparisons alone (no
	# ToString/interpolation, which is where ReportIntOverflowToString below finds the limit).
	ReportIntOverflowCompare<public>()<transacts>:void =
		Overflowed := 9223372036854775807 + 1
		Print("2^63-1 + 1 > int64 max: {if (Overflowed > 9223372036854775807) then "succeeds" else "fails"}")
		Print("2^63-1 + 1 < 0: {if (Overflowed < 0) then "succeeds" else "fails"}")
		Print("2^63-1 + 1 = int64 max: {if (Overflowed = 9223372036854775807) then "succeeds" else "fails"}")
		Doubled := 9223372036854775807 + 9223372036854775807
		Print("(2^63-1)*2 > int64 max: {if (Doubled > 9223372036854775807) then "succeeds" else "fails"}")

	# ToString/interpolation of the same overflowed value, isolated so a runtime error here does
	# not roll back ReportIntOverflowCompare's already-printed lines above (they are a separate
	# top-level call).
	ReportIntOverflowToString<public>()<transacts>:void =
		Overflowed := 9223372036854775807 + 1
		Print("2^63-1 + 1 interpolated: {Overflowed}")

	# A 2^70 literal (1180591620717411303424) was tried here and refused at compile time: error
	# 3555, "Integer literal must be in the range -9223372036854775808 to 9223372036854775807" --
	# an int literal is capped at the int64 range regardless of what int arithmetic itself does at
	# that boundary (ReportIntOverflowCompare/ReportIntOverflowToString, above).

	# --- 5. Adjacent facts: -0.0, and Floor/Ceil/Round on halves and negatives ----------------------
	ReportRounding<public>()<transacts>:void =
		Print("-0.0 =  0.0: {if (-0.0 = 0.0) then "succeeds" else "fails"}")
		Print("-0.0 <= 0.0: {if (-0.0 <= 0.0) then "succeeds" else "fails"}")
		Print("-0.0 <  0.0: {if (-0.0 < 0.0) then "succeeds" else "fails"}")
		if (R := Round[2.5]) then Print("Round[2.5] = {R}") else Print("Round[2.5] fails")
		if (R := Round[-2.5]) then Print("Round[-2.5] = {R}") else Print("Round[-2.5] fails")
		if (R := Round[0.5]) then Print("Round[0.5] = {R}") else Print("Round[0.5] fails")
		if (R := Round[-0.5]) then Print("Round[-0.5] = {R}") else Print("Round[-0.5] fails")
		if (F := Floor[-0.5]) then Print("Floor[-0.5] = {F}") else Print("Floor[-0.5] fails")
		if (F := Floor[0.5]) then Print("Floor[0.5] = {F}") else Print("Floor[0.5] fails")
		if (C := Ceil[-0.5]) then Print("Ceil[-0.5] = {C}") else Print("Ceil[-0.5] fails")
		if (C := Ceil[0.5]) then Print("Ceil[0.5] = {C}") else Print("Ceil[0.5] fails")
