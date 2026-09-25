#include "vm_math.h"

#include <algorithm>
#include <cmath>

#include "vm_number.h"

namespace vm {

namespace {

// spec/values.md §3.2, quoted by natives.md §5.1: replacing X with X + 0.0 turns -0.0 into +0.0 and
// changes nothing else. Named separately from vm_number.h's canonical_float because natives.md
// spells it as an operation on the argument, not a general value rule.
double plus_zero(double p_value) {
	return p_value + 0.0;
}

double clamp_unit(double p_value) {
	if (p_value < -1.0) {
		return -1.0;
	}
	if (p_value < 1.0) {
		return p_value;
	}
	return 1.0;
}

Outcome float_result(NativeCall &r_call, double p_value) {
	r_call.result = Value::from_float(p_value);
	return Outcome::Ok;
}

Outcome unary_float(NativeCall &r_call, double (*p_fn)(double)) {
	if (unbound_argument(r_call)) {
		return Outcome::Park;
	}
	const Value x = argument(r_call, 0);
	if (!x.is_float()) {
		return Outcome::Invalid;
	}
	return float_result(r_call, p_fn(x.as_float()));
}

Outcome binary_float(NativeCall &r_call, double (*p_fn)(double, double)) {
	if (unbound_argument(r_call)) {
		return Outcome::Park;
	}
	const Value a = argument(r_call, 0);
	const Value b = argument(r_call, 1);
	if (!a.is_float() || !b.is_float()) {
		return Outcome::Invalid;
	}
	return float_result(r_call, p_fn(a.as_float(), b.as_float()));
}

double do_sqrt(double x) { return std::sqrt(plus_zero(x)); }
double do_sin(double x) { return std::sin(x); }
double do_cos(double x) { return std::cos(x); }
double do_tan(double x) { return std::tan(plus_zero(x)); }
double do_arcsin(double x) { return std::asin(clamp_unit(x)); }
double do_arccos(double x) { return std::acos(clamp_unit(x)); }
double do_arctan1(double x) { return std::atan(x); }
double do_sinh(double x) { return std::sinh(x); }
double do_cosh(double x) { return std::cosh(x); }
double do_tanh(double x) { return std::tanh(x); }
double do_arsinh(double x) { return std::asinh(x); }
double do_arcosh(double x) { return std::acosh(x); }
double do_artanh(double x) { return std::atanh(x); }
double do_pow(double a, double b) { return std::pow(a, b); }
double do_exp(double x) { return std::exp(x); }
double do_ln(double x) { return std::log(plus_zero(x)); }

double do_arctan2(double y, double x) {
	if (x == 0.0 && y == 0.0) {
		return 0.0;
	}
	return std::atan2(y, x);
}

// Every argument must fit 64 bits (spec/natives.md §3.6); Clamp is in §5.3, not the §4 intrinsics.
Outcome int64_argument(NativeCall &r_call, uint32_t p_index, int64_t &r_value) {
	const Value value = argument(r_call, p_index);
	return int_to_int64(value, r_value, r_call.error);
}

// spec/natives.md §4: infinite two's-complement AND/OR/XOR over an arbitrary-precision sign-
// magnitude BigInt. y is x itself when x is non-negative, and BitNot(x) = -x-1 when it is negative,
// so y's own (non-negative) bits are x's two's-complement bits complemented exactly when x < 0.
enum class BitOp : uint8_t { And, Or, Xor };

bool combine_bits(BitOp p_op, bool p_left, bool p_right) {
	switch (p_op) {
		case BitOp::And:
			return p_left && p_right;
		case BitOp::Or:
			return p_left || p_right;
		case BitOp::Xor:
			return p_left != p_right;
	}
	return false;
}

BigInt twos_complement_magnitude(const BigInt &p_value) {
	return p_value.negative ? big_sub(big_neg(p_value), BigInt::from_int64(1)) : p_value;
}

bool twos_complement_bit(const BigInt &p_value, const BigInt &p_magnitude, size_t p_bit) {
	const bool bit = p_magnitude.test_bit(p_bit);
	return p_value.negative ? !bit : bit;
}

BigInt bitwise_op(BitOp p_op, const BigInt &p_left, const BigInt &p_right) {
	const BigInt left_magnitude = twos_complement_magnitude(p_left);
	const BigInt right_magnitude = twos_complement_magnitude(p_right);
	const size_t bit_count = std::max(left_magnitude.bit_length(), right_magnitude.bit_length()) + 1;
	const bool result_negative = combine_bits(p_op, p_left.negative, p_right.negative);

	BigInt accumulator;
	for (size_t bit = bit_count; bit-- > 0;) {
		accumulator = big_shift_left(accumulator, 1);
		const bool combined = combine_bits(p_op, twos_complement_bit(p_left, left_magnitude, bit),
				twos_complement_bit(p_right, right_magnitude, bit));
		const bool keep = result_negative ? !combined : combined;
		if (keep) {
			accumulator = big_add(accumulator, BigInt::from_int64(1));
		}
	}
	return result_negative ? big_neg(big_add(accumulator, BigInt::from_int64(1))) : accumulator;
}

Outcome bitwise_native(NativeCall &r_call, BitOp p_op) {
	if (unbound_argument(r_call)) {
		return Outcome::Park;
	}
	const Value left = argument(r_call, 0);
	const Value right = argument(r_call, 1);
	if (!is_int(left) || !is_int(right)) {
		return Outcome::Invalid;
	}
	// int64's two's complement is the infinite one truncated, and AND/OR/XOR of two values that
	// sign-extend from bit 63 sign-extends from bit 63 too, so the result is exact.
	int64_t left64 = 0;
	int64_t right64 = 0;
	if (int_as_int64(left, left64) && int_as_int64(right, right64)) {
		const int64_t combined = p_op == BitOp::And ? (left64 & right64) : (p_op == BitOp::Or ? (left64 | right64) : (left64 ^ right64));
		r_call.result = make_int(r_call.heap, combined);
		return Outcome::Ok;
	}
	r_call.result = make_int(r_call.heap, bitwise_op(p_op, int_value(left), int_value(right)));
	return Outcome::Ok;
}

} // namespace

Outcome bit_and_native(NativeCall &r_call) { return bitwise_native(r_call, BitOp::And); }
Outcome bit_or_native(NativeCall &r_call) { return bitwise_native(r_call, BitOp::Or); }
Outcome bit_xor_native(NativeCall &r_call) { return bitwise_native(r_call, BitOp::Xor); }

// BitNot(x) = -x-1 (spec/natives.md §4), true for every x including the two ends of int64.
Outcome bit_not_native(NativeCall &r_call) {
	if (unbound_argument(r_call)) {
		return Outcome::Park;
	}
	const Value value = argument(r_call, 0);
	if (!is_int(value)) {
		return Outcome::Invalid;
	}
	int64_t value64 = 0;
	if (int_as_int64(value, value64)) {
		r_call.result = make_int(r_call.heap, ~value64);
		return Outcome::Ok;
	}
	r_call.result = make_int(r_call.heap, big_sub(big_neg(int_value(value)), BigInt::from_int64(1)));
	return Outcome::Ok;
}

// Clamp(Val, A, B) = the median of Val and [min(A,B), max(A,B)] (spec/natives.md §5.3).
Outcome clamp_int_native(NativeCall &r_call) {
	if (unbound_argument(r_call)) {
		return Outcome::Park;
	}
	int64_t value = 0;
	int64_t a = 0;
	int64_t b = 0;
	Outcome outcome = int64_argument(r_call, 0, value);
	if (outcome != Outcome::Ok) {
		return outcome;
	}
	outcome = int64_argument(r_call, 1, a);
	if (outcome != Outcome::Ok) {
		return outcome;
	}
	outcome = int64_argument(r_call, 2, b);
	if (outcome != Outcome::Ok) {
		return outcome;
	}
	const int64_t lo = std::min(a, b);
	const int64_t hi = std::max(a, b);
	r_call.result = make_int(r_call.heap, std::max(std::min(value, hi), lo));
	return Outcome::Ok;
}

Outcome sqrt_native(NativeCall &r_call) { return unary_float(r_call, &do_sqrt); }
Outcome sin_native(NativeCall &r_call) { return unary_float(r_call, &do_sin); }
Outcome cos_native(NativeCall &r_call) { return unary_float(r_call, &do_cos); }
Outcome tan_native(NativeCall &r_call) { return unary_float(r_call, &do_tan); }
Outcome arcsin_native(NativeCall &r_call) { return unary_float(r_call, &do_arcsin); }
Outcome arccos_native(NativeCall &r_call) { return unary_float(r_call, &do_arccos); }
Outcome arctan1_native(NativeCall &r_call) { return unary_float(r_call, &do_arctan1); }
Outcome sinh_native(NativeCall &r_call) { return unary_float(r_call, &do_sinh); }
Outcome cosh_native(NativeCall &r_call) { return unary_float(r_call, &do_cosh); }
Outcome tanh_native(NativeCall &r_call) { return unary_float(r_call, &do_tanh); }
Outcome arsinh_native(NativeCall &r_call) { return unary_float(r_call, &do_arsinh); }
Outcome arcosh_native(NativeCall &r_call) { return unary_float(r_call, &do_arcosh); }
Outcome artanh_native(NativeCall &r_call) { return unary_float(r_call, &do_artanh); }
Outcome exp_native(NativeCall &r_call) { return unary_float(r_call, &do_exp); }
Outcome ln_native(NativeCall &r_call) { return unary_float(r_call, &do_ln); }
Outcome pow_native(NativeCall &r_call) { return binary_float(r_call, &do_pow); }

// ArcTan(Y, X): natives.md §5.1's own zero/zero special case ahead of atan2.
Outcome arctan2_native(NativeCall &r_call) { return binary_float(r_call, &do_arctan2); }

// Lerp: each product rounded, then the sum (spec/natives.md §5.1) -- not FMA, and not
// From + Parameter*(To-From), which would answer a different NaN for Lerp(0,Inf,0).
Outcome lerp_native(NativeCall &r_call) {
	if (unbound_argument(r_call)) {
		return Outcome::Park;
	}
	const Value from = argument(r_call, 0);
	const Value to = argument(r_call, 1);
	const Value parameter = argument(r_call, 2);
	if (!from.is_float() || !to.is_float() || !parameter.is_float()) {
		return Outcome::Invalid;
	}
	const double t = parameter.as_float();
	const double left = from.as_float() * (1.0 - t);
	const double right = to.as_float() * t;
	return float_result(r_call, left + right);
}

// spec/natives.md §5.11: the CSS cubic-bezier evaluator behind CubicBezier and Linear/Ease*.
Outcome cubic_bezier_interp_native(NativeCall &r_call) {
	if (unbound_argument(r_call)) {
		return Outcome::Park;
	}
	const Value t_value = argument(r_call, 0);
	const Value x0_value = argument(r_call, 1);
	const Value y0_value = argument(r_call, 2);
	const Value x1_value = argument(r_call, 3);
	const Value y1_value = argument(r_call, 4);
	if (!t_value.is_float() || !x0_value.is_float() || !y0_value.is_float() || !x1_value.is_float() || !y1_value.is_float()) {
		return Outcome::Invalid;
	}
	const double t = t_value.as_float();
	const double x0 = x0_value.as_float();
	const double y0 = y0_value.as_float();
	const double x1 = x1_value.as_float();
	const double y1 = y1_value.as_float();

	const bool in_range = 0.0 <= x0 && x0 <= 1.0 && 0.0 <= x1 && x1 <= 1.0;
	if (!in_range || (x0 == y0 && x1 == y1)) {
		return float_result(r_call, t);
	}

	const double a = x0 - 0.0;
	const double b = x1 - x0;
	const double c = 1.0 - x1;
	const double d = b - a;
	const double c3 = c - b - d;
	const double c2 = 3.0 * d;
	const double c1 = 3.0 * a;
	const double c0 = 0.0 - t;

	// spec/natives.md §5.11's own min/max, not std::min/std::max: p if p<q else q, and p if q<p else
	// q, which is what makes clamp(NaN, 0, 1) answer 1 rather than propagate the NaN.
	const auto verse_min = [](double p, double q) { return p < q ? p : q; };
	const auto verse_max = [](double p, double q) { return q < p ? p : q; };
	const auto clamp01 = [&](double x) { return verse_max(verse_min(x, 1.0), 0.0); };

	constexpr double kEpsilon = 9.99999974737875e-05;
	double r = t;
	for (int iteration = 0; iteration < 10; ++iteration) {
		const double n = ((c3 * r + c2) * r + c1) * r + c0;
		const double denominator = ((3.0 * c3) * r + (2.0 * c2)) * r + c1;
		const double division = denominator == 0.0 ? 0.0 : n / denominator;
		const double next = clamp01(r - division);
		const double step = next - r;
		r = next;
		if (std::fabs(step) <= kEpsilon) {
			break;
		}
	}

	const auto lerp_of = [](double p, double q, double s) { return p + s * (q - p); };
	const double e = lerp_of(0.0, y0, r);
	const double f = lerp_of(y0, y1, r);
	const double g = lerp_of(y1, 1.0, r);
	const double h = lerp_of(e, f, r);
	const double i = lerp_of(f, g, r);
	return float_result(r_call, lerp_of(h, i, r));
}

} // namespace vm
