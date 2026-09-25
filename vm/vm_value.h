#pragma once

#include <cstdint>
#include <cstring>

namespace vm {

struct Cell;

// A Verse value in one 64-bit word, the same on wasm32 and x64 (design §7.3).
//
// A float is stored as the bitwise complement of its binary64 pattern, with every NaN first
// canonicalised to 0x7FF8000000000000 -- there is one observable NaN (spec/values.md §3.3). The
// complement of a canonical double never has bits 63..51 all zero, so that space is free for
// everything else: bits 50..48 are a tag and bits 47..0 the payload.
//
//   tag 0  payload 0: empty -- a fresh slot, not a Verse value (spec/calls.md §2.2); all-zero bits
//          payload 1: uninitialized (format.md §3 tag 0)
//   tag 1  int32, sign-extended from the low 32 bits. Wider integers are heap int cells
//   tag 2  char, one UTF-8 code unit
//   tag 3  char32, one code point
//   tag 4  a Cell pointer. A wasm32 pointer is 32 bits; an x64 user-space pointer fits in 47
//
// Everything else -- false, true, strings, heap ints, placeholders -- is a cell.
struct Value {
	uint64_t bits = 0;

	static constexpr uint64_t TAG_SHIFT = 48;
	static constexpr uint64_t PAYLOAD_MASK = (uint64_t(1) << TAG_SHIFT) - 1;
	static constexpr uint64_t FLOAT_TEST_SHIFT = 51;

	enum Tag : uint64_t {
		TAG_SPECIAL = 0,
		TAG_INT32 = 1,
		TAG_CHAR8 = 2,
		TAG_CHAR32 = 3,
		TAG_CELL = 4,
	};

	static constexpr Value make(Tag p_tag, uint64_t p_payload) {
		return Value{ (uint64_t(p_tag) << TAG_SHIFT) | (p_payload & PAYLOAD_MASK) };
	}

	static constexpr Value empty() { return Value{ 0 }; }
	static constexpr Value uninitialized() { return make(TAG_SPECIAL, 1); }
	static constexpr Value from_int32(int32_t p_value) { return make(TAG_INT32, uint32_t(p_value)); }
	static constexpr Value from_char8(uint8_t p_value) { return make(TAG_CHAR8, p_value); }
	static constexpr Value from_char32(uint32_t p_value) { return make(TAG_CHAR32, p_value); }
	static Value from_float(double p_value) {
		uint64_t raw;
		if (p_value != p_value) {
			raw = 0x7FF8000000000000ULL;
		} else {
			std::memcpy(&raw, &p_value, sizeof(raw));
		}
		return Value{ ~raw };
	}
	static Value from_cell(const Cell *p_cell) {
		return make(TAG_CELL, uint64_t(reinterpret_cast<uintptr_t>(p_cell)));
	}

	bool is_float() const { return (bits >> FLOAT_TEST_SHIFT) != 0; }
	Tag tag() const { return Tag(bits >> TAG_SHIFT); }
	bool is_tagged(Tag p_tag) const { return !is_float() && tag() == p_tag; }

	bool is_empty() const { return bits == 0; }
	bool is_uninitialized() const { return bits == uninitialized().bits; }
	bool is_int32() const { return is_tagged(TAG_INT32); }
	bool is_char8() const { return is_tagged(TAG_CHAR8); }
	bool is_char32() const { return is_tagged(TAG_CHAR32); }
	bool is_cell() const { return is_tagged(TAG_CELL); }

	int32_t as_int32() const { return int32_t(uint32_t(bits)); }
	uint8_t as_char8() const { return uint8_t(bits); }
	uint32_t as_char32() const { return uint32_t(bits); }
	double as_float() const {
		const uint64_t raw = ~bits;
		double result;
		std::memcpy(&result, &raw, sizeof(result));
		return result;
	}
	Cell *as_cell() const { return reinterpret_cast<Cell *>(uintptr_t(bits & PAYLOAD_MASK)); }

	// Identity, not Verse equality: the same immediate or the same cell (spec/values.md §11.2 rule 1).
	bool same(Value p_other) const { return bits == p_other.bits; }
};

static_assert(sizeof(Value) == 8);
static_assert(sizeof(void *) <= 8);

} // namespace vm
