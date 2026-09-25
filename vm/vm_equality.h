#pragma once

#include <cstdint>

#include "vm_value.h"

// Equality and the map-key hash: spec/values.md §11 and §8.5.
namespace vm {

enum class Equality : uint8_t {
	Eq,
	Neq,
	Undecidable,
	Error,
};

// Called wherever the comparison meets an unbound placeholder on either side, at any depth, with
// both sides at that position. Unification binds there (spec/unification.md §3.1); Neq records it
// so it can wait (spec/ops.md Neq).
class PlaceholderMeeter {
public:
	virtual ~PlaceholderMeeter() = default;
	virtual Equality meet(Value p_left, Value p_right) = 0;
};

// With no meeter, a placeholder compares Eq provisionally (spec/values.md §11.1).
Equality values_equal(Value p_left, Value p_right, PlaceholderMeeter *p_meeter = nullptr);

// Equal keys hash equally: an integer by its value, a rational of denominator 1 as that integer,
// +0 and -0 alike, every NaN alike, a string as any array of the same chars, and `false`, every
// empty array and every empty map alike (spec/ops.md §15.1 item 2).
uint64_t hash_key(Value p_value);

} // namespace vm
