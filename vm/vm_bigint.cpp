#include "vm_bigint.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <utility>

namespace vm {

namespace {

using Limbs = std::vector<uint32_t>;

void trim_limbs(Limbs &r_limbs) {
	while (!r_limbs.empty() && r_limbs.back() == 0) {
		r_limbs.pop_back();
	}
}

int mag_compare(const Limbs &p_left, const Limbs &p_right) {
	if (p_left.size() != p_right.size()) {
		return p_left.size() < p_right.size() ? -1 : 1;
	}
	for (size_t i = p_left.size(); i-- > 0;) {
		if (p_left[i] != p_right[i]) {
			return p_left[i] < p_right[i] ? -1 : 1;
		}
	}
	return 0;
}

Limbs mag_add(const Limbs &p_left, const Limbs &p_right) {
	const Limbs &longer = p_left.size() >= p_right.size() ? p_left : p_right;
	const Limbs &shorter = p_left.size() >= p_right.size() ? p_right : p_left;
	Limbs result(longer.size() + 1, 0);
	uint64_t carry = 0;
	for (size_t i = 0; i < longer.size(); ++i) {
		const uint64_t sum = uint64_t(longer[i]) + (i < shorter.size() ? shorter[i] : 0) + carry;
		result[i] = uint32_t(sum);
		carry = sum >> 32;
	}
	result[longer.size()] = uint32_t(carry);
	trim_limbs(result);
	return result;
}

// p_left must not be smaller than p_right.
Limbs mag_sub(const Limbs &p_left, const Limbs &p_right) {
	Limbs result(p_left.size(), 0);
	int64_t borrow = 0;
	for (size_t i = 0; i < p_left.size(); ++i) {
		int64_t diff = int64_t(p_left[i]) - (i < p_right.size() ? int64_t(p_right[i]) : 0) - borrow;
		borrow = diff < 0 ? 1 : 0;
		if (diff < 0) {
			diff += int64_t(1) << 32;
		}
		result[i] = uint32_t(diff);
	}
	trim_limbs(result);
	return result;
}

Limbs mag_mul(const Limbs &p_left, const Limbs &p_right) {
	if (p_left.empty() || p_right.empty()) {
		return {};
	}
	Limbs result(p_left.size() + p_right.size(), 0);
	for (size_t i = 0; i < p_left.size(); ++i) {
		uint64_t carry = 0;
		for (size_t j = 0; j < p_right.size(); ++j) {
			const uint64_t product = uint64_t(p_left[i]) * p_right[j] + result[i + j] + carry;
			result[i + j] = uint32_t(product);
			carry = product >> 32;
		}
		size_t k = i + p_right.size();
		while (carry != 0) {
			const uint64_t sum = uint64_t(result[k]) + carry;
			result[k] = uint32_t(sum);
			carry = sum >> 32;
			++k;
		}
	}
	trim_limbs(result);
	return result;
}

uint32_t mag_divmod_small(const Limbs &p_dividend, uint32_t p_divisor, Limbs &r_quotient) {
	r_quotient.assign(p_dividend.size(), 0);
	uint64_t remainder = 0;
	for (size_t i = p_dividend.size(); i-- > 0;) {
		const uint64_t current = (remainder << 32) | p_dividend[i];
		r_quotient[i] = uint32_t(current / p_divisor);
		remainder = current % p_divisor;
	}
	trim_limbs(r_quotient);
	return uint32_t(remainder);
}

Limbs mag_shift_left(const Limbs &p_value, size_t p_bits) {
	if (p_value.empty()) {
		return {};
	}
	const size_t whole = p_bits / 32;
	const unsigned part = unsigned(p_bits % 32);
	Limbs result(p_value.size() + whole + 1, 0);
	for (size_t i = 0; i < p_value.size(); ++i) {
		result[i + whole] |= p_value[i] << part;
		if (part != 0) {
			result[i + whole + 1] |= p_value[i] >> (32 - part);
		}
	}
	trim_limbs(result);
	return result;
}

Limbs mag_shift_right(const Limbs &p_value, size_t p_bits) {
	const size_t whole = p_bits / 32;
	if (whole >= p_value.size()) {
		return {};
	}
	const unsigned part = unsigned(p_bits % 32);
	Limbs result(p_value.size() - whole, 0);
	for (size_t i = 0; i < result.size(); ++i) {
		result[i] = p_value[i + whole] >> part;
		if (part != 0 && i + whole + 1 < p_value.size()) {
			result[i] |= p_value[i + whole + 1] << (32 - part);
		}
	}
	trim_limbs(result);
	return result;
}

// Knuth, TAOCP vol. 2, §4.3.1, Algorithm D. p_divisor is not zero.
void mag_divmod(const Limbs &p_dividend, const Limbs &p_divisor, Limbs &r_quotient, Limbs &r_remainder) {
	if (mag_compare(p_dividend, p_divisor) < 0) {
		r_quotient.clear();
		r_remainder = p_dividend;
		return;
	}
	if (p_divisor.size() == 1) {
		const uint32_t remainder = mag_divmod_small(p_dividend, p_divisor[0], r_quotient);
		r_remainder.clear();
		if (remainder != 0) {
			r_remainder.push_back(remainder);
		}
		return;
	}

	const size_t n = p_divisor.size();
	const size_t m = p_dividend.size() - n;
	const unsigned shift = unsigned(std::countl_zero(p_divisor.back()));

	Limbs vn(n, 0);
	Limbs un(p_dividend.size() + 1, 0);
	for (size_t i = 0; i < n; ++i) {
		vn[i] = p_divisor[i] << shift;
		if (shift != 0 && i > 0) {
			vn[i] |= p_divisor[i - 1] >> (32 - shift);
		}
	}
	for (size_t i = 0; i < p_dividend.size(); ++i) {
		un[i] |= p_dividend[i] << shift;
		if (shift != 0) {
			un[i + 1] |= p_dividend[i] >> (32 - shift);
		}
	}

	const uint64_t base = uint64_t(1) << 32;
	r_quotient.assign(m + 1, 0);
	for (size_t j = m + 1; j-- > 0;) {
		const uint64_t numerator = (uint64_t(un[j + n]) << 32) | un[j + n - 1];
		uint64_t qhat = numerator / vn[n - 1];
		uint64_t rhat = numerator % vn[n - 1];
		while (qhat >= base || qhat * vn[n - 2] > ((rhat << 32) | un[j + n - 2])) {
			--qhat;
			rhat += vn[n - 1];
			if (rhat >= base) {
				break;
			}
		}

		int64_t borrow = 0;
		for (size_t i = 0; i < n; ++i) {
			const uint64_t product = qhat * vn[i];
			const int64_t t = int64_t(un[i + j]) - borrow - int64_t(product & 0xFFFFFFFFULL);
			un[i + j] = uint32_t(t);
			borrow = int64_t(product >> 32) - (t >> 32);
		}
		const int64_t top = int64_t(un[j + n]) - borrow;
		un[j + n] = uint32_t(top);

		if (top < 0) {
			--qhat;
			uint64_t carry = 0;
			for (size_t i = 0; i < n; ++i) {
				const uint64_t sum = uint64_t(un[i + j]) + vn[i] + carry;
				un[i + j] = uint32_t(sum);
				carry = sum >> 32;
			}
			un[j + n] = uint32_t(uint64_t(un[j + n]) + carry);
		}
		r_quotient[j] = uint32_t(qhat);
	}
	trim_limbs(r_quotient);

	r_remainder.assign(n, 0);
	for (size_t i = 0; i < n; ++i) {
		r_remainder[i] = un[i] >> shift;
		if (shift != 0) {
			r_remainder[i] |= un[i + 1] << (32 - shift);
		}
	}
	trim_limbs(r_remainder);
}

BigInt make_big(bool p_negative, Limbs p_limbs) {
	BigInt result;
	result.limbs = std::move(p_limbs);
	result.trim();
	result.negative = p_negative && !result.limbs.empty();
	return result;
}

} // namespace

BigInt BigInt::from_uint64(uint64_t p_value) {
	BigInt result;
	if (p_value != 0) {
		result.limbs.push_back(uint32_t(p_value));
		if ((p_value >> 32) != 0) {
			result.limbs.push_back(uint32_t(p_value >> 32));
		}
	}
	return result;
}

BigInt BigInt::from_int64(int64_t p_value) {
	const uint64_t magnitude = p_value < 0 ? (~uint64_t(p_value) + 1) : uint64_t(p_value);
	BigInt result = from_uint64(magnitude);
	result.negative = p_value < 0;
	return result;
}

size_t BigInt::bit_length() const {
	if (limbs.empty()) {
		return 0;
	}
	return (limbs.size() - 1) * 32 + (32 - size_t(std::countl_zero(limbs.back())));
}

bool BigInt::test_bit(size_t p_bit) const {
	const size_t index = p_bit / 32;
	return index < limbs.size() && ((limbs[index] >> (p_bit % 32)) & 1) != 0;
}

bool BigInt::fits_int32() const {
	if (limbs.size() > 1) {
		return false;
	}
	const uint64_t magnitude = limbs.empty() ? 0 : limbs[0];
	return negative ? magnitude <= (uint64_t(1) << 31) : magnitude < (uint64_t(1) << 31);
}

bool BigInt::fits_int64() const {
	if (limbs.size() > 2) {
		return false;
	}
	uint64_t magnitude = 0;
	for (size_t i = limbs.size(); i-- > 0;) {
		magnitude = (magnitude << 32) | limbs[i];
	}
	return negative ? magnitude <= (uint64_t(1) << 63) : magnitude < (uint64_t(1) << 63);
}

int32_t BigInt::to_int32() const {
	return int32_t(to_int64());
}

int64_t BigInt::to_int64() const {
	uint64_t magnitude = 0;
	for (size_t i = std::min<size_t>(limbs.size(), 2); i-- > 0;) {
		magnitude = (magnitude << 32) | limbs[i];
	}
	return negative ? int64_t(~magnitude + 1) : int64_t(magnitude);
}

void BigInt::trim() {
	trim_limbs(limbs);
	if (limbs.empty()) {
		negative = false;
	}
}

int big_compare_magnitude(const BigInt &p_left, const BigInt &p_right) {
	return mag_compare(p_left.limbs, p_right.limbs);
}

int big_compare(const BigInt &p_left, const BigInt &p_right) {
	if (p_left.negative != p_right.negative) {
		return p_left.negative ? -1 : 1;
	}
	const int magnitude = mag_compare(p_left.limbs, p_right.limbs);
	return p_left.negative ? -magnitude : magnitude;
}

BigInt big_add(const BigInt &p_left, const BigInt &p_right) {
	if (p_left.negative == p_right.negative) {
		return make_big(p_left.negative, mag_add(p_left.limbs, p_right.limbs));
	}
	const int order = mag_compare(p_left.limbs, p_right.limbs);
	if (order == 0) {
		return BigInt();
	}
	if (order > 0) {
		return make_big(p_left.negative, mag_sub(p_left.limbs, p_right.limbs));
	}
	return make_big(p_right.negative, mag_sub(p_right.limbs, p_left.limbs));
}

BigInt big_neg(const BigInt &p_value) {
	BigInt result = p_value;
	result.negative = !p_value.negative && !p_value.limbs.empty();
	return result;
}

BigInt big_abs(const BigInt &p_value) {
	BigInt result = p_value;
	result.negative = false;
	return result;
}

BigInt big_sub(const BigInt &p_left, const BigInt &p_right) {
	return big_add(p_left, big_neg(p_right));
}

BigInt big_mul(const BigInt &p_left, const BigInt &p_right) {
	return make_big(p_left.negative != p_right.negative, mag_mul(p_left.limbs, p_right.limbs));
}

void big_divmod_trunc(const BigInt &p_dividend, const BigInt &p_divisor, BigInt &r_quotient, BigInt &r_remainder) {
	Limbs quotient;
	Limbs remainder;
	mag_divmod(p_dividend.limbs, p_divisor.limbs, quotient, remainder);
	r_quotient = make_big(p_dividend.negative != p_divisor.negative, std::move(quotient));
	r_remainder = make_big(p_dividend.negative, std::move(remainder));
}

BigInt big_div_floor(const BigInt &p_dividend, const BigInt &p_divisor) {
	BigInt quotient;
	BigInt remainder;
	big_divmod_trunc(p_dividend, p_divisor, quotient, remainder);
	if (!remainder.is_zero() && p_dividend.negative != p_divisor.negative) {
		quotient = big_sub(quotient, BigInt::from_int64(1));
	}
	return quotient;
}

BigInt big_div_ceil(const BigInt &p_dividend, const BigInt &p_divisor) {
	BigInt quotient;
	BigInt remainder;
	big_divmod_trunc(p_dividend, p_divisor, quotient, remainder);
	if (!remainder.is_zero() && p_dividend.negative == p_divisor.negative) {
		quotient = big_add(quotient, BigInt::from_int64(1));
	}
	return quotient;
}

BigInt big_gcd(const BigInt &p_left, const BigInt &p_right) {
	BigInt a = big_abs(p_left);
	BigInt b = big_abs(p_right);
	while (!b.is_zero()) {
		BigInt quotient;
		BigInt remainder;
		big_divmod_trunc(a, b, quotient, remainder);
		a = std::move(b);
		b = std::move(remainder);
	}
	return a;
}

BigInt big_shift_left(const BigInt &p_value, size_t p_bits) {
	return make_big(p_value.negative, mag_shift_left(p_value.limbs, p_bits));
}

BigInt big_shift_right(const BigInt &p_value, size_t p_bits) {
	return make_big(p_value.negative, mag_shift_right(p_value.limbs, p_bits));
}

std::string big_to_decimal(const BigInt &p_value) {
	if (p_value.is_zero()) {
		return "0";
	}
	std::string reversed;
	Limbs rest = p_value.limbs;
	while (!rest.empty()) {
		Limbs quotient;
		uint32_t chunk = mag_divmod_small(rest, 1000000000u, quotient);
		rest = std::move(quotient);
		for (int i = 0; i < 9; ++i) {
			if (rest.empty() && chunk == 0) {
				break;
			}
			reversed.push_back(char('0' + chunk % 10));
			chunk /= 10;
		}
	}
	if (p_value.negative) {
		reversed.push_back('-');
	}
	std::reverse(reversed.begin(), reversed.end());
	return reversed;
}

double big_to_double(const BigInt &p_value) {
	const size_t length = p_value.bit_length();
	if (length == 0) {
		return 0.0;
	}
	uint64_t mantissa = 0;
	size_t shift = 0;
	if (length <= 53) {
		for (size_t i = std::min<size_t>(p_value.limbs.size(), 2); i-- > 0;) {
			mantissa = (mantissa << 32) | p_value.limbs[i];
		}
	} else {
		shift = length - 53;
		const Limbs top = mag_shift_right(p_value.limbs, shift);
		mantissa = uint64_t(top[0]) | (uint64_t(top.size() > 1 ? top[1] : 0) << 32);
		const bool half_bit = p_value.test_bit(shift - 1);
		bool below_half = false;
		for (size_t bit = 0; bit + 1 < shift && !below_half; ++bit) {
			below_half = p_value.test_bit(bit);
		}
		if (half_bit && (below_half || (mantissa & 1) != 0)) {
			++mantissa;
			if (mantissa == (uint64_t(1) << 53)) {
				mantissa >>= 1;
				++shift;
			}
		}
	}
	if (shift > 1100) {
		return p_value.negative ? -HUGE_VAL : HUGE_VAL;
	}
	const double magnitude = std::ldexp(double(mantissa), int(shift));
	return p_value.negative ? -magnitude : magnitude;
}

BigInt big_from_integral_double(double p_value) {
	uint64_t raw;
	std::memcpy(&raw, &p_value, sizeof(raw));
	const bool negative = (raw >> 63) != 0;
	const int exponent = int((raw >> 52) & 0x7FF);
	if (exponent == 0) {
		return BigInt();
	}
	const uint64_t mantissa = (raw & ((uint64_t(1) << 52) - 1)) | (uint64_t(1) << 52);
	const int shift = exponent - 1075;
	BigInt result = BigInt::from_uint64(mantissa);
	result = shift >= 0 ? big_shift_left(result, size_t(shift)) : big_shift_right(result, size_t(-shift));
	if (negative) {
		result = big_neg(result);
	}
	return result;
}

} // namespace vm
