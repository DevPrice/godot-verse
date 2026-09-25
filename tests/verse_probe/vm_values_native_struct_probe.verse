using { /Godot.org/Godot }

# T1.3: equality of a struct declared in the Godot package (a native struct to the VM) against a
# struct the script declares, with NaN and signed-zero fields. docs/web-vm/spec/values.md §11.

vm_values_native_struct_local := struct:
	X:float = 0.0
	Y:float = 0.0

vm_values_native_struct_probe := class(object):

	NativeStructEquality<public>()<transacts>:void =
		Zero := 0.0
		A := vector2{X := NaN, Y := 1.0}
		B := vector2{X := Inf - Inf, Y := 1.0}
		Print("ns: vector2 NaN = vector2 NaN: {if (A = B) then "succeeds" else "fails"}")
		C := vector2{X := Zero, Y := 1.0}
		D := vector2{X := -Zero, Y := 1.0}
		Print("ns: vector2 0 = vector2 -0: {if (C = D) then "succeeds" else "fails"}")
		Print("ns: vector2 same = same: {if (C = vector2{X := 0.0, Y := 1.0}) then "succeeds" else "fails"}")
		L1 := vm_values_native_struct_local{X := NaN}
		L2 := vm_values_native_struct_local{X := Inf - Inf}
		Print("ns: local struct NaN = NaN: {if (L1 = L2) then "succeeds" else "fails"}")
		L3 := vm_values_native_struct_local{X := Zero}
		L4 := vm_values_native_struct_local{X := -Zero}
		Print("ns: local struct 0 = -0: {if (L3 = L4) then "succeeds" else "fails"}")
		M := map{C => "c"}
		if (V := M[D]) then Print("ns: vector2 key 0 finds -0 = {V}") else Print("ns: vector2 key 0 finds -0 fails")
