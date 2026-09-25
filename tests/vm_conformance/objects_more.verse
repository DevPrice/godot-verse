using { /Godot.org/Godot }

# spec/objects.md §13 casts, §11 interfaces, §16 accessor members, §9.2 native fields, §10.2 struct
# equality, and calls.md §7.4's (super:). A native int field's out-of-range store is a unit case in
# tests/vm, not a method here: the UE runtime host raises it and then asserts, taking the probe down.

MoreSide(Label:string, Value:int)<transacts>:int =
	Print("  eval {Label}")
	Value

more_base := class:
	Describe()<transacts>:string = "base"
	Greet(Who:string)<transacts>:string = "base greets {Who}"

more_mid := class(more_base):
	Greet<override>(Who:string)<transacts>:string = "mid then {(super:)Greet(Who)}"

more_leaf := class(more_mid):
	Describe<override>()<transacts>:string = "leaf then {(super:)Describe()}"
	Greet<override>(Who:string)<transacts>:string = "leaf then {(super:)Greet(Who)}"

more_named := interface:
	Label:string = "named"
	Title()<transacts>:string
	Name()<transacts>:string = "named.Name({Label})"

more_sized := interface(more_named):
	Size:int = 3

more_thing := class(more_base, more_sized):
	Title<override>()<transacts>:string = "thing Size={Size}"

more_other := class(more_sized):
	Label<override>:string = "other"
	Title<override>()<transacts>:string = "other Size={Size}"
	Name<override>()<transacts>:string = "other then {(super:)Name()}"

more_prop := class:
	var Stored:int = 0
	Other:int = 0
	var Level<getter(LevelGetter)><setter(LevelSetter)>:int = external{}
	LevelGetter(Accessor:accessor)<transacts>:int =
		Print("  get Level -> {Stored}")
		Stored
	LevelSetter(Accessor:accessor, Value:int)<transacts>:void =
		Print("  set Level <- {Value}")
		set Stored = Value
	block:
		Print("  prop block Stored={Stored} Other={Other}")

more_prop_child := class(more_prop):
	Extra:int = 4
	block:
		Print("  child block Stored={Stored}")

more_pair := struct:
	A:int = 1
	B:string = "b"
	C:float = 0.5

more_holder := struct:
	Pair:more_pair = more_pair{}
	N:int = 0

more_percent := type{_X:float where 0.0 <= _X, _X <= 1.0}
more_digit := type{_X:int where 0 <= _X, _X <= 9}
more_natural := type{_X:int where 0 <= _X}

objects_more := class(object):

	ClassCasts<public>():void =
		B:more_base = more_leaf{}
		if (L := more_leaf[B]) then Print("leaf[leaf] ok") else Print("leaf[leaf] fails")
		if (M := more_mid[B]) then Print("mid[leaf] ok") else Print("mid[leaf] fails")
		P:more_base = more_mid{}
		if (L := more_leaf[P]) then Print("leaf[mid] ok") else Print("leaf[mid] fails")
		if (T := more_thing[B]) then Print("thing[leaf] ok") else Print("thing[leaf] fails")

	InterfaceCasts<public>():void =
		B:more_base = more_thing{}
		if (N := more_named[B]) then Print("named[thing] ok: {N.Title()}") else Print("named[thing] fails")
		if (S := more_sized[B]) then Print("sized[thing] ok: {S.Size}") else Print("sized[thing] fails")
		L:more_base = more_leaf{}
		if (N := more_named[L]) then Print("named[leaf] ok") else Print("named[leaf] fails")
		O:more_named = more_other{}
		if (S := more_sized[O]) then Print("sized[other] ok: {S.Name()}") else Print("sized[other] fails")
		if (T := more_thing[O]) then Print("thing[other] ok") else Print("thing[other] fails")

	CastInFullContext<public>():void =
		Things:[]more_base = array{more_leaf{}, more_thing{}, more_mid{}, more_base{}}
		for (Index -> Thing : Things):
			Result := if (X := more_mid[Thing], Y := more_base[X]) then "mid" else "not mid"
			Print("{Index}: {Result}")

	FloatCasts<public>():void =
		for (V : array{0.5, 0.0, -0.0, 1.0, 2.0, -1.0, 1.0 / 0.0, 0.0 / 0.0}):
			if (P := more_percent[V]) then Print("percent[{V}] ok") else Print("percent[{V}] fails")

	IntCasts<public>():void =
		for (V : array{-1, 0, 9, 10}):
			if (D := more_digit[V]) then Print("digit[{V}] ok") else Print("digit[{V}] fails")
		Big := 1073741824 * 1073741824 * 1024
		if (N := more_natural[Big]) then Print("natural[2^70] ok") else Print("natural[2^70] fails")
		if (N := more_natural[-Big]) then Print("natural[-2^70] ok") else Print("natural[-2^70] fails")
		if (D := more_digit[Big]) then Print("digit[2^70] ok") else Print("digit[2^70] fails")

	InterfaceMembers<public>():void =
		T := more_thing{}
		Print("thing: Label={T.Label} Size={T.Size} Name={T.Name()} Title={T.Title()} Describe={T.Describe()}")
		O := more_other{}
		Print("other: Label={O.Label} Size={O.Size} Name={O.Name()} Title={O.Title()}")
		N:more_named = O
		Print("as named: {N.Name()} {N.Title()}")

	SuperCalls<public>():void =
		L := more_leaf{}
		Print(L.Describe())
		Print(L.Greet("you"))
		M:more_base = L
		Print(M.Greet("again"))
		F := L.Greet
		Print(F("later"))

	AccessorReadWrite<public>():void =
		P := more_prop{}
		Print("read Level={P.Level}")
		set P.Level = 5
		Print("after set Level={P.Level} Stored={P.Stored}")
		set P.Level += 2
		Print("after += Level={P.Level}")

	AccessorDeferredSetter<public>():void =
		Print("archetype start")
		P := more_prop{Level := MoreSide("Level", 9), Other := MoreSide("Other", 1)}
		Print("Stored={P.Stored} Other={P.Other}")
		Print("child start")
		C := more_prop_child{Level := MoreSide("child Level", 7), Extra := MoreSide("child Extra", 5)}
		Print("child Stored={C.Stored} Extra={C.Extra}")

	StructEquality<public>():void =
		P := more_pair{}
		Q := more_pair{A := 1}
		R := more_pair{A := 1, B := "b", C := 0.5}
		S := more_pair{B := "c"}
		Print("default=partial: {if (P = Q) then "yes" else "no"} default=full: {if (P = R) then "yes" else "no"} default=other: {if (P = S) then "yes" else "no"}")
		H := more_holder{}
		G := more_holder{Pair := more_pair{C := 0.5}}
		K := more_holder{Pair := more_pair{C := -0.5}}
		Print("nested equal: {if (H = G) then "yes" else "no"} nested differ: {if (H = K) then "yes" else "no"}")
		Z := more_pair{C := -0.0}
		Y := more_pair{C := 0.0}
		Print("signed zero fields: {if (Z = Y) then "yes" else "no"}")
		Keyed:[more_pair]int = map{P => 1, S => 2}
		if (V := Keyed[R]) then Print("map by struct key: {V}") else Print("map by struct key fails")

	NativeFields<public>():void =
		V := VariantInt(4611686018427387904)
		if (I := V.AsInt[]) then Print("I0 round trip {I}") else Print("AsInt fails")
		Low := VariantInt(-9223372036854775807 - 1)
		if (I := Low.AsInt[]) then Print("I0 min {I}") else Print("AsInt fails")
		F := VariantFloat(-2.5)
		if (X := F.AsFloat[]) then Print("F0 {X}") else Print("AsFloat fails")
		S := VariantString("text")
		if (X := S.AsString[]) then Print("Text {X}") else Print("AsString fails")
		Print("variant equality: {if (VariantInt(3) = VariantInt(3)) then "yes" else "no"} {if (VariantInt(3) = VariantInt(4)) then "yes" else "no"}")
