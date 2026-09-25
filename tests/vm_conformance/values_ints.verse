using { /Godot.org/Godot }

# spec/values.md §2: integer range, overflow, printing, division and rationals.
# One behaviour per method; a method that raises is on its own so a raise's rollback (facts.md,
# "a raise erases the whole call's Prints") does not swallow an earlier line.

values_ints := class(object):

	IntArith<public>():void =
		Print("Add={2 + 3} Sub={7 - 2} Mul={4 * 5} Neg={0 - 7}")

	IntOverflowNoWrap<public>():void =
		Big := 9223372036854775807 + 1
		Print("gt max: {if (Big > 9223372036854775807) then "yes" else "no"}")
		Print("lt zero: {if (Big < 0) then "yes" else "no"}")
		Sum := 9223372036854775807 + 9223372036854775807
		Print("sum gt max: {if (Sum > 9223372036854775807) then "yes" else "no"}")

	IntHeapRoundTrip<public>():void =
		Big := 9223372036854775807 + 1
		Print("(2^63-1)+1-1={Big - 1}")

	IntMinLiteral<public>():void =
		Print("min={-9223372036854775808}")

	IntNegOfMin<public>():void =
		Lo := -9223372036854775807 - 1
		Print("-(min)={-Lo}")

	IntBelowMinPrints<public>():void =
		Lo := -9223372036854775807 - 1
		Print("min-1={Lo - 1}")

	IntOverflowToStringRaises<public>():void =
		Big := 9223372036854775807 + 1
		Print("never printed: {Big}")

	IntBigMultiplyExceeds<public>():void =
		Sq := 4294967296 * 4294967296
		Print("2^64 gt max: {if (Sq > 9223372036854775807) then "yes" else "no"}")
		Print("2^64/2^32 back: {if (R := Sq / 4294967296) then Floor(R) else -1}")

	IntRationalFloorCeil<public>():void =
		if (R := 7 / 2) then Print("7/2 floor={Floor(R)} ceil={Ceil(R)}")
		if (R := -7 / 2) then Print("-7/2 floor={Floor(R)} ceil={Ceil(R)}")
		if (R := 7 / -2) then Print("7/-2 floor={Floor(R)} ceil={Ceil(R)}")
		if (R := -7 / -2) then Print("-7/-2 floor={Floor(R)} ceil={Ceil(R)}")
		if (R := 6 / 3) then Print("6/3 floor={Floor(R)} ceil={Ceil(R)}")
		Print("Floor(5)={Floor(5)} Ceil(-5)={Ceil(-5)}")

	IntRationalEquality<public>():void =
		if (A := -1 / -2, B := 1 / 2):
			Print("-1/-2 = 1/2: {if (A = B) then "yes" else "no"}")
		if (C := 14 / 4, D := 7 / 2):
			Print("14/4 = 7/2: {if (C = D) then "yes" else "no"}")

	IntDivByZeroFails<public>():void =
		if (Q := 7 / 0) then Print("got {Floor(Q)}") else Print("7/0 fails")
		if (Q := 0 / 0) then Print("got {Floor(Q)}") else Print("0/0 fails")
