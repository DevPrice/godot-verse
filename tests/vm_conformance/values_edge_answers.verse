using { /Godot.org/Godot }

# Answers the lead asked for directly from the reference, rather than guessing:
# (1) chained comparison; (2) function value vs array equality; (3) char32 beyond U+10FFFF and a
# surrogate; (4) Mod/Quotient with both a 64-bit-range violation and a zero divisor at once.

values_edge_answers := class(object):

	ChainedComparison<public>():void =
		Print("1<2<2: {if (1 < 2 < 2) then "yes" else "no"}")
		Print("1<2<3: {if (1 < 2 < 3) then "yes" else "no"}")

	# FunctionVsArrayEquality is not a fixture: `F = array{}` and `array{} = F` (F a function value)
	# are both refused at compile time -- error 3509, "This function parameter expects a value of
	# type tuple(comparable,comparable), but this argument is an incompatible value of type
	# tuple(int->int,[]false)" (and the mirror image for the reversed order). `=` is declared only
	# for `comparable`, a function type never satisfies it, and the reference's "Undecidable" answer
	# for a function cell (values.md §11.2 rule 7's last row) is therefore reached by no compiled
	# Godot script, on either side of the comparison.

	# Char32AboveMax and Char32Surrogate are not fixtures: both literal spellings are refused at
	# compile time (see the task report), so there is nothing to run. 0u110000 fails to lex at all
	# (error 3100, vErr:S77 "Unexpected '0' following expression" -- it does not even reach a
	# char32-literal diagnostic); 0uD800 lexes but is refused by name (error 3110, "Character
	# literals must be Unicode scalars, which exclude the surrogate code points
	# 0uD800..0uDFFF"). No script spelling reaches natives.md §11.1 Q5's case.

	ModRangeVsZeroDivisor<public>():void =
		Big := 9223372036854775807 + 1
		if (M := Mod[Big, 0]) then Print("Mod[2^63,0]={M}") else Print("Mod[2^63,0] fails")

	QuotientRangeVsZeroDivisor<public>():void =
		Big := 9223372036854775807 + 1
		if (Q := Quotient[Big, 0]) then Print("Quotient[2^63,0]={Q}") else Print("Quotient[2^63,0] fails")
