using { /Godot.org/Godot }

# spec/objects.md: the layout rule (override, archetype-supplied fields), construction order,
# blocks, casts, interfaces, structs, enums, <unique> equality.

ObjectsSide(Label:string, Value:int)<transacts>:int =
	Print("  eval {Label}")
	Value

objects_base := class:
	X:int = 1
	Name:string = "base"
	var Count:int = 0
	Describe()<transacts>:string = "base.Describe({Name})"
	Kind()<transacts>:string = "base"
	block:
		Print("  base block: Name={Name} X={X} Count={Count}")
		set Count += 1

objects_derived := class(objects_base):
	X<override>:int = 2
	Name<override>:string = "derived"
	Extra:int = 5
	Describe<override>()<transacts>:string = "derived.Describe then {(super:)Describe()}"
	block:
		Print("  derived block: Count={Count} Extra={Extra}")
		set Count += 10

objects_point := struct:
	X:int = 0
	Y:int = 0

objects_unique := class<unique>:
	Id:int = 0

objects_iface := interface:
	Tag:int = 4
	IfaceName()<transacts>:string

objects_both := class(objects_base, objects_iface):
	IfaceName<override>()<transacts>:string = "both.IfaceName Tag={Tag}"

objects_late_derived := class(objects_late_base):
	Late<override>()<transacts>:string = "late derived"

objects_late_base := class:
	Late()<transacts>:string = "late base"

ObjectsMakeBase<constructor>(N:int)<transacts> := objects_base:
	block:
		Print("  MakeBase body N={N}")
	X := N

ObjectsMakeDerived<constructor>(N:int)<transacts> := objects_derived:
	block:
		Print("  MakeDerived body N={N}")
	Extra := ObjectsSide("MakeDerived Extra", N + 1)
	ObjectsMakeBase<constructor>(N * 10)

objects_basic := class(object):

	# Host-built: this class is instantiated through the ABI, not by an archetype -- objects.md §7.11.
	Held:objects_base = objects_base{}
	var HostBlockRuns:int = 0

	block:
		Print("  objects_basic block")
		set HostBlockRuns += 1

	OverrideAndArchetypeWins<public>():void =
		Print("base{}.X={objects_base{}.X} derived{}.X={objects_derived{}.X}")
		Print("derived(X:=7).X={objects_derived{X := 7}.X} base(X:=8).X={objects_base{X := 8}.X}")

	ArchetypeOrderAndBlocks<public>():void =
		Print("archetype start")
		D := objects_derived{Extra := ObjectsSide("Extra", 6), X := ObjectsSide("X", 3)}
		Print("Count={D.Count} X={D.X} Extra={D.Extra} Name={D.Name}")

	VirtualDispatchAndSuper<public>():void =
		B:objects_base = objects_derived{}
		Print("{B.Describe()} / {B.Kind()}")

	StructCopySemantics<public>():void =
		var P:objects_point = objects_point{X := 1, Y := 2}
		Q := P
		set P = objects_point{X := 9, Y := 9}
		Print("P.X={P.X} Q.X={Q.X}")

	UniqueEquality<public>():void =
		U := objects_unique{}
		Twin := objects_unique{}
		Print("self=eq: {if (U = U) then "yes" else "no"} twin=ne: {if (U = Twin) then "yes" else "no"}")

	ClassAndInterfaceCasts<public>():void =
		D:objects_base = objects_derived{}
		if (X := objects_derived[D]) then Print("derived[D] succeeds") else Print("derived[D] fails")
		Both := objects_both{}
		I:objects_iface = Both
		if (X := objects_iface[I]) then Print("iface[Both] succeeds") else Print("iface[Both] fails")

	EnumIdentity<public>():void =
		A := objects_color.Red
		B := objects_color.Red
		C := objects_color.Green
		Print("Red=Red: {if (A = B) then "yes" else "no"} Red=Green: {if (A = C) then "yes" else "no"}")

	ConstructorDelegation<public>():void =
		Print("MakeDerived(4) start")
		D := ObjectsMakeDerived(4)
		Print("X={D.X} Extra={D.Extra}")

	HostBuiltMemberDefaultOrder<public>():void =
		Print("Held.Count={Held.Count} HostBlockRuns={HostBlockRuns}")

	InterfaceDefaultReachesImplementor<public>():void =
		Both := objects_both{}
		Print("both.Tag={Both.Tag} {Both.IfaceName()}")

	ConstrainedIntTypeCast<public>():void =
		if (V := objects_small[7]) then Print("small[7]={V}") else Print("small[7] fails")
		if (V := objects_small[15]) then Print("small[15]={V}") else Print("small[15] fails")

	DeclaredBeforeSuperclass<public>():void =
		L := objects_late_derived{}
		Print("late derived: {L.Late()}")

objects_color := enum:
	Red
	Green
	Blue

objects_small := type{_X:int where 0 <= _X, _X <= 10}
