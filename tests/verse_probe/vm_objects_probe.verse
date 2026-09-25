using { /Godot.org/Godot }

# T1.6 (docs/web-vm/spec/objects.md): what classes, archetypes and construction do, run rather
# than read. Every public method is zero-argument and is called by the probe driver in declaration
# order; each prints what it saw.
#
#   bin/verse_probe.exe <engine>/Engine/Binaries/Win64/verse_host.dll <engine>/Engine \
#       tests/verse_probe/vm_objects_probe.verse --class vm_objects_probe
#
# What it answered at engine commit 203d764:
#
#   O1   base{}.X=1 derived{}.X=2; derived{X:=7}.X=7 -- an archetype-supplied field beats an override
#   O2   archetype arguments evaluate in source order; blocks run after every field, base first, and
#        see final values (base block: Name=derived X=3 Count=0; derived block: Count=1; Count=11)
#   O3   derived.Describe then base.Describe(derived) / base
#   O4   a method value keeps its receiver
#   O5   P.X=9 Q.X=1
#   O6   struct equality by fields, and independent of which fields were defaulted
#   O7   <unique>: self=eq twin=ne
#   O8   class and interface casts succeed only on a subtype
#   O9   enumerators compare by identity
#   O10  MakeDerived(4) delegating to MakeBase(40): X=2, not 40 -- the subclass override wins over a
#        base constructor function; order: constructor block, Extra, MakeBase block, class blocks
#   O11  host-built: the member default obj_base{} ran its block before this class's own block
#   O12  an interface data-member default reaches the implementing object (Tag=4)
#   O13  a constrained int type as a cast target: 7 accepted, 15 rejected
#   O14  variant, a native struct, compares by fields
#   O16  a class declared before its superclass works

Side(Label:string, Value:int)<transacts>:int =
	Print("  eval {Label}")
	Value

obj_base := class:
	X:int = 1
	Name:string = "base"
	var Count:int = 0
	Describe()<transacts>:string = "base.Describe({Name})"
	Kind()<transacts>:string = "base"
	block:
		Print("  base block: Name={Name} X={X} Count={Count}")
		set Count += 1

obj_derived := class(obj_base):
	X<override>:int = 2
	Name<override>:string = "derived"
	Extra:int = 5
	Describe<override>()<transacts>:string = "derived.Describe then {(super:)Describe()}"
	block:
		Print("  derived block: Count={Count} Extra={Extra}")
		set Count += 10

obj_point := struct:
	X:int = 0
	Y:int = 0


obj_unique := class<unique>:
	Id:int = 0

obj_iface := interface:
	Tag:int = 4
	IfaceName()<transacts>:string

obj_both := class(obj_base, obj_iface):
	IfaceName<override>()<transacts>:string = "both.IfaceName Tag={Tag}"

obj_color := enum:
	Red
	Green
	Blue

# Declared before its superclass, so the superclass is not yet built when this one is compiled.
obj_late_derived := class(obj_late_base):
	Late<override>()<transacts>:string = "late derived"

obj_late_base := class:
	Late()<transacts>:string = "late base"

obj_small := type{_X:int where 0 <= _X, _X <= 10}

MakeBase<constructor>(N:int)<transacts> := obj_base:
	block:
		Print("  MakeBase body N={N}")
	X := N

MakeDerived<constructor>(N:int)<transacts> := obj_derived:
	block:
		Print("  MakeDerived body N={N}")
	Extra := Side("MakeDerived Extra", N + 1)
	MakeBase<constructor>(N * 10)

vm_objects_probe := class(object):

	# Host-built: this class is instantiated through the ABI, not by an archetype.
	Held:obj_base = obj_base{}
	var HostBlockRuns:int = 0

	block:
		Print("  vm_objects_probe block")
		set HostBlockRuns += 1

	# O1 -- the override rule, and an archetype argument beating an overridden default.
	O1<public>()<transacts>:void =
		Print("O1 base{}.X={obj_base{}.X} derived{}.X={obj_derived{}.X}")
		Print("O1 derived(X:=7).X={obj_derived{X := 7}.X} base(X:=8).X={obj_base{X := 8}.X}")

	# O2 -- archetype argument order, then block order and what the blocks see.
	O2<public>()<transacts>:void =
		Print("O2 start")
		D := obj_derived{Extra := Side("Extra", 6), X := Side("X", 3)}
		Print("O2 Count={D.Count} X={D.X} Extra={D.Extra} Name={D.Name}")

	# O3 -- virtual dispatch through a base-typed value, and (super:).
	O3<public>()<transacts>:void =
		B:obj_base = obj_derived{}
		Print("O3 {B.Describe()} / {B.Kind()}")

	# O4 -- a method taken as a value keeps its receiver.
	O4<public>()<transacts>:void =
		B:obj_base = obj_derived{}
		F := B.Describe
		Print("O4 {F()}")

	# O5 -- a struct is a value: a copy does not see a later set of the variable holding it.
	O5<public>()<transacts>:void =
		var P:obj_point = obj_point{X := 1, Y := 2}
		Q := P
		set P = obj_point{X := 9, Y := P.Y}
		Print("O5 P.X={P.X} Q.X={Q.X}")

	# O6 -- struct equality is by fields.
	O6<public>()<transacts>:void =
		A := obj_point{X := 1, Y := 2}
		B := obj_point{X := 1, Y := 2}
		C := obj_point{X := 1, Y := 3}
		R1 := if (A = B) then "eq" else "ne"
		R2 := if (A = C) then "eq" else "ne"
		# Defaults left in place against the same values supplied: two different layouts.
		R3 := if (obj_point{} = obj_point{X := 0, Y := 0}) then "eq" else "ne"
		Print("O6 same-fields={R1} different={R2} defaulted-vs-supplied={R3}")

	# O7 -- <unique> equality is identity.
	O7<public>()<transacts>:void =
		A := obj_unique{Id := 1}
		B := obj_unique{Id := 1}
		R1 := if (A = A) then "eq" else "ne"
		R2 := if (A = B) then "eq" else "ne"
		Print("O7 self={R1} twin={R2}")

	# O8 -- dynamic casts to a class and to an interface.
	O8<public>()<transacts>:void =
		B1:obj_base = obj_derived{}
		B2:obj_base = obj_base{}
		B3:obj_base = obj_both{}
		R1 := if (D := obj_derived[B1]) then "ok {D.Extra}" else "fail"
		R2 := if (D := obj_derived[B2]) then "ok {D.Extra}" else "fail"
		R3 := if (I := obj_iface[B3]) then "ok {I.IfaceName()}" else "fail"
		R4 := if (I := obj_iface[B1]) then "ok {I.IfaceName()}" else "fail"
		Print("O8 derived[derived]={R1} derived[base]={R2} iface[both]={R3} iface[derived]={R4}")

	# O9 -- enumerators compare by identity.
	O9<public>()<transacts>:void =
		C:obj_color = obj_color.Green
		R1 := if (C = obj_color.Green) then "eq" else "ne"
		R2 := if (C = obj_color.Blue) then "eq" else "ne"
		Print("O9 green=green {R1} green=blue {R2}")

	# O10 -- constructor functions, and delegation to a superclass constructor.
	O10<public>()<transacts>:void =
		Print("O10 start")
		D := MakeDerived(4)
		Print("O10 X={D.X} Extra={D.Extra} Count={D.Count}")

	# O11 -- what a host-built object's field default holds.
	O11<public>()<transacts>:void =
		Print("O11 HostBlockRuns={HostBlockRuns}")
		Print("O11 Held.Count={Held.Count}")
		Print("O11 Held.X={Held.X} Held.Name={Held.Name}")

	# O13 -- a fallible cast to a constrained int type.
	O13<public>()<transacts>:void =
		A:int = 7
		B:int = 15
		R1 := if (S := obj_small[A]) then "ok {S}" else "fail"
		R2 := if (S := obj_small[B]) then "ok {S}" else "fail"
		Print("O13 small[7]={R1} small[15]={R2}")

	# O14 -- a native-represented struct (variant) compares by fields, like any struct.
	O14<public>()<transacts>:void =
		A := VariantInt(3)
		B := VariantInt(3)
		C := VariantInt(4)
		R1 := if (A = B) then "eq" else "ne"
		R2 := if (A = C) then "eq" else "ne"
		Print("O14 same={R1} different={R2}")

	# O16 -- a class declared before its superclass.
	O16<public>()<transacts>:void =
		L:obj_late_base = obj_late_derived{}
		Print("O16 {L.Late()}")

	# O12 -- an interface's data member default, through the implementing class.
	O12<public>()<transacts>:void =
		O := obj_both{}
		Print("O12 Tag={O.Tag} Count={O.Count}")
