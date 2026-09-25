using { /Godot.org/Godot }

# spec/values.md §3: float arithmetic, signed zero, NaN, printing.

values_floats := class(object):

	FloatArith<public>():void =
		Print("Add={2.0 + 3.5} Sub={7.0 - 2.5} Mul={4.0 * 5.0} Div={1.0 / 4.0}")

	FloatDivByZero<public>():void =
		Zero := 0.0
		NegZero := -Zero
		Print("1/0={1.0 / Zero}")
		Print("1/-0={1.0 / NegZero}")
		Print("-1/-0={-1.0 / NegZero}")
		Print("0/-0={Zero / NegZero}")

	FloatSpecials<public>():void =
		Print("Inf-Inf={Inf - Inf}")
		Print("Inf*0={Inf * 0.0}")
		Print("-NaN={-NaN}")
		Print("Inf=Inf: {if (Inf = Inf) then "yes" else "no"}")
		Print("-Inf<-1e308: {if (-Inf < -1.0e308) then "yes" else "no"}")
		Print("NaN>Inf: {if (NaN > Inf) then "yes" else "no"}")

	FloatNegZero<public>():void =
		Zero := 0.0
		NegZero := -Zero
		Print("-0=0: {if (NegZero = Zero) then "yes" else "no"}")
		Print("-0<=0: {if (NegZero <= Zero) then "yes" else "no"}")
		Print("-0<0: {if (NegZero < Zero) then "yes" else "no"}")
		Print("-0 printed={NegZero}")

	FloatNaNComparisons<public>():void =
		Print("NaN<=NaN: {if (NaN <= NaN) then "yes" else "no"}")
		Print("NaN>=NaN: {if (NaN >= NaN) then "yes" else "no"}")
		Print("NaN<NaN: {if (NaN < NaN) then "yes" else "no"}")
		Print("NaN=NaN: {if (NaN = NaN) then "yes" else "no"}")
		Print("NaN<>NaN: {if (NaN <> NaN) then "yes" else "no"}")
		Print("NaN<=1.0: {if (NaN <= 1.0) then "yes" else "no"}")
		Print("NaN>=1.0: {if (NaN >= 1.0) then "yes" else "no"}")

	FloatPrintTable<public>():void =
		Print("1.0={1.0}")
		Print("0.1={0.1}")
		Print("0.5={0.5}")
		Print("2.5={2.5}")
		Print("1/3={1.0 / 3.0}")
		Print("1/128={1.0 / 128.0}")
		Print("3/128={3.0 / 128.0}")
		Print("5/128={5.0 / 128.0}")
		Print("0.1+0.2={0.1 + 0.2}")
		Print("123.4567895={123.4567895}")
		Print("999999.9999995={999999.9999995}")
		Print("1e15+0.3={1.0e15 + 0.3}")

	FloatPrintExtremes<public>():void =
		Print("1e20={1.0e20}")
		Print("1e22={1.0e22}")
		Print("1e23={1.0e23}")
		Print("1e300={1.0e300}")
		Print("max={1.7976931348623157e308}")
		Print("-max={-1.7976931348623157e308}")
		Print("smallest normal={2.2250738585072014e-308}")
		Print("smallest subnormal={4.9406564584124654e-324}")
		Print("2^53+1 literal={9007199254740993.0}")
		Print("Inf={Inf} -Inf={-Inf} NaN={NaN}")

	FloatToStringNoMethod<public>():void =
		Print("no float method form: relies on ToString/interpolation only")

	IntToFloat<public>():void =
		Print("3*1.5={3 * 1.5}")
		Print("1.5*3={1.5 * 3}")
		Print("9007199254740993*1.0={9007199254740993 * 1.0}")
		Print("0*-1.0={0 * -1.0}")

	FloatFloorCeilRoundInt<public>():void =
		if (I := Floor[0.5]) then Print("Floor[0.5]={I}") else Print("Floor[0.5] fails")
		if (I := Floor[-0.5]) then Print("Floor[-0.5]={I}") else Print("Floor[-0.5] fails")
		if (I := Ceil[0.5]) then Print("Ceil[0.5]={I}") else Print("Ceil[0.5] fails")
		if (I := Ceil[-0.5]) then Print("Ceil[-0.5]={I}") else Print("Ceil[-0.5] fails")
		if (I := Round[2.5]) then Print("Round[2.5]={I}") else Print("Round[2.5] fails")
		if (I := Round[-2.5]) then Print("Round[-2.5]={I}") else Print("Round[-2.5] fails")
		if (I := Round[0.5]) then Print("Round[0.5]={I}") else Print("Round[0.5] fails")
		if (I := Round[-0.5]) then Print("Round[-0.5]={I}") else Print("Round[-0.5] fails")
		if (I := Int[-2.7]) then Print("Int[-2.7]={I}") else Print("Int[-2.7] fails")
		if (I := Floor[Inf]) then Print("Floor[Inf]={I}") else Print("Floor[Inf] fails")
		if (I := Round[NaN]) then Print("Round[NaN]={I}") else Print("Round[NaN] fails")

	FloatToIntTooBigRaises<public>():void =
		if (I := Floor[1.0e19]) then Print("Floor[1e19]={I}") else Print("Floor[1e19] fails")
