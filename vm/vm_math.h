#pragma once

#include "vm_natives.h"

// The $BuiltIn bitwise intrinsics (spec/natives.md §4), the integer Clamp and the seventeen
// `/Verse.org/Verse` float functions (§5.1), and Easing's pure float half (§5.11). Kept apart from
// vm_natives.cpp because none of these touch the heap beyond a BigInt or a Value::from_float: no
// object, no map, no string.
namespace vm {

Outcome bit_and_native(NativeCall &r_call);
Outcome bit_or_native(NativeCall &r_call);
Outcome bit_xor_native(NativeCall &r_call);
Outcome bit_not_native(NativeCall &r_call);

Outcome clamp_int_native(NativeCall &r_call);

Outcome sqrt_native(NativeCall &r_call);
Outcome sin_native(NativeCall &r_call);
Outcome cos_native(NativeCall &r_call);
Outcome tan_native(NativeCall &r_call);
Outcome arcsin_native(NativeCall &r_call);
Outcome arccos_native(NativeCall &r_call);
Outcome arctan1_native(NativeCall &r_call);
Outcome arctan2_native(NativeCall &r_call);
Outcome sinh_native(NativeCall &r_call);
Outcome cosh_native(NativeCall &r_call);
Outcome tanh_native(NativeCall &r_call);
Outcome arsinh_native(NativeCall &r_call);
Outcome arcosh_native(NativeCall &r_call);
Outcome artanh_native(NativeCall &r_call);
Outcome pow_native(NativeCall &r_call);
Outcome exp_native(NativeCall &r_call);
Outcome ln_native(NativeCall &r_call);
Outcome lerp_native(NativeCall &r_call);

// (/Verse.org/Verse/Easing:)CubicBezierInterpInternal: the CSS cubic-bezier evaluator. Pure float
// math; CubicBezier itself (which returns a bound function value) lives in vm_natives.cpp beside
// the other object-returning natives.
Outcome cubic_bezier_interp_native(NativeCall &r_call);

} // namespace vm
