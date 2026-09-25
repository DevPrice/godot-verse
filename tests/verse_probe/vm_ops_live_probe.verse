using { /Godot.org/Godot }

# T1.9 (docs/web-vm/spec/ops.md): are live variables -- the only source of RefSetLive, CallSetLive
# and SetFieldLive -- reachable from a script package, and does an ordinary `set` of a live-bound
# var end the binding? Kept apart from vm_ops_probe.verse so a refusal here costs that file nothing.
#
#   bin/verse_probe.exe <engine>/Engine/Binaries/Win64/verse_host.dll <engine>/Engine \
#       <repo>/tests/verse_probe/vm_ops_live_probe.verse --class vm_ops_live_probe

vm_ops_live_probe := class(object):

	var Src:int = 1
	var Dst:int = 0

	L01_Bind<public>():void =
		set live Dst = Src
		Print("L01 bound: Dst = {Dst}")

	L02_WriteSource<public>():void =
		set Src = 5
		Print("L02 Src = 5: Dst = {Dst}")

	L03_OverwriteBound<public>():void =
		set Dst = 100
		Print("L03 Dst set to 100: Dst = {Dst}")

	L04_WriteSourceAgain<public>():void =
		set Src = 7
		Print("L04 Src = 7: Dst = {Dst}")

	var Arr:[]int = array{0, 0}

	L05_BindElement<public>():void =
		if (set live Arr[1] = Src) {}
		if (E := Arr[1]) then Print("L05 Arr[1] = {E}")

	L06_WriteSourceForElement<public>():void =
		set Src = 9
		if (E := Arr[1]) then Print("L06 Src = 9: Arr[1] = {E}")

	L07_OverwriteBoundElement<public>():void =
		if (set Arr[1] = 100) {}
		if (E := Arr[1]) then Print("L07 Arr[1] set to 100: Arr[1] = {E}")

	L08_WriteSourceAgainForElement<public>():void =
		set Src = 11
		if (E := Arr[1]) then Print("L08 Src = 11: Arr[1] = {E}")

	# await on an element of a var array, woken by an element write (CallSet), not by a var write
	var Elems:[]int = array{0, 0}

	ElemAwaiter()<suspends>:void =
		await{Elems[0] > 2}
		if (E := Elems[0]) then Print("  A awaiter resumed, Elems[0] = {E}")

	A01_AwaitElement<public>():void =
		spawn{ElemAwaiter()}
		Print("A01 spawned")

	A02_WriteOtherElement<public>():void =
		if (set Elems[1] = 9) {}
		Print("A02 wrote Elems[1]")

	A03_WriteElement<public>():void =
		if (set Elems[0] = 3) {}
		Print("A03 wrote Elems[0]")
