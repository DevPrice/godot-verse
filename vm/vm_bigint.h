#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace vm {

// An exact integer of any size: a sign and a little-endian magnitude of 32-bit limbs with no high
// zero limb. Zero is an empty magnitude and is never negative.
struct BigInt {
	bool negative = false;
	std::vector<uint32_t> limbs;

	static BigInt from_int64(int64_t p_value);
	static BigInt from_uint64(uint64_t p_value);

	bool is_zero() const { return limbs.empty(); }
	bool is_odd() const { return !limbs.empty() && (limbs[0] & 1) != 0; }
	size_t bit_length() const;
	bool test_bit(size_t p_bit) const;

	bool fits_int32() const;
	bool fits_int64() const;
	int32_t to_int32() const;
	int64_t to_int64() const;

	void trim();
};

int big_compare(const BigInt &p_left, const BigInt &p_right);
int big_compare_magnitude(const BigInt &p_left, const BigInt &p_right);

BigInt big_add(const BigInt &p_left, const BigInt &p_right);
BigInt big_sub(const BigInt &p_left, const BigInt &p_right);
BigInt big_mul(const BigInt &p_left, const BigInt &p_right);
BigInt big_neg(const BigInt &p_value);
BigInt big_abs(const BigInt &p_value);

// Truncating division, as C does it: the quotient rounds toward zero and the remainder takes the
// dividend's sign. p_divisor must not be zero.
void big_divmod_trunc(const BigInt &p_dividend, const BigInt &p_divisor, BigInt &r_quotient, BigInt &r_remainder);
// Flooring and ceiling quotients. p_divisor must not be zero.
BigInt big_div_floor(const BigInt &p_dividend, const BigInt &p_divisor);
BigInt big_div_ceil(const BigInt &p_dividend, const BigInt &p_divisor);

// Non-negative greatest common divisor; gcd(0, 0) is 0.
BigInt big_gcd(const BigInt &p_left, const BigInt &p_right);

// Magnitude shifts; the sign is kept.
BigInt big_shift_left(const BigInt &p_value, size_t p_bits);
BigInt big_shift_right(const BigInt &p_value, size_t p_bits);

// An optional '-' and the decimal digits, no leading zeros.
std::string big_to_decimal(const BigInt &p_value);

// Round to nearest, ties to even; ±Inf past the largest finite binary64 (spec/values.md §2.7).
double big_to_double(const BigInt &p_value);

// p_value must be finite and integral.
BigInt big_from_integral_double(double p_value);

} // namespace vm
