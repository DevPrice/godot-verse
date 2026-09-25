#pragma once

#include <cstdint>
#include <string>

#include "vm_bigint.h"
#include "vm_heap.h"
#include "vm_status.h"
#include "vm_value.h"

// Integers, rationals and floats: spec/values.md §2 and §3.
namespace vm {

bool is_int(Value p_value);
bool is_rational(Value p_value);

// An integer as an immediate when it fits int32 and as a heap int otherwise.
Value make_int(Heap &r_heap, int64_t p_value);
Value make_int(Heap &r_heap, const BigInt &p_value);
// p_value must be an integer.
BigInt int_value(Value p_value);
// False for anything but an integer inside int64; unlike int_to_int64 it never raises.
bool int_as_int64(Value p_value, int64_t &r_value);

// Lowest terms, positive denominator. p_denominator must not be zero.
Value make_rational(Heap &r_heap, const BigInt &p_numerator, const BigInt &p_denominator);

// The Add, Sub, Mul, Div and Neg ops on numbers and, for Add, arrays (spec/values.md §2.2-§2.4,
// §3.1, §6.3). Div of two integers is a rational and fails on a zero divisor; float division never
// fails and treats a divisor of -0 as +0.
Outcome value_add(Heap &r_heap, Value p_left, Value p_right, Value &r_result);
Outcome value_sub(Heap &r_heap, Value p_left, Value p_right, Value &r_result);
Outcome value_mul(Heap &r_heap, Value p_left, Value p_right, Value &r_result);
Outcome value_div(Heap &r_heap, Value p_left, Value p_right, Value &r_result);
Outcome value_neg(Heap &r_heap, Value p_operand, Value &r_result);

// Floor and Ceil of an integer or a rational (spec/values.md §2.4).
Outcome rational_floor(Heap &r_heap, Value p_operand, Value &r_result);
Outcome rational_ceil(Heap &r_heap, Value p_operand, Value &r_result);

enum class OrderOp : uint8_t {
	Lt,
	Lte,
	Gt,
	Gte,
};

// Lt, Lte, Gt, Gte (spec/values.md §12): two integers, two floats, or a rational with a rational or
// an integer. Anything else is Invalid.
Outcome value_order(OrderOp p_op, Value p_left, Value p_right, bool &r_holds);

// The conversion every native taking an `int` makes (spec/values.md §2.6).
Outcome int_to_int64(Value p_value, int64_t &r_result, RuntimeError &r_error);

// The library's Quotient[] and Mod[]: Euclidean, the remainder in [0, |divisor|) (facts.md §2).
// Fail on a zero divisor; raise on an argument outside int64 and on the one quotient that
// overflows (spec/values.md §2.5).
Outcome euclidean_quotient(Heap &r_heap, Value p_dividend, Value p_divisor, Value &r_result, RuntimeError &r_error);
Outcome euclidean_mod(Heap &r_heap, Value p_dividend, Value p_divisor, Value &r_result, RuntimeError &r_error);

// Round to nearest, ties to even (spec/values.md §2.7).
double int_to_float(Value p_value);

enum class FloatRounding : uint8_t {
	Floor,
	Ceil,
	Round,
	Truncate,
};

// Floor[], Ceil[], Round[] (ties to even) and Int[] of a float (spec/values.md §3.6).
Outcome float_to_int(Heap &r_heap, double p_value, FloatRounding p_rounding, Value &r_result, RuntimeError &r_error);

// What a native receives for a float: -0 becomes +0 (spec/values.md §3.2).
double canonical_float(double p_value);

// ToString(:int): raises outside int64 (spec/values.md §2.6).
Outcome int_to_string(Value p_value, std::string &r_text, RuntimeError &r_error);
// ToString(:float), spec/values.md §3.4: six decimals from the exact binary value, ties to even.
std::string float_to_string(double p_value);

} // namespace vm
