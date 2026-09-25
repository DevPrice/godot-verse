#include "vm_number.h"

#include <cmath>
#include <cstring>

#include "vm_cell.h"
#include "vm_values.h"

namespace vm {

namespace {

const char *const GENERATED_NATIVE_INTERNAL = "ErrRuntime_GeneratedNativeInternal";
const char *const GENERATED_NATIVE_INTERNAL_DESCRIPTION = "An internal runtime error occurred in (generated) native code that was called from Verse. There is no other information available.";
const char *const EXCEEDS_INT64 = "Value exceeds the range of a 64 bit integer.";
const char *const INTEGER_OVERFLOW = "ErrRuntime_IntegerOverflow";
const char *const INTEGER_OVERFLOW_TEXT = "Integer overflow encountered.";
const char *const INTEGER_BOUNDS_EXCEEDED = "ErrRuntime_IntegerBoundsExceeded";
const char *const INTEGER_BOUNDS_EXCEEDED_DESCRIPTION = "A value does not fall inside the representable range of a Verse integer.";

struct Fraction {
	BigInt numerator;
	BigInt denominator;
};

// An integer reads as n/1; anything but an integer or a rational answers false.
bool as_fraction(Value p_value, Fraction &r_fraction) {
	if (is_int(p_value)) {
		r_fraction.numerator = int_value(p_value);
		r_fraction.denominator = BigInt::from_int64(1);
		return true;
	}
	if (is_rational(p_value)) {
		const RationalCell *rational = cell_as<RationalCell>(p_value);
		r_fraction.numerator = rational->numerator;
		r_fraction.denominator = rational->denominator;
		return true;
	}
	return false;
}

// At least one side is a rational and neither is anything but an integer or a rational: the
// shape spec/values.md §2.4 gives the rational ops.
bool as_fraction_pair(Value p_left, Value p_right, Fraction &r_left, Fraction &r_right) {
	if (!is_rational(p_left) && !is_rational(p_right)) {
		return false;
	}
	return as_fraction(p_left, r_left) && as_fraction(p_right, r_right);
}

bool both_int32(Value p_left, Value p_right) {
	return p_left.is_int32() && p_right.is_int32();
}

bool both_int(Value p_left, Value p_right) {
	return is_int(p_left) && is_int(p_right);
}

bool both_float(Value p_left, Value p_right) {
	return p_left.is_float() && p_right.is_float();
}

void set_range_error(RuntimeError &r_error) {
	r_error.diagnostic = GENERATED_NATIVE_INTERNAL;
	r_error.description = GENERATED_NATIVE_INTERNAL_DESCRIPTION;
	r_error.message = EXCEEDS_INT64;
}

// Euclidean division on int64, spec/values.md §2.5 and facts.md §2.
Outcome euclidean(Value p_dividend, Value p_divisor, int64_t &r_quotient, int64_t &r_remainder, RuntimeError &r_error) {
	int64_t dividend = 0;
	int64_t divisor = 0;
	Outcome outcome = int_to_int64(p_dividend, dividend, r_error);
	if (outcome != Outcome::Ok) {
		return outcome;
	}
	outcome = int_to_int64(p_divisor, divisor, r_error);
	if (outcome != Outcome::Ok) {
		return outcome;
	}
	if (divisor == 0) {
		return Outcome::Fail;
	}
	if (dividend == INT64_MIN && divisor == -1) {
		r_error.diagnostic = INTEGER_OVERFLOW;
		r_error.description = INTEGER_OVERFLOW_TEXT;
		r_error.message = INTEGER_OVERFLOW_TEXT;
		return Outcome::Error;
	}
	int64_t quotient = dividend / divisor;
	int64_t remainder = dividend % divisor;
	if (remainder < 0) {
		if (divisor > 0) {
			--quotient;
			remainder += divisor;
		} else {
			++quotient;
			remainder -= divisor;
		}
	}
	r_quotient = quotient;
	r_remainder = remainder;
	return Outcome::Ok;
}

} // namespace

bool is_int(Value p_value) {
	return p_value.is_int32() || is_cell_kind(p_value, CellKind::HeapInt);
}

bool is_rational(Value p_value) {
	return is_cell_kind(p_value, CellKind::Rational);
}

Value make_int(Heap &r_heap, int64_t p_value) {
	if (p_value >= INT32_MIN && p_value <= INT32_MAX) {
		return Value::from_int32(int32_t(p_value));
	}
	return Value::from_cell(r_heap.make<HeapIntCell>(BigInt::from_int64(p_value)));
}

Value make_int(Heap &r_heap, const BigInt &p_value) {
	if (p_value.fits_int32()) {
		return Value::from_int32(p_value.to_int32());
	}
	return Value::from_cell(r_heap.make<HeapIntCell>(p_value));
}

BigInt int_value(Value p_value) {
	if (p_value.is_int32()) {
		return BigInt::from_int64(p_value.as_int32());
	}
	return cell_as<HeapIntCell>(p_value)->value;
}

Value make_rational(Heap &r_heap, const BigInt &p_numerator, const BigInt &p_denominator) {
	const BigInt divisor = big_gcd(p_numerator, p_denominator);
	BigInt numerator;
	BigInt denominator;
	BigInt remainder;
	big_divmod_trunc(p_numerator, divisor, numerator, remainder);
	big_divmod_trunc(p_denominator, divisor, denominator, remainder);
	if (denominator.negative) {
		numerator = big_neg(numerator);
		denominator = big_neg(denominator);
	}
	return Value::from_cell(r_heap.make<RationalCell>(std::move(numerator), std::move(denominator)));
}

Outcome value_add(Heap &r_heap, Value p_left, Value p_right, Value &r_result) {
	if (both_int32(p_left, p_right)) {
		r_result = make_int(r_heap, int64_t(p_left.as_int32()) + p_right.as_int32());
		return Outcome::Ok;
	}
	if (both_int(p_left, p_right)) {
		r_result = make_int(r_heap, big_add(int_value(p_left), int_value(p_right)));
		return Outcome::Ok;
	}
	if (both_float(p_left, p_right)) {
		r_result = Value::from_float(p_left.as_float() + p_right.as_float());
		return Outcome::Ok;
	}
	Fraction left;
	Fraction right;
	if (as_fraction_pair(p_left, p_right, left, right)) {
		const BigInt numerator = big_add(big_mul(left.numerator, right.denominator), big_mul(right.numerator, left.denominator));
		r_result = make_rational(r_heap, numerator, big_mul(left.denominator, right.denominator));
		return Outcome::Ok;
	}
	return array_concat(r_heap, p_left, p_right, r_result);
}

Outcome value_sub(Heap &r_heap, Value p_left, Value p_right, Value &r_result) {
	if (both_int32(p_left, p_right)) {
		r_result = make_int(r_heap, int64_t(p_left.as_int32()) - p_right.as_int32());
		return Outcome::Ok;
	}
	if (both_int(p_left, p_right)) {
		r_result = make_int(r_heap, big_sub(int_value(p_left), int_value(p_right)));
		return Outcome::Ok;
	}
	if (both_float(p_left, p_right)) {
		r_result = Value::from_float(p_left.as_float() - p_right.as_float());
		return Outcome::Ok;
	}
	Fraction left;
	Fraction right;
	if (as_fraction_pair(p_left, p_right, left, right)) {
		const BigInt numerator = big_sub(big_mul(left.numerator, right.denominator), big_mul(right.numerator, left.denominator));
		r_result = make_rational(r_heap, numerator, big_mul(left.denominator, right.denominator));
		return Outcome::Ok;
	}
	return Outcome::Invalid;
}

Outcome value_mul(Heap &r_heap, Value p_left, Value p_right, Value &r_result) {
	if (both_int32(p_left, p_right)) {
		r_result = make_int(r_heap, int64_t(p_left.as_int32()) * p_right.as_int32());
		return Outcome::Ok;
	}
	if (both_int(p_left, p_right)) {
		r_result = make_int(r_heap, big_mul(int_value(p_left), int_value(p_right)));
		return Outcome::Ok;
	}
	if (both_float(p_left, p_right)) {
		r_result = Value::from_float(p_left.as_float() * p_right.as_float());
		return Outcome::Ok;
	}
	if (is_int(p_left) && p_right.is_float()) {
		r_result = Value::from_float(int_to_float(p_left) * p_right.as_float());
		return Outcome::Ok;
	}
	if (p_left.is_float() && is_int(p_right)) {
		r_result = Value::from_float(p_left.as_float() * int_to_float(p_right));
		return Outcome::Ok;
	}
	Fraction left;
	Fraction right;
	if (as_fraction_pair(p_left, p_right, left, right)) {
		r_result = make_rational(r_heap, big_mul(left.numerator, right.numerator), big_mul(left.denominator, right.denominator));
		return Outcome::Ok;
	}
	return Outcome::Invalid;
}

Outcome value_div(Heap &r_heap, Value p_left, Value p_right, Value &r_result) {
	if (both_float(p_left, p_right)) {
		// spec/values.md §3.1: a divisor of -0 divides as +0.
		const double divisor = p_right.as_float() == 0.0 ? 0.0 : p_right.as_float();
		r_result = Value::from_float(p_left.as_float() / divisor);
		return Outcome::Ok;
	}
	Fraction left;
	Fraction right;
	if (!as_fraction(p_left, left) || !as_fraction(p_right, right)) {
		return Outcome::Invalid;
	}
	if (right.numerator.is_zero()) {
		return Outcome::Fail;
	}
	r_result = make_rational(r_heap, big_mul(left.numerator, right.denominator), big_mul(left.denominator, right.numerator));
	return Outcome::Ok;
}

Outcome value_neg(Heap &r_heap, Value p_operand, Value &r_result) {
	if (p_operand.is_int32()) {
		r_result = make_int(r_heap, -int64_t(p_operand.as_int32()));
		return Outcome::Ok;
	}
	if (is_int(p_operand)) {
		r_result = make_int(r_heap, big_neg(int_value(p_operand)));
		return Outcome::Ok;
	}
	if (p_operand.is_float()) {
		r_result = Value::from_float(-p_operand.as_float());
		return Outcome::Ok;
	}
	if (is_rational(p_operand)) {
		const RationalCell *rational = cell_as<RationalCell>(p_operand);
		r_result = Value::from_cell(r_heap.make<RationalCell>(big_neg(rational->numerator), rational->denominator));
		return Outcome::Ok;
	}
	return Outcome::Invalid;
}

Outcome rational_floor(Heap &r_heap, Value p_operand, Value &r_result) {
	Fraction fraction;
	if (!as_fraction(p_operand, fraction)) {
		return Outcome::Invalid;
	}
	r_result = make_int(r_heap, big_div_floor(fraction.numerator, fraction.denominator));
	return Outcome::Ok;
}

Outcome rational_ceil(Heap &r_heap, Value p_operand, Value &r_result) {
	Fraction fraction;
	if (!as_fraction(p_operand, fraction)) {
		return Outcome::Invalid;
	}
	r_result = make_int(r_heap, big_div_ceil(fraction.numerator, fraction.denominator));
	return Outcome::Ok;
}

Outcome value_order(OrderOp p_op, Value p_left, Value p_right, bool &r_holds) {
	if (both_float(p_left, p_right)) {
		const double left = p_left.as_float();
		const double right = p_right.as_float();
		const bool both_nan = std::isnan(left) && std::isnan(right);
		switch (p_op) {
			case OrderOp::Lt:
				r_holds = left < right;
				break;
			case OrderOp::Lte:
				r_holds = both_nan || left <= right;
				break;
			case OrderOp::Gt:
				r_holds = left > right;
				break;
			case OrderOp::Gte:
				r_holds = both_nan || left >= right;
				break;
		}
		return Outcome::Ok;
	}

	int order = 0;
	if (both_int32(p_left, p_right)) {
		order = p_left.as_int32() < p_right.as_int32() ? -1 : (p_left.as_int32() > p_right.as_int32() ? 1 : 0);
	} else if (both_int(p_left, p_right)) {
		order = big_compare(int_value(p_left), int_value(p_right));
	} else {
		Fraction left;
		Fraction right;
		if (!as_fraction_pair(p_left, p_right, left, right)) {
			return Outcome::Invalid;
		}
		order = big_compare(big_mul(left.numerator, right.denominator), big_mul(right.numerator, left.denominator));
	}
	switch (p_op) {
		case OrderOp::Lt:
			r_holds = order < 0;
			break;
		case OrderOp::Lte:
			r_holds = order <= 0;
			break;
		case OrderOp::Gt:
			r_holds = order > 0;
			break;
		case OrderOp::Gte:
			r_holds = order >= 0;
			break;
	}
	return Outcome::Ok;
}

Outcome int_to_int64(Value p_value, int64_t &r_result, RuntimeError &r_error) {
	if (p_value.is_int32()) {
		r_result = p_value.as_int32();
		return Outcome::Ok;
	}
	if (!is_int(p_value)) {
		return Outcome::Invalid;
	}
	const BigInt &value = cell_as<HeapIntCell>(p_value)->value;
	if (!value.fits_int64()) {
		set_range_error(r_error);
		return Outcome::Error;
	}
	r_result = value.to_int64();
	return Outcome::Ok;
}

Outcome euclidean_quotient(Heap &r_heap, Value p_dividend, Value p_divisor, Value &r_result, RuntimeError &r_error) {
	int64_t quotient = 0;
	int64_t remainder = 0;
	const Outcome outcome = euclidean(p_dividend, p_divisor, quotient, remainder, r_error);
	if (outcome == Outcome::Ok) {
		r_result = make_int(r_heap, quotient);
	}
	return outcome;
}

Outcome euclidean_mod(Heap &r_heap, Value p_dividend, Value p_divisor, Value &r_result, RuntimeError &r_error) {
	int64_t quotient = 0;
	int64_t remainder = 0;
	const Outcome outcome = euclidean(p_dividend, p_divisor, quotient, remainder, r_error);
	if (outcome == Outcome::Ok) {
		r_result = make_int(r_heap, remainder);
	}
	return outcome;
}

double int_to_float(Value p_value) {
	if (p_value.is_int32()) {
		return double(p_value.as_int32());
	}
	return big_to_double(cell_as<HeapIntCell>(p_value)->value);
}

Outcome float_to_int(Heap &r_heap, double p_value, FloatRounding p_rounding, Value &r_result, RuntimeError &r_error) {
	if (!std::isfinite(p_value)) {
		return Outcome::Fail;
	}
	double rounded = 0.0;
	switch (p_rounding) {
		case FloatRounding::Floor:
			rounded = std::floor(p_value);
			break;
		case FloatRounding::Ceil:
			rounded = std::ceil(p_value);
			break;
		case FloatRounding::Truncate:
			rounded = std::trunc(p_value);
			break;
		case FloatRounding::Round: {
			const double below = std::floor(p_value);
			const double fraction = p_value - below;
			if (fraction > 0.5) {
				rounded = below + 1.0;
			} else if (fraction < 0.5) {
				rounded = below;
			} else {
				rounded = std::fmod(below, 2.0) == 0.0 ? below : below + 1.0;
			}
			break;
		}
	}
	// [-2^63, 2^63): both bounds are exact binary64 values.
	if (rounded < -9223372036854775808.0 || rounded >= 9223372036854775808.0) {
		r_error.diagnostic = INTEGER_BOUNDS_EXCEEDED;
		r_error.description = INTEGER_BOUNDS_EXCEEDED_DESCRIPTION;
		r_error.message = "The value " + float_to_string(rounded) +
				" cannot be converted to an integer because it does not fall inside the representable range of a Verse integer.";
		return Outcome::Error;
	}
	r_result = make_int(r_heap, int64_t(rounded));
	return Outcome::Ok;
}

double canonical_float(double p_value) {
	return p_value == 0.0 ? 0.0 : p_value;
}

Outcome int_to_string(Value p_value, std::string &r_text, RuntimeError &r_error) {
	int64_t value = 0;
	const Outcome outcome = int_to_int64(p_value, value, r_error);
	if (outcome == Outcome::Ok) {
		r_text = big_to_decimal(BigInt::from_int64(value));
	}
	return outcome;
}

std::string float_to_string(double p_value) {
	if (std::isnan(p_value)) {
		return "NaN";
	}
	if (std::isinf(p_value)) {
		return p_value > 0 ? "Inf" : "-Inf";
	}
	const bool negative = p_value < 0.0;

	uint64_t raw;
	std::memcpy(&raw, &p_value, sizeof(raw));
	const int biased_exponent = int((raw >> 52) & 0x7FF);
	uint64_t mantissa = raw & ((uint64_t(1) << 52) - 1);
	int exponent = -1074;
	if (biased_exponent != 0) {
		mantissa |= uint64_t(1) << 52;
		exponent = biased_exponent - 1075;
	}

	// |value| * 10^6 = mantissa * 10^6 * 2^exponent, rounded to an integer half to even.
	BigInt scaled = big_mul(BigInt::from_uint64(mantissa), BigInt::from_int64(1000000));
	if (exponent >= 0) {
		scaled = big_shift_left(scaled, size_t(exponent));
	} else {
		const size_t shift = size_t(-exponent);
		const BigInt quotient = big_shift_right(scaled, shift);
		const BigInt remainder = big_sub(scaled, big_shift_left(quotient, shift));
		const int against_half = big_compare(remainder, big_shift_left(BigInt::from_int64(1), shift - 1));
		scaled = quotient;
		if (against_half > 0 || (against_half == 0 && quotient.is_odd())) {
			scaled = big_add(scaled, BigInt::from_int64(1));
		}
	}

	std::string digits = big_to_decimal(scaled);
	if (digits.size() < 7) {
		digits.insert(0, 7 - digits.size(), '0');
	}
	digits.insert(digits.size() - 6, 1, '.');
	if (negative) {
		digits.insert(0, 1, '-');
	}
	return digits;
}

} // namespace vm
