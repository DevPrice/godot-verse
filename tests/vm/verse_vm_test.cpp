// Standalone driver for vm/: the .vbc primitive reader (format.md §1) and the values
// (spec/values.md). No Godot and no godot-cpp, like the lexer and module-map tests beside it. A
// value case's name cites the spec table or section it checks.
#include "vbc_reader.h"
#include "vm_bigint.h"
#include "vm_cell.h"
#include "vm_equality.h"
#include "vm_heap.h"
#include "vm_number.h"
#include "vm_values.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace {

bool Step(const char *Name, bool Result) {
	printf("[verse_vm_test] %s: %s\n", Name, Result ? "ok" : "FAIL");
	return Result;
}

// Test-only LEB128/zigzag encoders, so a round trip exercises the reader against an independent
// implementation rather than only against itself.
void AppendUv(std::vector<uint8_t> &Bytes, uint64_t Value) {
	do {
		uint8_t Byte = Value & 0x7F;
		Value >>= 7;
		if (Value != 0) {
			Byte |= 0x80;
		}
		Bytes.push_back(Byte);
	} while (Value != 0);
}

void AppendSv(std::vector<uint8_t> &Bytes, int64_t Value) {
	const uint64_t Zigzag = (static_cast<uint64_t>(Value) << 1) ^ static_cast<uint64_t>(Value >> 63);
	AppendUv(Bytes, Zigzag);
}

bool TestUvRoundTrip() {
	const uint64_t Values[] = { 0, 1, 127, 128, 300, 16384, 0xFFFFFFFFULL, UINT64_MAX };
	for (uint64_t Value : Values) {
		std::vector<uint8_t> Bytes;
		AppendUv(Bytes, Value);
		VbcReader Reader = vbc_reader_make(Bytes.data(), Bytes.size());
		const uint64_t Decoded = vbc_read_uv(Reader);
		if (!Reader.ok() || Decoded != Value || Reader.offset != Bytes.size()) {
			return false;
		}
	}
	return true;
}

bool TestUvKnownEncoding() {
	// 300 is the textbook LEB128 example: 0xAC 0x02.
	const uint8_t Bytes[] = { 0xAC, 0x02 };
	VbcReader Reader = vbc_reader_make(Bytes, sizeof(Bytes));
	const uint64_t Decoded = vbc_read_uv(Reader);
	return Reader.ok() && Decoded == 300 && Reader.offset == sizeof(Bytes);
}

bool TestSvRoundTrip() {
	const int64_t Values[] = { 0, -1, 1, -2, 2, INT32_MIN, INT32_MAX, INT64_MIN, INT64_MAX };
	for (int64_t Value : Values) {
		std::vector<uint8_t> Bytes;
		AppendSv(Bytes, Value);
		VbcReader Reader = vbc_reader_make(Bytes.data(), Bytes.size());
		const int64_t Decoded = vbc_read_sv(Reader);
		if (!Reader.ok() || Decoded != Value) {
			return false;
		}
	}
	return true;
}

bool TestSvKnownEncoding() {
	// Zigzag by hand: 0->0, -1->1, 1->2, -2->3, 2->4.
	struct Case {
		uint8_t byte;
		int64_t expected;
	};
	const Case Cases[] = { { 0x00, 0 }, { 0x01, -1 }, { 0x02, 1 }, { 0x03, -2 }, { 0x04, 2 } };
	for (const Case &C : Cases) {
		VbcReader Reader = vbc_reader_make(&C.byte, 1);
		const int64_t Decoded = vbc_read_sv(Reader);
		if (!Reader.ok() || Decoded != C.expected) {
			return false;
		}
	}
	return true;
}

bool TestUvDanglingContinuationRefused() {
	const uint8_t Bytes[] = { 0x80 }; // continuation bit set, nothing follows
	VbcReader Reader = vbc_reader_make(Bytes, sizeof(Bytes));
	vbc_read_uv(Reader);
	return !Reader.ok() && !Reader.error.empty();
}

bool TestUvNeverTerminatesRefused() {
	// 10 bytes, every continuation bit set: the most format.md §1 allows, and none of them ends it.
	std::vector<uint8_t> Bytes(10, 0x80);
	VbcReader Reader = vbc_reader_make(Bytes.data(), Bytes.size());
	vbc_read_uv(Reader);
	return !Reader.ok();
}

bool TestF64RoundTrip() {
	const double Value = 1.5;
	std::vector<uint8_t> Bytes(sizeof(Value));
	std::memcpy(Bytes.data(), &Value, sizeof(Value));
	VbcReader Reader = vbc_reader_make(Bytes.data(), Bytes.size());
	const double Decoded = vbc_read_f64(Reader);
	return Reader.ok() && Decoded == Value;
}

bool TestF64TruncationRefused() {
	const uint8_t Bytes[4] = {};
	VbcReader Reader = vbc_reader_make(Bytes, sizeof(Bytes));
	vbc_read_f64(Reader);
	return !Reader.ok();
}

bool TestStrRoundTrip() {
	const std::string Text = "verse";
	std::vector<uint8_t> Bytes;
	AppendUv(Bytes, Text.size());
	Bytes.insert(Bytes.end(), Text.begin(), Text.end());
	VbcReader Reader = vbc_reader_make(Bytes.data(), Bytes.size());
	const std::string Decoded = vbc_read_str(Reader);
	return Reader.ok() && Decoded == Text && Reader.offset == Bytes.size();
}

bool TestStrTruncationRefused() {
	std::vector<uint8_t> Bytes;
	AppendUv(Bytes, 10); // claims 10 bytes of body, supplies none
	VbcReader Reader = vbc_reader_make(Bytes.data(), Bytes.size());
	vbc_read_str(Reader);
	return !Reader.ok();
}

bool TestRefIsUv() {
	std::vector<uint8_t> Bytes;
	AppendUv(Bytes, 42);
	VbcReader Reader = vbc_reader_make(Bytes.data(), Bytes.size());
	return vbc_read_ref(Reader) == 42 && Reader.ok();
}

bool TestFailureIsSticky() {
	// Once truncated, later reads answer a defined placeholder and stop moving the cursor.
	const uint8_t Bytes[] = { 0x80 };
	VbcReader Reader = vbc_reader_make(Bytes, sizeof(Bytes));
	vbc_read_uv(Reader);
	const size_t OffsetAfterFailure = Reader.offset;
	const uint64_t Second = vbc_read_uv(Reader);
	return !Reader.ok() && Second == 0 && Reader.offset == OffsetAfterFailure;
}

using namespace vm;

class Cases {
public:
	bool all_ok = true;

	void check(const std::string &p_name, bool p_result) {
		all_ok &= Step(p_name.c_str(), p_result);
	}
};

Value Int(Heap &r_heap, int64_t p_value) {
	return make_int(r_heap, p_value);
}

Value Pow2(Heap &r_heap, size_t p_exponent) {
	return make_int(r_heap, big_shift_left(BigInt::from_int64(1), p_exponent));
}

Value F(double p_value) {
	return Value::from_float(p_value);
}

Value Str(Heap &r_heap, const char *p_text) {
	return make_string(r_heap, p_text);
}

Value Arr(Heap &r_heap, std::initializer_list<Value> p_elements, bool p_mutable = false) {
	return make_array(r_heap, std::vector<Value>(p_elements), p_mutable);
}

Value Add(Heap &r_heap, Value p_left, Value p_right) {
	Value result = Value::uninitialized();
	return value_add(r_heap, p_left, p_right, result) == Outcome::Ok ? result : Value::uninitialized();
}

Value Sub(Heap &r_heap, Value p_left, Value p_right) {
	Value result = Value::uninitialized();
	return value_sub(r_heap, p_left, p_right, result) == Outcome::Ok ? result : Value::uninitialized();
}

Value Mul(Heap &r_heap, Value p_left, Value p_right) {
	Value result = Value::uninitialized();
	return value_mul(r_heap, p_left, p_right, result) == Outcome::Ok ? result : Value::uninitialized();
}

Value Div(Heap &r_heap, Value p_left, Value p_right) {
	Value result = Value::uninitialized();
	return value_div(r_heap, p_left, p_right, result) == Outcome::Ok ? result : Value::uninitialized();
}

Value Neg(Heap &r_heap, Value p_operand) {
	Value result = Value::uninitialized();
	return value_neg(r_heap, p_operand, result) == Outcome::Ok ? result : Value::uninitialized();
}

// Every value a test prints, in a form a case can compare against.
std::string Show(Value p_value) {
	p_value = follow(p_value);
	if (is_int(p_value)) {
		return big_to_decimal(int_value(p_value));
	}
	if (p_value.is_float()) {
		return float_to_string(p_value.as_float());
	}
	if (p_value.is_char8()) {
		return std::string("'") + char(p_value.as_char8()) + "'";
	}
	if (is_cell_kind(p_value, CellKind::False)) {
		return "false";
	}
	if (is_cell_kind(p_value, CellKind::True)) {
		return "true";
	}
	std::string text;
	if (string_bytes(p_value, text) && cell_as<ArrayCell>(p_value)->length() > 0) {
		return "\"" + text + "\"";
	}
	if (is_cell_kind(p_value, CellKind::Array) || is_cell_kind(p_value, CellKind::MutableArray)) {
		const ArrayCell *array = cell_as<ArrayCell>(p_value);
		std::string shown = "[";
		for (size_t i = 0; i < array->length(); ++i) {
			shown += (i ? "," : "") + Show(array->get(i));
		}
		return shown + "]";
	}
	if (is_cell_kind(p_value, CellKind::Map) || is_cell_kind(p_value, CellKind::MutableMap)) {
		std::string shown = "{";
		bool first = true;
		for (const MapEntry &entry : cell_as<MapCell>(p_value)->entries) {
			shown += (first ? "" : ",") + Show(entry.key) + "=>" + Show(entry.value);
			first = false;
		}
		return shown + "}";
	}
	if (is_cell_kind(p_value, CellKind::Option)) {
		return "option{" + Show(cell_as<OptionCell>(p_value)->content) + "}";
	}
	if (is_rational(p_value)) {
		const RationalCell *rational = cell_as<RationalCell>(p_value);
		return big_to_decimal(rational->numerator) + "/" + big_to_decimal(rational->denominator);
	}
	return "<cell>";
}

std::string IntText(Value p_value) {
	std::string text;
	RuntimeError error;
	return int_to_string(p_value, text, error) == Outcome::Ok ? text : "<raise>";
}

bool Holds(OrderOp p_op, Value p_left, Value p_right) {
	bool holds = false;
	return value_order(p_op, p_left, p_right, holds) == Outcome::Ok && holds;
}

Equality Compare(Value p_left, Value p_right) {
	return values_equal(p_left, p_right);
}

bool Equal(Value p_left, Value p_right) {
	return values_equal(p_left, p_right) == Equality::Eq && hash_key(p_left) == hash_key(p_right);
}

Value MakeMap(Heap &r_heap, std::initializer_list<Value> p_keys, std::initializer_list<Value> p_values) {
	Value result = Value::uninitialized();
	make_map(r_heap, std::vector<Value>(p_keys), std::vector<Value>(p_values), result);
	return result;
}

Value Lookup(Value p_map, Value p_key) {
	Value result = Value::uninitialized();
	return map_lookup(p_map, p_key, result) == Outcome::Ok ? result : Value::uninitialized();
}

Value Index(Value p_array, Value p_index) {
	Value result = Value::uninitialized();
	return array_index(p_array, p_index, result) == Outcome::Ok ? result : Value::uninitialized();
}

bool Fails(Outcome p_outcome) {
	return p_outcome == Outcome::Fail;
}

const ObjectCell *MakeObject(Heap &r_heap, const ClassCell *p_class, std::initializer_list<const char *> p_names, std::initializer_list<Value> p_values) {
	ObjectCell *object = r_heap.make<ObjectCell>();
	object->object_class = p_class;
	for (const char *name : p_names) {
		object->field_names.push_back(r_heap.intern(name));
	}
	object->field_values = std::vector<Value>(p_values);
	return object;
}

void EncodingCases(Cases &r_cases) {
	Heap heap;
	r_cases.check("encoding: the empty slot is all-zero bits and no Verse value", Value().is_empty() && !Value().is_float() && !Value().is_int32());
	r_cases.check("encoding: int32 round trips at both ends", Value::from_int32(INT32_MIN).as_int32() == INT32_MIN && Value::from_int32(INT32_MAX).as_int32() == INT32_MAX && Value::from_int32(-1).is_int32());
	r_cases.check("encoding: char and char32 are distinct tags", Value::from_char8(0xFF).as_char8() == 0xFF && Value::from_char32(0x10FFFF).as_char32() == 0x10FFFF && !Value::from_char8(97).same(Value::from_char32(97)));
	r_cases.check("encoding: -0.0 is kept, +0.0 is a float", std::signbit(F(-0.0).as_float()) && F(0.0).is_float() && F(0.0).as_float() == 0.0);
	r_cases.check("encoding: every NaN is one pattern", F(std::nan("")).same(F(-std::numeric_limits<double>::quiet_NaN())) && std::isnan(F(std::nan("7")).as_float()));
	r_cases.check("encoding: infinities and the extremes are floats", F(HUGE_VAL).as_float() == HUGE_VAL && F(-HUGE_VAL).is_float() && F(4.9406564584124654e-324).as_float() == 4.9406564584124654e-324 && F(-1.7976931348623157e308).is_float());
	r_cases.check("encoding: uninitialized is its own value", Value::uninitialized().is_uninitialized() && !Value::uninitialized().is_empty() && !Value::uninitialized().is_float());
	const Value cell = heap.false_value();
	r_cases.check("encoding: a cell pointer round trips", cell.is_cell() && cell.as_cell()->kind == CellKind::False && !cell.is_float());
	r_cases.check("heap: interning answers one cell per contents", heap.intern("X") == heap.intern(std::string("X")) && heap.intern("X") != heap.intern("Y"));
}

void IntegerCases(Cases &r_cases) {
	Heap heap;
	const Value int32_max = Int(heap, INT32_MAX);
	const Value crossed = Add(heap, int32_max, Int(heap, 1));
	r_cases.check("values §2.1: 2147483647 + 1 is a heap int printing 2147483648", is_cell_kind(crossed, CellKind::HeapInt) && IntText(crossed) == "2147483648");
	r_cases.check("values §2.1: a heap result that fits comes back immediate", Sub(heap, crossed, Int(heap, 1)).is_int32() && Sub(heap, crossed, Int(heap, 1)).as_int32() == INT32_MAX);
	r_cases.check("values §2.1: -2147483648 - 1 crosses and returns", IntText(Sub(heap, Int(heap, INT32_MIN), Int(heap, 1))) == "-2147483649" && Add(heap, Sub(heap, Int(heap, INT32_MIN), Int(heap, 1)), Int(heap, 1)).is_int32());
	r_cases.check("values §2.1: Neg of int32 min is a heap int", IntText(Neg(heap, Int(heap, INT32_MIN))) == "2147483648");

	const Value max64 = Int(heap, INT64_MAX);
	const Value min64 = Int(heap, INT64_MIN);
	const Value past = Add(heap, max64, Int(heap, 1));
	r_cases.check("facts §4: 9223372036854775807 + 1 > int64 max and not < 0", Holds(OrderOp::Gt, past, max64) && !Holds(OrderOp::Lt, past, Int(heap, 0)) && !Equal(past, max64));
	r_cases.check("facts §4: int64 max + int64 max > int64 max", Holds(OrderOp::Gt, Add(heap, max64, max64), max64));
	r_cases.check("values §2.1: (2^63-1)+1-1 prints 9223372036854775807", IntText(Sub(heap, past, Int(heap, 1))) == "9223372036854775807");
	const Value square32 = Mul(heap, Pow2(heap, 32), Pow2(heap, 32));
	Value floored = Value::uninitialized();
	rational_floor(heap, Div(heap, square32, Pow2(heap, 32)), floored);
	r_cases.check("values §2.1: 2^32 * 2^32 > 2^63-1 and divides back to 2^32", Holds(OrderOp::Gt, square32, max64) && Equal(floored, Pow2(heap, 32)));
	r_cases.check("values §2.1: -(-2^63) exceeds 2^63-1", Holds(OrderOp::Gt, Neg(heap, min64), max64) && Show(Neg(heap, min64)) == "9223372036854775808");
	r_cases.check("ops Sub: -(2^63) - 1 + 1 prints -9223372036854775808", IntText(Add(heap, Sub(heap, min64, Int(heap, 1)), Int(heap, 1))) == "-9223372036854775808");
	r_cases.check("ops Mul: (-2^63) * -1 equals 2^63", Equal(Mul(heap, min64, Int(heap, -1)), Pow2(heap, 63)));
	const Value max_squared = Mul(heap, max64, max64);
	rational_floor(heap, Div(heap, max_squared, max64), floored);
	r_cases.check("ops Div: Floor((2^63-1)^2 / (2^63-1)) is 2^63-1", Holds(OrderOp::Gt, max_squared, max64) && Equal(floored, max64));
	r_cases.check("bignum: 2^64 * 2^64 is 2^128 exactly", Show(Mul(heap, Pow2(heap, 64), Pow2(heap, 64))) == "340282366920938463463374607431768211456");
	r_cases.check("bignum: 2^200 - 1 in decimal", Show(Sub(heap, Pow2(heap, 200), Int(heap, 1))) == "1606938044258990275541962092341162602522202993782792835301375");
	r_cases.check("bignum: 10^30 / 7 floors and ceils correctly", [&] {
		Value ten30 = Int(heap, 1);
		for (int i = 0; i < 30; ++i) {
			ten30 = Mul(heap, ten30, Int(heap, 10));
		}
		Value down = Value::uninitialized();
		Value up = Value::uninitialized();
		rational_floor(heap, Div(heap, ten30, Int(heap, 7)), down);
		rational_ceil(heap, Div(heap, Neg(heap, ten30), Int(heap, 7)), up);
		return Show(down) == "142857142857142857142857142857" && Show(up) == "-142857142857142857142857142857";
	}());

	bool invariants = true;
	uint64_t seed = 0x243F6A8885A308D3ULL;
	auto next = [&seed] {
		seed ^= seed << 13;
		seed ^= seed >> 7;
		seed ^= seed << 17;
		return seed;
	};
	for (int round = 0; round < 300 && invariants; ++round) {
		BigInt a;
		BigInt b;
		const size_t a_limbs = 1 + next() % 7;
		const size_t b_limbs = 1 + next() % 4;
		for (size_t i = 0; i < a_limbs; ++i) {
			a.limbs.push_back(uint32_t(next()));
		}
		for (size_t i = 0; i < b_limbs; ++i) {
			b.limbs.push_back(uint32_t(round % 5 == 0 ? 0xFFFFFFFFu : next()));
		}
		a.negative = (next() & 1) != 0;
		b.negative = (next() & 1) != 0;
		a.trim();
		b.trim();
		if (b.is_zero()) {
			continue;
		}
		BigInt quotient;
		BigInt remainder;
		big_divmod_trunc(a, b, quotient, remainder);
		invariants &= big_compare(big_add(big_mul(quotient, b), remainder), a) == 0;
		invariants &= big_compare_magnitude(remainder, b) < 0;
		invariants &= remainder.is_zero() || remainder.negative == a.negative;
		invariants &= big_compare(big_sub(big_add(a, b), b), a) == 0;
		big_divmod_trunc(big_mul(a, b), b, quotient, remainder);
		invariants &= remainder.is_zero() && big_compare(quotient, a) == 0;
	}
	r_cases.check("bignum: 300 random divmod, add/sub and mul/div identities hold", invariants);

	RuntimeError error;
	std::string text;
	r_cases.check("values §2.6: ToString(2^63) raises with the exact three strings",
			int_to_string(Pow2(heap, 63), text, error) == Outcome::Error && error.diagnostic == "ErrRuntime_GeneratedNativeInternal" &&
					error.description == "An internal runtime error occurred in (generated) native code that was called from Verse. There is no other information available." &&
					error.message == "Value exceeds the range of a 64 bit integer.");
	r_cases.check("values §2.6: ToString(-2^63 - 1) raises", int_to_string(Sub(heap, min64, Int(heap, 1)), text, error) == Outcome::Error);
	r_cases.check("facts §4: ToString of 0, -5, 2^62, int64 max, int64 min",
			IntText(Int(heap, 0)) == "0" && IntText(Int(heap, -5)) == "-5" && IntText(Pow2(heap, 62)) == "4611686018427387904" &&
					IntText(max64) == "9223372036854775807" && IntText(min64) == "-9223372036854775808");
	int64_t converted = 0;
	r_cases.check("values §2.6: int64 conversion at the boundaries", int_to_int64(min64, converted, error) == Outcome::Ok && converted == INT64_MIN && int_to_int64(past, converted, error) == Outcome::Error);
}

void RationalCases(Cases &r_cases) {
	Heap heap;
	struct Row {
		int64_t numerator;
		int64_t denominator;
		const char *floor;
		const char *ceil;
	};
	const Row rows[] = {
		{ 7, 2, "3", "4" },
		{ -7, 2, "-4", "-3" },
		{ 7, -2, "-4", "-3" },
		{ -7, -2, "3", "4" },
		{ 6, 3, "2", "2" },
		{ 0, 5, "0", "0" },
	};
	for (const Row &row : rows) {
		const Value quotient = Div(heap, Int(heap, row.numerator), Int(heap, row.denominator));
		Value down = Value::uninitialized();
		Value up = Value::uninitialized();
		rational_floor(heap, quotient, down);
		rational_ceil(heap, quotient, up);
		r_cases.check("values §2.4 Floor/Ceil table: " + std::to_string(row.numerator) + " / " + std::to_string(row.denominator),
				is_rational(quotient) && Show(down) == row.floor && Show(up) == row.ceil);
	}
	Value down = Value::uninitialized();
	rational_floor(heap, Div(heap, Pow2(heap, 63), Int(heap, 3)), down);
	r_cases.check("values §2.4 Floor/Ceil table: 2^63 / 3 floors to 3074457345618258602", Show(down) == "3074457345618258602");
	Value up = Value::uninitialized();
	rational_floor(heap, Int(heap, 5), down);
	rational_ceil(heap, Int(heap, -5), up);
	r_cases.check("values §2.4: Floor(5) = 5, Ceil(-5) = -5", Show(down) == "5" && Show(up) == "-5");

	Value result = Value::uninitialized();
	r_cases.check("values §2.3: 7 / 0 and 0 / 0 fail", Fails(value_div(heap, Int(heap, 7), Int(heap, 0), result)) && Fails(value_div(heap, Int(heap, 0), Int(heap, 0), result)));
	r_cases.check("values §2.3: 6 / 3 is the rational 2/1, never an int", Show(Div(heap, Int(heap, 6), Int(heap, 3))) == "2/1");
	r_cases.check("values §2.4: -1 / -2 equals 1 / 2 in lowest terms", Equal(Div(heap, Int(heap, -1), Int(heap, -2)), Div(heap, Int(heap, 1), Int(heap, 2))) && Show(Div(heap, Int(heap, -1), Int(heap, -2))) == "1/2");
	r_cases.check("values §2.4: 14 / 4 equals 7 / 2", Equal(Div(heap, Int(heap, 14), Int(heap, 4)), Div(heap, Int(heap, 7), Int(heap, 2))));
	r_cases.check("values §2.4: 1 / 3 <> 2 / 3", Compare(Div(heap, Int(heap, 1), Int(heap, 3)), Div(heap, Int(heap, 2), Int(heap, 3))) == Equality::Neq);
	r_cases.check("values §2.4 / §11.2 rule 3: R = N iff denominator 1 and numerator N", Equal(Div(heap, Int(heap, 6), Int(heap, 3)), Int(heap, 2)) && Equal(Int(heap, 2), Div(heap, Int(heap, 6), Int(heap, 3))) && Compare(Div(heap, Int(heap, 7), Int(heap, 2)), Int(heap, 3)) == Equality::Neq);
	r_cases.check("values §2.4 ops level: rational arithmetic in lowest terms",
			Show(Add(heap, Div(heap, Int(heap, 1), Int(heap, 2)), Div(heap, Int(heap, 1), Int(heap, 3)))) == "5/6" &&
					Show(Sub(heap, Div(heap, Int(heap, 1), Int(heap, 2)), Int(heap, 1))) == "-1/2" &&
					Show(Mul(heap, Div(heap, Int(heap, 2), Int(heap, 3)), Int(heap, 3))) == "2/1" &&
					Show(Div(heap, Div(heap, Int(heap, 1), Int(heap, 2)), Div(heap, Int(heap, -1), Int(heap, 4)))) == "-2/1" &&
					Show(Neg(heap, Div(heap, Int(heap, 1), Int(heap, 2)))) == "-1/2");
	r_cases.check("values §2.4 ops level: Div by a zero rational fails", Fails(value_div(heap, Div(heap, Int(heap, 1), Int(heap, 2)), Div(heap, Int(heap, 0), Int(heap, 5)), result)));
	r_cases.check("values §2.4 ops level: rationals order exactly against rationals and ints",
			Holds(OrderOp::Lt, Div(heap, Int(heap, 1), Int(heap, 3)), Div(heap, Int(heap, 1), Int(heap, 2))) &&
					Holds(OrderOp::Gt, Div(heap, Int(heap, 7), Int(heap, 2)), Int(heap, 3)) &&
					Holds(OrderOp::Lte, Int(heap, 2), Div(heap, Int(heap, 6), Int(heap, 3))) &&
					!Holds(OrderOp::Lt, Int(heap, 4), Div(heap, Int(heap, 7), Int(heap, 2))));
	bool holds = false;
	r_cases.check("values §15: a rational beside a float is outside the contract", value_add(heap, Div(heap, Int(heap, 1), Int(heap, 2)), F(1.0), result) == Outcome::Invalid && value_order(OrderOp::Lt, Div(heap, Int(heap, 1), Int(heap, 2)), F(1.0), holds) == Outcome::Invalid);
}

void EuclideanCases(Cases &r_cases) {
	Heap heap;
	struct Row {
		int64_t dividend;
		int64_t divisor;
		int64_t mod;
		int64_t quotient;
	};
	const Row rows[] = {
		{ 7, 2, 1, 3 },
		{ -7, 2, 1, -4 },
		{ 7, -2, 1, -3 },
		{ -7, -2, 1, 4 },
		{ 6, 3, 0, 2 },
		{ -6, -3, 0, 2 },
		{ 0, -5, 0, 0 },
		{ INT64_MIN, 1, 0, INT64_MIN },
		{ INT64_MIN, 7, 6, -1317624576693539402 },
	};
	RuntimeError error;
	for (const Row &row : rows) {
		Value mod = Value::uninitialized();
		Value quotient = Value::uninitialized();
		const bool ok = euclidean_mod(heap, Int(heap, row.dividend), Int(heap, row.divisor), mod, error) == Outcome::Ok &&
				euclidean_quotient(heap, Int(heap, row.dividend), Int(heap, row.divisor), quotient, error) == Outcome::Ok;
		r_cases.check("facts §2 Mod/Quotient table: " + std::to_string(row.dividend) + ", " + std::to_string(row.divisor),
				ok && Equal(mod, Int(heap, row.mod)) && Equal(quotient, Int(heap, row.quotient)));
	}
	Value result = Value::uninitialized();
	r_cases.check("facts §2: Mod[7, 0] and Quotient[7, 0] fail", Fails(euclidean_mod(heap, Int(heap, 7), Int(heap, 0), result, error)) && Fails(euclidean_quotient(heap, Int(heap, 7), Int(heap, 0), result, error)));
	error = RuntimeError();
	r_cases.check("values §2.5 table: Mod[2^63, 7] raises the 64-bit range error",
			euclidean_mod(heap, Pow2(heap, 63), Int(heap, 7), result, error) == Outcome::Error && error.diagnostic == "ErrRuntime_GeneratedNativeInternal" && error.message == "Value exceeds the range of a 64 bit integer.");
	error = RuntimeError();
	r_cases.check("values §2.5 table: Mod[-2^63, -1] raises integer overflow",
			euclidean_mod(heap, Int(heap, INT64_MIN), Int(heap, -1), result, error) == Outcome::Error && error.diagnostic == "ErrRuntime_IntegerOverflow" &&
					error.description == "Integer overflow encountered." && error.message == "Integer overflow encountered.");
	error = RuntimeError();
	r_cases.check("values §2.5 table: Quotient[-2^63, -1] raises integer overflow", euclidean_quotient(heap, Int(heap, INT64_MIN), Int(heap, -1), result, error) == Outcome::Error && error.diagnostic == "ErrRuntime_IntegerOverflow");
}

void FloatPrintCases(Cases &r_cases) {
	struct Row {
		const char *name;
		double value;
		const char *printed;
	};
	const Row rows[] = {
		{ "1.0", 1.0, "1.000000" },
		{ "0.1", 0.1, "0.100000" },
		{ "1.5", 1.5, "1.500000" },
		{ "0.5", 0.5, "0.500000" },
		{ "2.5", 2.5, "2.500000" },
		{ "-0.0", -0.0, "0.000000" },
		{ "1.0 / 3.0", 1.0 / 3.0, "0.333333" },
		{ "0.1 + 0.2", 0.1 + 0.2, "0.300000" },
		{ "1.0 / 128.0 (tie)", 1.0 / 128.0, "0.007812" },
		{ "3.0 / 128.0 (tie)", 3.0 / 128.0, "0.023438" },
		{ "5.0 / 128.0 (tie)", 5.0 / 128.0, "0.039062" },
		{ "0.0000005", 0.0000005, "0.000000" },
		{ "0.0000015", 0.0000015, "0.000002" },
		{ "-0.0000001", -0.0000001, "-0.000000" },
		{ "-0.0000004", -0.0000004, "-0.000000" },
		{ "123.4567895", 123.4567895, "123.456789" },
		{ "999999.9999995", 999999.9999995, "999999.999999" },
		{ "1.0e15 + 0.3", 1.0e15 + 0.3, "1000000000000000.250000" },
		{ "123456789.125", 123456789.125, "123456789.125000" },
		{ "1.0e20", 1.0e20, "100000000000000000000.000000" },
		{ "1.0e22", 1.0e22, "10000000000000000000000.000000" },
		{ "1.0e23", 1.0e23, "99999999999999991611392.000000" },
		{ "1.0e300", 1.0e300, "1000000000000000052504760255204420248704468581108159154915854115511802457988908195786371375080447864043704443832883878176942523235360430575644792184786706982848387200926575803737830233794788090059368953234970799945081119038967640880074652742780142494579258788820056842838115669472196386865459400540160.000000" },
		{ "largest finite", 1.7976931348623157e308, "179769313486231570814527423731704356798070567525844996598917476803157260780028538760589558632766878171540458953514382464234321326889464182768467546703537516986049910576551282076245490090389328944075868508455133942304583236903222948165808559332123348274797826204144723168738177180919299881250404026184124858368.000000" },
		{ "its negation", -1.7976931348623157e308, "-179769313486231570814527423731704356798070567525844996598917476803157260780028538760589558632766878171540458953514382464234321326889464182768467546703537516986049910576551282076245490090389328944075868508455133942304583236903222948165808559332123348274797826204144723168738177180919299881250404026184124858368.000000" },
		{ "smallest normal", 2.2250738585072014e-308, "0.000000" },
		{ "smallest subnormal", 4.9406564584124654e-324, "0.000000" },
		{ "Inf", HUGE_VAL, "Inf" },
		{ "-Inf", -HUGE_VAL, "-Inf" },
		{ "NaN", std::numeric_limits<double>::quiet_NaN(), "NaN" },
		{ "-NaN", -std::numeric_limits<double>::quiet_NaN(), "NaN" },
		{ "facts §3 Inf from 1.0 / 0.0", 1.0 / F(0.0).as_float(), "Inf" },
		{ "-2.5", -2.5, "-2.500000" },
		{ "-1.0 / 128.0 (tie)", -1.0 / 128.0, "-0.007812" },
		{ "2^53 + 1 rounded", 9007199254740993.0, "9007199254740992.000000" },
	};
	for (const Row &row : rows) {
		const std::string printed = float_to_string(row.value);
		r_cases.check(std::string("values §3.4 / facts §3 printing table: ") + row.name + " -> " + row.printed, printed == row.printed);
		if (printed != row.printed) {
			printf("    got %s\n", printed.c_str());
		}
	}
}

void FloatCases(Cases &r_cases) {
	Heap heap;
	r_cases.check("values §3.1: 1 / -0.0 = Inf, -1 / -0.0 = -Inf, 0 / -0.0 = NaN",
			Show(Div(heap, F(1.0), F(-0.0))) == "Inf" && Show(Div(heap, F(-1.0), F(-0.0))) == "-Inf" && Show(Div(heap, F(0.0), F(-0.0))) == "NaN");
	r_cases.check("values §3.1: 1 / 0.0 = Inf, -1 / 0.0 = -Inf, 0 / 0.0 = NaN",
			Show(Div(heap, F(1.0), F(0.0))) == "Inf" && Show(Div(heap, F(-1.0), F(0.0))) == "-Inf" && Show(Div(heap, F(0.0), F(0.0))) == "NaN");
	r_cases.check("values §3.1: Inf - Inf and Inf * 0.0 are NaN", Show(Sub(heap, F(HUGE_VAL), F(HUGE_VAL))) == "NaN" && Show(Mul(heap, F(HUGE_VAL), F(0.0))) == "NaN");
	r_cases.check("values §3.2: Neg 0.0 is -0, which prints 0.000000 and equals 0.0", std::signbit(Neg(heap, F(0.0)).as_float()) && Show(Neg(heap, F(0.0))) == "0.000000" && Equal(Neg(heap, F(0.0)), F(0.0)));
	r_cases.check("values §3.2: a native receives +0 for -0", !std::signbit(canonical_float(-0.0)) && canonical_float(-2.5) == -2.5);
	r_cases.check("facts §5: -0.0 = 0.0, -0.0 <= 0.0, not -0.0 < 0.0", Equal(F(-0.0), F(0.0)) && Holds(OrderOp::Lte, F(-0.0), F(0.0)) && !Holds(OrderOp::Lt, F(-0.0), F(0.0)));
	r_cases.check("values §3.3: NaN > Inf fails, Inf = Inf, -Inf < -1e308", !Holds(OrderOp::Gt, F(NAN), F(HUGE_VAL)) && Equal(F(HUGE_VAL), F(HUGE_VAL)) && Holds(OrderOp::Lt, F(-HUGE_VAL), F(-1e308)));

	const Value nan = F(std::numeric_limits<double>::quiet_NaN());
	const Value other_nan = Sub(heap, F(HUGE_VAL), F(HUGE_VAL));
	const Value one = F(1.0);
	struct Row {
		const char *op;
		bool nan_nan;
		bool nan_one;
		bool one_nan;
	};
	const Row rows[] = {
		{ "=", true, false, false },
		{ "<>", false, true, true },
		{ "<", false, false, false },
		{ ">", false, false, false },
		{ "<=", true, false, false },
		{ ">=", true, false, false },
	};
	auto apply = [](const char *p_op, Value p_left, Value p_right) {
		const std::string op = p_op;
		if (op == "=") {
			return Compare(p_left, p_right) == Equality::Eq;
		}
		if (op == "<>") {
			return Compare(p_left, p_right) == Equality::Neq;
		}
		const OrderOp order = op == "<" ? OrderOp::Lt : op == ">" ? OrderOp::Gt : op == "<=" ? OrderOp::Lte : OrderOp::Gte;
		return Holds(order, p_left, p_right);
	};
	for (const Row &row : rows) {
		r_cases.check(std::string("values §3.3 / facts §1 NaN table: ") + row.op,
				apply(row.op, nan, other_nan) == row.nan_nan && apply(row.op, nan, one) == row.nan_one && apply(row.op, one, nan) == row.one_nan);
	}

	struct PrintRow {
		const char *name;
		Value value;
		const char *printed;
	};
	const PrintRow products[] = {
		{ "3 * 1.5", Mul(heap, Int(heap, 3), F(1.5)), "4.500000" },
		{ "1.5 * 3", Mul(heap, F(1.5), Int(heap, 3)), "4.500000" },
		{ "(2^53+1) * 1.0", Mul(heap, Int(heap, 9007199254740993LL), F(1.0)), "9007199254740992.000000" },
		{ "2^63 * 1.0", Mul(heap, Pow2(heap, 63), F(1.0)), "9223372036854775808.000000" },
		{ "2^1071 * 1.0", Mul(heap, Pow2(heap, 1071), F(1.0)), "Inf" },
		{ "0 * -1.0", Mul(heap, Int(heap, 0), F(-1.0)), "0.000000" },
		{ "(-2^63) * 1.0", Mul(heap, Int(heap, INT64_MIN), F(1.0)), "-9223372036854775808.000000" },
		{ "(2^63-1) * 1.0", Mul(heap, Int(heap, INT64_MAX), F(1.0)), "9223372036854775808.000000" },
		{ "0 * -1.5", Mul(heap, Int(heap, 0), F(-1.5)), "0.000000" },
		{ "-(2^1071) * 1.0", Mul(heap, Neg(heap, Pow2(heap, 1071)), F(1.0)), "-Inf" },
	};
	for (const PrintRow &row : products) {
		r_cases.check(std::string("values §2.7 / ops Mul table: ") + row.name + " prints " + row.printed, Show(row.value) == row.printed);
	}
	r_cases.check("values §2.7: int to float ties to even above 2^53", int_to_float(Add(heap, Pow2(heap, 54), Int(heap, 2))) == 18014398509481984.0 && int_to_float(Add(heap, Pow2(heap, 54), Int(heap, 6))) == 18014398509481992.0);
	r_cases.check("values §16 Q3 (chosen): 2^1024 - 2^970 rounds up to Inf", std::isinf(int_to_float(Sub(heap, Pow2(heap, 1024), Pow2(heap, 970)))) && !std::isinf(int_to_float(Sub(heap, Pow2(heap, 1024), Pow2(heap, 971)))));
	Value result = Value::uninitialized();
	r_cases.check("values §2.2: Add and Div of an int with a float are outside the contract", value_add(heap, Int(heap, 1), F(1.0), result) == Outcome::Invalid && value_div(heap, F(1.0), Int(heap, 1), result) == Outcome::Invalid);
}

void FloatToIntCases(Cases &r_cases) {
	Heap heap;
	RuntimeError error;
	Value result = Value::uninitialized();
	auto rounded = [&](double p_value, FloatRounding p_rounding) {
		Value out = Value::uninitialized();
		return float_to_int(heap, p_value, p_rounding, out, error) == Outcome::Ok ? Show(out) : std::string("<not ok>");
	};
	r_cases.check("facts §5 Round table: 2.5 -> 2, -2.5 -> -2, 0.5 -> 0, -0.5 -> 0",
			rounded(2.5, FloatRounding::Round) == "2" && rounded(-2.5, FloatRounding::Round) == "-2" && rounded(0.5, FloatRounding::Round) == "0" && rounded(-0.5, FloatRounding::Round) == "0");
	r_cases.check("facts §5 table: Floor/Ceil of -0.5 and 0.5",
			rounded(-0.5, FloatRounding::Floor) == "-1" && rounded(-0.5, FloatRounding::Ceil) == "0" && rounded(0.5, FloatRounding::Floor) == "0" && rounded(0.5, FloatRounding::Ceil) == "1");
	r_cases.check("values §3.6: Round 3.5 -> 4, 1.4999 -> 1, -3.5 -> -4", rounded(3.5, FloatRounding::Round) == "4" && rounded(1.4999, FloatRounding::Round) == "1" && rounded(-3.5, FloatRounding::Round) == "-4");
	r_cases.check("values §3.6: Int[-2.7] is -2", rounded(-2.7, FloatRounding::Truncate) == "-2");
	r_cases.check("values §3.6: Floor[Inf] and Floor[NaN] fail", Fails(float_to_int(heap, HUGE_VAL, FloatRounding::Floor, result, error)) && Fails(float_to_int(heap, NAN, FloatRounding::Floor, result, error)));
	r_cases.check("values §3.6: Floor[-2^63] is -9223372036854775808", rounded(-9.2233720368547758e18, FloatRounding::Floor) == "-9223372036854775808");
	r_cases.check("values §3.6: Floor[5e9] is a heap int 5000000000", rounded(5.0e9, FloatRounding::Floor) == "5000000000");
	error = RuntimeError();
	r_cases.check("values §3.6: Floor[1.0e19] raises with the rounded value printed",
			float_to_int(heap, 1.0e19, FloatRounding::Floor, result, error) == Outcome::Error && error.diagnostic == "ErrRuntime_IntegerBoundsExceeded" &&
					error.description == "A value does not fall inside the representable range of a Verse integer." &&
					error.message == "The value 10000000000000000000.000000 cannot be converted to an integer because it does not fall inside the representable range of a Verse integer.");
	r_cases.check("values §3.6: 2^63 itself is out of range", float_to_int(heap, 9223372036854775808.0, FloatRounding::Truncate, result, error) == Outcome::Error);
}

void CharAndStringCases(Cases &r_cases) {
	Heap heap;
	RuntimeError error;
	Value result = Value::uninitialized();
	auto to_string = [&](Value p_value) {
		Value out = Value::uninitialized();
		std::string bytes;
		return value_to_string(heap, p_value, out, error) == Outcome::Ok && string_bytes(out, bytes) ? bytes : std::string("<not ok>");
	};
	r_cases.check("values §4 table: char = and <> by value", Equal(Value::from_char8('a'), Value::from_char8('a')) && Compare(Value::from_char8('a'), Value::from_char8('b')) == Equality::Neq);
	r_cases.check("values §4: a char never equals a char32", Compare(Value::from_char8('a'), Value::from_char32('a')) == Equality::Neq);
	bool holds = false;
	r_cases.check("values §4 table: char ordering is outside the contract", value_order(OrderOp::Lt, Value::from_char8('a'), Value::from_char8('b'), holds) == Outcome::Invalid);
	r_cases.check("values §4 table: ToString(char) is one byte, unvalidated", to_string(Value::from_char8('z')) == "z" && to_string(Value::from_char8(0x80)) == "\x80");
	r_cases.check("values §4 table: ToString(char32) is UTF-8, one to four bytes",
			to_string(Value::from_char32(0x41)) == "A" && to_string(Value::from_char32(0xE9)) == "\xC3\xA9" && to_string(Value::from_char32(0x20AC)) == "\xE2\x82\xAC" && to_string(Value::from_char32(0x1F600)) == "\xF0\x9F\x98\x80");
	r_cases.check("values §16 Q4 (chosen): above U+10FFFF is U+FFFD, a surrogate its three bytes", to_string(Value::from_char32(0x110000)) == "\xEF\xBF\xBD" && to_string(Value::from_char32(0xD800)) == "\xED\xA0\x80");
	r_cases.check("values §14: ToString of int and float, and a string is itself", to_string(Int(heap, -42)) == "-42" && to_string(F(2.5)) == "2.500000" && to_string(Str(heap, "s")) == "s");
	r_cases.check("values §14: ToString of a logic is outside the contract", value_to_string(heap, heap.true_value(), result, error) == Outcome::Invalid);

	int64_t length = -1;
	r_cases.check("values §5.1 table: \"é\".Length = 2", value_length(Str(heap, "\xC3\xA9"), length) == Outcome::Ok && length == 2);
	r_cases.check("values §5.1 table: \"\".Length = 0", value_length(Str(heap, ""), length) == Outcome::Ok && length == 0);
	const Value abc = Str(heap, "abc");
	r_cases.check("values §5.1 table: S[I] is a char", Index(abc, Int(heap, 1)).same(Value::from_char8('b')));
	r_cases.check("values §5.1 table / §6.2: \"abc\"[3], [-1], [2^32], [2^63] fail",
			Fails(array_index(abc, Int(heap, 3), result)) && Fails(array_index(abc, Int(heap, -1), result)) && Fails(array_index(abc, Pow2(heap, 32), result)) && Fails(array_index(abc, Pow2(heap, 63), result)));
	r_cases.check("ops ArrayIndexFastFail: an index computed through the heap range that ends small indexes", Index(abc, Sub(heap, Pow2(heap, 32), Sub(heap, Pow2(heap, 32), Int(heap, 1)))).same(Value::from_char8('b')));
	r_cases.check("values §5.1 table: \"ab\" + \"cd\" is abcd", Show(Add(heap, Str(heap, "ab"), Str(heap, "cd"))) == "\"abcd\"");
	r_cases.check("values §5.1 table: \"ab\" <> \"abc\"", Compare(Str(heap, "ab"), Str(heap, "abc")) == Equality::Neq);
	const Value char_array = Arr(heap, { Value::from_char8('a'), Value::from_char8('b') });
	ArrayCell *general = heap.make<ArrayCell>(false);
	general->values = { Value::from_char8('a'), Value::from_char8('b') };
	r_cases.check("values §5.1 table / §6.1: array{'a','b'} = \"ab\", whatever the storage", Equal(char_array, Str(heap, "ab")) && Equal(Value::from_cell(general), Str(heap, "ab")));
	std::string bytes;
	r_cases.check("values §5.1 table: an array of chars reads as a string", string_bytes(Value::from_cell(general), bytes) && bytes == "ab");
	r_cases.check("values §5.1 table: string ordering is outside the contract", value_order(OrderOp::Lt, Str(heap, "a"), Str(heap, "b"), holds) == Outcome::Invalid);
	r_cases.check("values §5.2: false as a string has Length 0 and equals \"\"",
			value_length(heap.false_value(), length) == Outcome::Ok && length == 0 && Equal(heap.false_value(), Str(heap, "")) && string_bytes(heap.false_value(), bytes) && bytes.empty());
	r_cases.check("values §5.3: 'a' is not \"a\"", Compare(Value::from_char8('a'), Str(heap, "a")) == Equality::Neq && Compare(Str(heap, "a"), Value::from_char8('a')) == Equality::Neq);
}

void ArrayCases(Cases &r_cases) {
	Heap heap;
	Value result = Value::uninitialized();
	const Value one_two = Arr(heap, { Int(heap, 1), Int(heap, 2) });
	r_cases.check("values §6.3: false + array{1, 2} has Length 2 and is the right operand", Add(heap, heap.false_value(), one_two).same(one_two) && Add(heap, one_two, heap.false_value()).same(one_two));
	r_cases.check("ops Add: false + false is false, \"\" + \"\" has Length 0", Add(heap, heap.false_value(), heap.false_value()).same(heap.false_value()) && Show(Add(heap, Str(heap, ""), Str(heap, ""))) == "[]");
	const Value mutable_left = Arr(heap, { Int(heap, 1) }, true);
	const Value sum = Add(heap, mutable_left, Arr(heap, { F(2.5), Str(heap, "s") }));
	r_cases.check("values §6.3: Add of a mutable and an immutable array is a new immutable array", is_cell_kind(sum, CellKind::Array) && Show(sum) == "[1,2.500000,\"s\"]" && cell_as<ArrayCell>(mutable_left)->length() == 1);
	r_cases.check("values §6.3: Sub of arrays is outside the contract", value_sub(heap, one_two, one_two, result) == Outcome::Invalid);
	r_cases.check("values §6.2 table: indexing an array; out of range fails", Index(one_two, Int(heap, 1)).same(Int(heap, 2)) && Fails(array_index(one_two, Int(heap, 2), result)));
	r_cases.check("values §16.1 Q1: indexing false fails", Fails(array_index(heap.false_value(), Int(heap, 0), result)));
	r_cases.check("values §15: indexing a map with array_index, or by a float, is outside the contract", array_index(one_two, F(0.0), result) == Outcome::Invalid && array_index(Int(heap, 3), Int(heap, 0), result) == Outcome::Invalid);

	const Value built = make_array(heap, {}, true);
	for (int i = 1; i <= 4; ++i) {
		array_append(built, Int(heap, i * i));
	}
	const Cell *identity = built.as_cell();
	const bool made = array_make_immutable(built) == Outcome::Ok;
	r_cases.check("values §6.4: a for-built array (ArrayAdd, InPlaceMakeImmutable) keeps its cell and equals the literal",
			made && built.as_cell() == identity && is_cell_kind(built, CellKind::Array) && Equal(built, Arr(heap, { Int(heap, 1), Int(heap, 4), Int(heap, 9), Int(heap, 16) })) && Index(built, Int(heap, 3)).same(Int(heap, 16)));
	r_cases.check("values §15: ArrayAdd and InPlaceMakeImmutable on an immutable array are outside the contract", array_append(built, Int(heap, 0)) == Outcome::Invalid && array_make_immutable(built) == Outcome::Invalid);

	const Value tuple = Arr(heap, { Int(heap, 3), Int(heap, 4) });
	r_cases.check("values §10: a tuple equals the array of the same elements", Equal(tuple, Arr(heap, { Int(heap, 3), Int(heap, 4) })));
	r_cases.check("values §10: (1,2) = (1,2), (1,2) <> (1,3)", Equal(Arr(heap, { Int(heap, 1), Int(heap, 2) }), Arr(heap, { Int(heap, 1), Int(heap, 2) })) && Compare(Arr(heap, { Int(heap, 1), Int(heap, 2) }), Arr(heap, { Int(heap, 1), Int(heap, 3) })) == Equality::Neq);
	int64_t length = 0;
	r_cases.check("values §6.2: Length of a mutable array, a map and false", value_length(mutable_left, length) == Outcome::Ok && length == 1 && value_length(heap.false_value(), length) == Outcome::Ok && length == 0 && value_length(Int(heap, 1), length) == Outcome::Invalid);
}

void MeltFreezeCases(Cases &r_cases) {
	Heap heap;
	Value result = Value::uninitialized();
	Value old = Value::uninitialized();

	Value var_a = Value::uninitialized();
	melt(heap, Arr(heap, { Int(heap, 1), Int(heap, 2), Int(heap, 3) }), var_a);
	Value b = Value::uninitialized();
	freeze(heap, var_a, b);
	array_set(var_a, Int(heap, 0), Int(heap, 9), old);
	Value a_now = Value::uninitialized();
	freeze(heap, var_a, a_now);
	r_cases.check("values §7 table row 1: B := A; set A[0] = 9 leaves B[0] = 1", Index(b, Int(heap, 0)).same(Int(heap, 1)) && Index(a_now, Int(heap, 0)).same(Int(heap, 9)) && old.same(Int(heap, 1)));

	Value var_m = Value::uninitialized();
	melt(heap, Arr(heap, { Arr(heap, { Int(heap, 1), Int(heap, 2) }), Arr(heap, { Int(heap, 3) }) }), var_m);
	Value n = Value::uninitialized();
	freeze(heap, var_m, n);
	const Value inner = Index(var_m, Int(heap, 0));
	array_set(inner, Int(heap, 1), Int(heap, 7), old);
	Value m_now = Value::uninitialized();
	freeze(heap, var_m, m_now);
	r_cases.check("values §7 table row 2: the copy is deep", is_cell_kind(inner, CellKind::MutableArray) && Show(n) == "[[1,2],[3]]" && Show(m_now) == "[[1,7],[3]]" && is_cell_kind(Index(m_now, Int(heap, 0)), CellKind::Array));

	Value parked = Value::uninitialized();
	array_fast_append(heap, var_a, Arr(heap, { Int(heap, 4), Int(heap, 5) }), parked);
	int64_t length = 0;
	value_length(var_a, length);
	int64_t b_length = 0;
	value_length(b, b_length);
	r_cases.check("values §7 table row 3: set A += array{4,5} after B := A", length == 5 && b_length == 3);
	Value var_c = Value::uninitialized();
	Value frozen_a = Value::uninitialized();
	freeze(heap, var_a, frozen_a);
	melt(heap, frozen_a, var_c);
	array_fast_append(heap, var_c, Arr(heap, { Int(heap, 6) }), parked);
	int64_t c_length = 0;
	value_length(var_c, c_length);
	value_length(var_a, length);
	r_cases.check("values §7 table row 4: var C := A; set C += array{6}", c_length == 6 && length == 5);
	r_cases.check("values §6.4 FastAppendToArray: appended elements are melted", [&] {
		Value target = make_array(heap, {}, true);
		array_fast_append(heap, target, Arr(heap, { Arr(heap, { Int(heap, 1) }) }), parked);
		return is_cell_kind(Index(target, Int(heap, 0)), CellKind::MutableArray);
	}());
	r_cases.check("failure §6.1: undoing an append is a truncate", [&] {
		cell_as<ArrayCell>(var_c)->truncate(3);
		value_length(var_c, c_length);
		return c_length == 3;
	}());

	const Value five = Int(heap, 5);
	const Value option = make_option(heap, Arr(heap, { five }));
	Value melted_option = Value::uninitialized();
	melt(heap, option, melted_option);
	r_cases.check("values §7 Melt table: an option is a new option with melted content", !melted_option.same(option) && is_cell_kind(cell_as<OptionCell>(melted_option)->content, CellKind::MutableArray));
	r_cases.check("values §7 Melt table: true is itself; ints, floats, chars, false are themselves",
			melt(heap, heap.true_value(), result) == Outcome::Ok && result.same(heap.true_value()) && melt(heap, five, result) == Outcome::Ok && result.same(five) && melt(heap, heap.false_value(), result) == Outcome::Ok && result.same(heap.false_value()));
	const Value map = MakeMap(heap, { Int(heap, 2), Int(heap, 1) }, { Arr(heap, { five }), Str(heap, "x") });
	Value melted_map = Value::uninitialized();
	melt(heap, map, melted_map);
	Value frozen_map = Value::uninitialized();
	freeze(heap, melted_map, frozen_map);
	r_cases.check("values §7 / §8.1: melting and freezing a map keeps its order, values deep", is_cell_kind(melted_map, CellKind::MutableMap) && is_cell_kind(cell_as<MapCell>(melted_map)->entries[0].value, CellKind::MutableArray) && Show(frozen_map) == "{2=>[5],1=>\"x\"}" && Equal(frozen_map, map));
	r_cases.check("values §7: Freeze of an immutable array or map is outside the contract", freeze(heap, Arr(heap, { five }), result) == Outcome::Invalid && freeze(heap, map, result) == Outcome::Invalid);

	ClassCell *point = heap.make<ClassCell>();
	point->class_kind = ClassKind::Struct;
	const ObjectCell *value = MakeObject(heap, point, { "X", "Y" }, { Arr(heap, { five }), F(1.0) });
	Value melted_struct = Value::uninitialized();
	melt(heap, Value::from_cell(value), melted_struct);
	r_cases.check("values §7 Melt table: a struct is a new struct with melted fields", !melted_struct.same(Value::from_cell(value)) && is_cell_kind(cell_as<ObjectCell>(melted_struct)->field_values[0], CellKind::MutableArray) && Equal(melted_struct, Value::from_cell(value)));
	ClassCell *plain = heap.make<ClassCell>();
	const ObjectCell *instance = MakeObject(heap, plain, { "X" }, { five });
	r_cases.check("values §7 Melt table: a class instance is not copied", melt(heap, Value::from_cell(instance), result) == Outcome::Ok && result.same(Value::from_cell(instance)));

	PlaceholderCell *placeholder = heap.make<PlaceholderCell>();
	r_cases.check("values §7: melting an unbound placeholder anywhere waits on it",
			melt(heap, Arr(heap, { five, Value::from_cell(placeholder) }), result) == Outcome::Park && result.same(Value::from_cell(placeholder)));
	r_cases.check("ops FastAppendToArray: an unbound element parks and appends nothing", [&] {
		const Value target = Arr(heap, { five }, true);
		const Outcome outcome = array_fast_append(heap, target, Arr(heap, { five, Value::from_cell(placeholder) }), parked);
		value_length(target, length);
		return outcome == Outcome::Park && length == 1;
	}());
	r_cases.check("ops Freeze: an unbound placeholder inside is an invariant violation", freeze(heap, Arr(heap, { Value::from_cell(placeholder) }, true), result) == Outcome::Invalid);
}

void MapCases(Cases &r_cases) {
	Heap heap;
	Value result = Value::uninitialized();
	const Value a = Str(heap, "a");
	const Value b = Str(heap, "b");
	const Value c = Str(heap, "c");
	r_cases.check("values §8.1 table: a literal iterates in textual order", Show(MakeMap(heap, { Int(heap, 3), Int(heap, 1), Int(heap, 2) }, { c, a, b })) == "{3=>\"c\",1=>\"a\",2=>\"b\"}");
	r_cases.check("values §8.1 table: a repeated key takes the last value and position",
			Show(MakeMap(heap, { Int(heap, 1), Int(heap, 2), Int(heap, 1) }, { Str(heap, "x"), Str(heap, "y"), Str(heap, "z") })) == "{2=>\"y\",1=>\"z\"}");
	Value concatenated = Value::uninitialized();
	concatenate_maps(heap, MakeMap(heap, { Int(heap, 1), Int(heap, 2) }, { a, b }), MakeMap(heap, { Int(heap, 2), Int(heap, 3) }, { Str(heap, "B"), c }), concatenated);
	r_cases.check("values §8.1 table: ConcatenateMaps with an overlapping key", Show(concatenated) == "{1=>\"a\",2=>\"B\",3=>\"c\"}");
	concatenate_maps(heap, MakeMap(heap, { Int(heap, 1), Int(heap, 2) }, { a, b }), MakeMap(heap, { Int(heap, 1) }, { Str(heap, "A") }), concatenated);
	r_cases.check("values §8.1 table: ConcatenateMaps moves a repeated key to the right's position", Show(concatenated) == "{2=>\"b\",1=>\"A\"}");

	Value var_map = Value::uninitialized();
	melt(heap, MakeMap(heap, { Int(heap, 1), Int(heap, 2) }, { a, b }), var_map);
	bool inserted = false;
	Value old = Value::uninitialized();
	map_set(var_map, Int(heap, 1), Str(heap, "A"), inserted, old);
	r_cases.check("values §8.1 table: set M[K] on a present key replaces in place", !inserted && Show(old) == "\"a\"" && Show(var_map) == "{1=>\"A\",2=>\"b\"}");
	map_set(var_map, Int(heap, 0), c, inserted, old);
	r_cases.check("values §8.1 table: set M[K] on an absent key appends", inserted && Show(var_map) == "{1=>\"A\",2=>\"b\",0=>\"c\"}");
	map_remove_last(var_map);
	r_cases.check("failure §6.1: undoing an insert removes it and restores the order", Show(var_map) == "{1=>\"A\",2=>\"b\"}" && Fails(map_lookup(var_map, Int(heap, 0), result)));
	r_cases.check("values §15: set on an immutable map is outside the contract", map_set(MakeMap(heap, {}, {}), Int(heap, 0), a, inserted, old) == Outcome::Invalid);

	const Value letters = MakeMap(heap, { Int(heap, 1), Int(heap, 2) }, { a, b });
	r_cases.check("values §8.2: lookup finds an equal key and fails on a missing one", Show(Lookup(letters, Int(heap, 2))) == "\"b\"" && Fails(map_lookup(letters, Int(heap, 9), result)));
	r_cases.check("values §8.2: MapKey and MapValue by position", map_key_at(letters, Int(heap, 1), result) == Outcome::Ok && result.same(Int(heap, 2)) && map_value_at(letters, Int(heap, 0), result) == Outcome::Ok && Show(result) == "\"a\"");
	r_cases.check("values §8.4: map equality is positional", Equal(letters, MakeMap(heap, { Int(heap, 1), Int(heap, 2) }, { a, b })) && Compare(letters, MakeMap(heap, { Int(heap, 2), Int(heap, 1) }, { b, a })) == Equality::Neq);
	r_cases.check("values §8.4 / §11.2 rule 4: an empty map equals an empty array, \"\" and false",
			Equal(MakeMap(heap, {}, {}), MakeMap(heap, {}, {})) && Equal(MakeMap(heap, {}, {}), Arr(heap, {})) && Equal(MakeMap(heap, {}, {}), Str(heap, "")) && Equal(MakeMap(heap, {}, {}), heap.false_value()));

	const Value signed_zero = MakeMap(heap, { F(0.0), F(-0.0) }, { Str(heap, "pos"), Str(heap, "neg") });
	r_cases.check("values §8.5 table: 0.0 and -0.0 are one entry, 0.000000 => neg", Show(signed_zero) == "{0.000000=>\"neg\"}" && Show(Lookup(signed_zero, F(0.0))) == "\"neg\"");
	const Value nan_map = MakeMap(heap, { F(NAN), F(1.0) }, { a, b });
	r_cases.check("values §8.5 table: a NaN key is found by Inf - Inf", Show(Lookup(nan_map, Sub(heap, F(HUGE_VAL), F(HUGE_VAL)))) == "\"a\"");
	const Value big_map = MakeMap(heap, { Pow2(heap, 70) }, { a });
	r_cases.check("values §8.5 table: a heap int key is found by an equal value computed separately", Show(Lookup(big_map, Mul(heap, Pow2(heap, 35), Pow2(heap, 35)))) == "\"a\"");
	const Value one_key = MakeMap(heap, { Int(heap, 1) }, { a });
	r_cases.check("ops Add: a map keyed by 1 is found by (2^63 + 1) - 2^63", Show(Lookup(one_key, Sub(heap, Add(heap, Pow2(heap, 63), Int(heap, 1)), Pow2(heap, 63)))) == "\"a\"");
	const Value string_map = MakeMap(heap, { Str(heap, "ab") }, { a });
	ArrayCell *general = heap.make<ArrayCell>(false);
	general->values = { Value::from_char8('a'), Value::from_char8('b') };
	r_cases.check("values §8.5 table: a string key is found by an equal array of chars", Show(Lookup(string_map, Value::from_cell(general))) == "\"a\"");
	r_cases.check("values §8.5: a char key is not a one-char string key", Fails(map_lookup(MakeMap(heap, { Str(heap, "a") }, { a }), Value::from_char8('a'), result)));

	ClassCell *point = heap.make<ClassCell>();
	point->class_kind = ClassKind::Struct;
	const Value p1 = Value::from_cell(MakeObject(heap, point, { "X", "Y" }, { F(0.0), Int(heap, 1) }));
	const Value p2 = Value::from_cell(MakeObject(heap, point, { "Y", "X" }, { Int(heap, 1), F(-0.0) }));
	const Value struct_map = MakeMap(heap, { p1 }, { a });
	r_cases.check("values §8.5 table: equal structs (and -0.0 fields) are one key", Show(Lookup(struct_map, p2)) == "\"a\"");
	const Value tuple_map = MakeMap(heap, { Arr(heap, { Int(heap, 1), a }) }, { b });
	r_cases.check("values §8.5 table: equal tuples are one key", Show(Lookup(tuple_map, Arr(heap, { Int(heap, 1), Str(heap, "a") }))) == "\"b\"");
	const Value option_map = MakeMap(heap, { make_option(heap, Int(heap, 5)) }, { a });
	r_cases.check("values §8.5 table: equal options are one key", Show(Lookup(option_map, make_option(heap, Int(heap, 5)))) == "\"a\"" && Fails(map_lookup(option_map, heap.false_value(), result)));
	EnumeratorCell *red = heap.make<EnumeratorCell>();
	EnumeratorCell *green = heap.make<EnumeratorCell>();
	const Value enum_map = MakeMap(heap, { Value::from_cell(red) }, { a });
	r_cases.check("values §8.5 table: the same enumerator is one key, another is not", Show(Lookup(enum_map, Value::from_cell(red))) == "\"a\"" && Fails(map_lookup(enum_map, Value::from_cell(green), result)));
	ClassCell *unique = heap.make<ClassCell>();
	unique->flags = ClassCell::FLAG_UNIQUE;
	const Value first = Value::from_cell(MakeObject(heap, unique, {}, {}));
	const Value second = Value::from_cell(MakeObject(heap, unique, {}, {}));
	const Value unique_map = MakeMap(heap, { first }, { a });
	r_cases.check("values §8.5 table: two distinct <unique> instances are two keys", Show(Lookup(unique_map, first)) == "\"a\"" && Fails(map_lookup(unique_map, second, result)));
	const Value empty_key_map = MakeMap(heap, { heap.false_value() }, { a });
	r_cases.check("ops §15.1 item 2: false, \"\", array{} and map{} are one map key",
			Show(Lookup(empty_key_map, Str(heap, ""))) == "\"a\"" && Show(Lookup(empty_key_map, Arr(heap, {}))) == "\"a\"" && Show(Lookup(empty_key_map, MakeMap(heap, {}, {}))) == "\"a\"" &&
					Show(MakeMap(heap, { Str(heap, ""), heap.false_value() }, { a, b })) == "{false=>\"b\"}");

	Value var_float = Value::uninitialized();
	melt(heap, MakeMap(heap, {}, {}), var_float);
	map_set(var_float, F(NAN), a, inserted, old);
	map_set(var_float, Sub(heap, F(HUGE_VAL), F(HUGE_VAL)), b, inserted, old);
	map_set(var_float, F(0.0), a, inserted, old);
	map_set(var_float, F(-0.0), c, inserted, old);
	int64_t length = 0;
	value_length(var_float, length);
	r_cases.check("ops CallSet: NaN and Inf - Inf are one key, 0.0 and -0.0 are one key", length == 2 && Show(var_float) == "{NaN=>\"b\",0.000000=>\"c\"}");

	std::vector<Value> keys;
	std::vector<Value> values;
	for (int i = 0; i < 200; ++i) {
		keys.push_back(Int(heap, (i * 37) % 100));
		values.push_back(Int(heap, i));
	}
	Value large = Value::uninitialized();
	make_map(heap, keys, values, large);
	bool large_ok = cell_as<MapCell>(large)->entries.size() == 100;
	for (int i = 100; i < 200 && large_ok; ++i) {
		large_ok = Lookup(large, Int(heap, (i * 37) % 100)).same(Int(heap, i)) && cell_as<MapCell>(large)->entries[size_t(i - 100)].key.same(Int(heap, (i * 37) % 100));
	}
	Value large_var = Value::uninitialized();
	melt(heap, large, large_var);
	for (int i = 0; i < 50; ++i) {
		map_set(large_var, Int(heap, 1000 + i), a, inserted, old);
	}
	for (int i = 0; i < 50; ++i) {
		map_remove_last(large_var);
	}
	large_ok = large_ok && Fails(map_lookup(large_var, Int(heap, 1000), result)) && Lookup(large_var, Int(heap, 99)).same(Lookup(large, Int(heap, 99))) && Equal(large_var, large);
	r_cases.check("values §8.1: a 200-literal map over 100 keys keeps last positions, indexed lookups and undo agree", large_ok);
	r_cases.check("values §8.1: NewMap with unequal lists is outside the contract", make_map(heap, { Int(heap, 1) }, {}, result) == Outcome::Invalid);
	PlaceholderCell *placeholder = heap.make<PlaceholderCell>();
	r_cases.check("values §8.1: NewMap waits on an unbound key", make_map(heap, { Value::from_cell(placeholder) }, { a }, result) == Outcome::Park && result.same(Value::from_cell(placeholder)));
}

void OptionCases(Cases &r_cases) {
	Heap heap;
	Value result = Value::uninitialized();
	r_cases.check("values §9 Query table: option{5}? is 5", option_query(heap, make_option(heap, Int(heap, 5)), result) == Outcome::Ok && result.same(Int(heap, 5)));
	r_cases.check("values §9 Query table: false? fails", Fails(option_query(heap, heap.false_value(), result)));
	r_cases.check("values §9 Query table: true? succeeds with false", option_query(heap, heap.true_value(), result) == Outcome::Ok && result.same(heap.false_value()));
	const Value option_false = make_option(heap, heap.false_value());
	r_cases.check("values §9: option{false} is a fresh cell whose content is false", !option_false.same(heap.true_value()) && option_query(heap, option_false, result) == Outcome::Ok && result.same(heap.false_value()));
	r_cases.check("values §9 Query table: anything else is outside the contract", option_query(heap, Int(heap, 1), result) == Outcome::Invalid);
	r_cases.check("values §9: option{false} <> true in both orders", Compare(option_false, heap.true_value()) == Equality::Neq && Compare(heap.true_value(), option_false) == Equality::Neq);
	r_cases.check("values §9: option{false} = option{false}", Equal(option_false, make_option(heap, heap.false_value())));
	r_cases.check("values §9: option{option{5}} <> option{false}", Compare(make_option(heap, make_option(heap, Int(heap, 5))), option_false) == Equality::Neq);
	r_cases.check("values §9: option{5} <> false, false = false", Compare(make_option(heap, Int(heap, 5)), heap.false_value()) == Equality::Neq && Equal(heap.false_value(), heap.false_value()));
	r_cases.check("values §11.2 rule 7: an option never equals a non-option", Compare(make_option(heap, Int(heap, 5)), Int(heap, 5)) == Equality::Neq && Compare(make_option(heap, Int(heap, 5)), Arr(heap, { Int(heap, 5) })) == Equality::Neq);
}

class CountingMeeter : public PlaceholderMeeter {
public:
	int met = 0;
	Equality answer = Equality::Eq;

	Equality meet(Value, Value) override {
		++met;
		return answer;
	}
};

void EqualityCases(Cases &r_cases) {
	Heap heap;
	r_cases.check("values §11.2 rule 3: 1 <> 1.0 in both orders", Compare(Int(heap, 1), F(1.0)) == Equality::Neq && Compare(F(1.0), Int(heap, 1)) == Equality::Neq);
	r_cases.check("values §11.2 rule 3: an int never equals a char or an array", Compare(Int(heap, 97), Value::from_char8('a')) == Equality::Neq && Compare(Int(heap, 0), Arr(heap, {})) == Equality::Neq);
	r_cases.check("values §11.2 rule 3: a heap int equals only an equal heap int", Equal(Pow2(heap, 40), Pow2(heap, 40)) && Compare(Pow2(heap, 40), Pow2(heap, 41)) == Equality::Neq && Compare(Pow2(heap, 40), Int(heap, 1)) == Equality::Neq);
	r_cases.check("values §11.2 rule 4: \"\", array{}, map{} and false equal one another, and nothing else",
			Equal(Str(heap, ""), Arr(heap, {})) && Equal(heap.false_value(), Arr(heap, {}, true)) && Compare(Arr(heap, {}), Arr(heap, { Int(heap, 0) })) == Equality::Neq &&
					Compare(heap.false_value(), Str(heap, "a")) == Equality::Neq && Compare(Arr(heap, {}), heap.true_value()) == Equality::Neq);
	r_cases.check("values §11.2 rule 5: false <> true, true = true, true <> a non-empty array", Compare(heap.false_value(), heap.true_value()) == Equality::Neq && Equal(heap.true_value(), heap.true_value()) && Compare(heap.true_value(), Arr(heap, { Int(heap, 1) })) == Equality::Neq);

	EnumerationCell *colours = heap.make<EnumerationCell>();
	EnumeratorCell *red = heap.make<EnumeratorCell>();
	EnumeratorCell *green = heap.make<EnumeratorCell>();
	red->enumeration = colours;
	green->enumeration = colours;
	r_cases.check("values §11.2 rules 1 and 6: an enumerator equals only itself", Equal(Value::from_cell(red), Value::from_cell(red)) && Compare(Value::from_cell(red), Value::from_cell(green)) == Equality::Neq);

	FunctionCell *function = heap.make<FunctionCell>();
	FunctionCell *other_function = heap.make<FunctionCell>();
	r_cases.check("values §11.2 rules 1 and 7: a function equals itself, and is undecidable against another", Equal(Value::from_cell(function), Value::from_cell(function)) && Compare(Value::from_cell(function), Value::from_cell(other_function)) == Equality::Undecidable);
	r_cases.check("values §11.2 rule 8: a function against a char or a float is Neq", Compare(Value::from_cell(function), Value::from_char8('a')) == Equality::Neq && Compare(F(1.0), Value::from_cell(function)) == Equality::Neq);
	BoundedTypeCell *int_type = heap.make<BoundedTypeCell>(false);
	SimpleTypeCell *any_type = heap.make<SimpleTypeCell>();
	r_cases.check("values §13: two distinct type cells are undecidable", Compare(Value::from_cell(int_type), Value::from_cell(any_type)) == Equality::Undecidable);
	r_cases.check("values §11.2 rule 7: a function against an array is undecidable", Compare(Value::from_cell(function), Arr(heap, { Int(heap, 1) })) == Equality::Undecidable);

	ClassCell *plain = heap.make<ClassCell>();
	ClassCell *unique = heap.make<ClassCell>();
	unique->flags = ClassCell::FLAG_UNIQUE;
	const Value plain_a = Value::from_cell(MakeObject(heap, plain, {}, {}));
	const Value plain_b = Value::from_cell(MakeObject(heap, plain, {}, {}));
	const Value unique_a = Value::from_cell(MakeObject(heap, unique, {}, {}));
	const Value unique_b = Value::from_cell(MakeObject(heap, unique, {}, {}));
	r_cases.check("values §11.2 rule 7: VM-level instances -- same Eq, unique Neq, plain Undecidable, other class Neq",
			Equal(plain_a, plain_a) && Compare(unique_a, unique_b) == Equality::Neq && Compare(plain_a, plain_b) == Equality::Undecidable && Compare(plain_a, unique_a) == Equality::Neq);

	ClassCell *point = heap.make<ClassCell>();
	point->class_kind = ClassKind::Struct;
	ClassCell *other_point = heap.make<ClassCell>();
	other_point->class_kind = ClassKind::Struct;
	const Value nan_point = Value::from_cell(MakeObject(heap, point, { "X", "Y" }, { F(NAN), F(0.0) }));
	const Value nan_point_again = Value::from_cell(MakeObject(heap, point, { "X", "Y" }, { Sub(heap, F(HUGE_VAL), F(HUGE_VAL)), F(-0.0) }));
	r_cases.check("values §11.2: struct fields compare with Verse float equality (NaN, -0)", Equal(nan_point, nan_point_again));
	r_cases.check("values §11.2 rule 7: a struct against another struct class, or a differing field, is Neq",
			Compare(nan_point, Value::from_cell(MakeObject(heap, other_point, { "X", "Y" }, { F(NAN), F(0.0) }))) == Equality::Neq &&
					Compare(nan_point, Value::from_cell(MakeObject(heap, point, { "X", "Y" }, { F(1.0), F(0.0) }))) == Equality::Neq &&
					Compare(nan_point, Value::from_cell(MakeObject(heap, point, { "X" }, { F(NAN) }))) == Equality::Neq);
	r_cases.check("values §11.2 rule 7: array element answers propagate, first non-Eq wins",
			Compare(Arr(heap, { Value::from_cell(function), Int(heap, 1) }), Arr(heap, { Value::from_cell(other_function), Int(heap, 2) })) == Equality::Undecidable &&
					Compare(Arr(heap, { Int(heap, 1), Value::from_cell(function) }), Arr(heap, { Int(heap, 2), Value::from_cell(other_function) })) == Equality::Neq);

	PlaceholderCell *placeholder = heap.make<PlaceholderCell>();
	const Value pending = Arr(heap, { Int(heap, 1), Value::from_cell(placeholder) });
	CountingMeeter meeter;
	r_cases.check("values §11.1: a placeholder is handed to the meeter at its position", values_equal(pending, Arr(heap, { Int(heap, 1), Int(heap, 2) }), &meeter) == Equality::Eq && meeter.met == 1);
	meeter.met = 0;
	r_cases.check("unification §3.1: a decided difference before the placeholder never meets it", values_equal(Arr(heap, { Int(heap, 0), Value::from_cell(placeholder) }), pending, &meeter) == Equality::Neq && meeter.met == 0);
	r_cases.check("values §11.1: with no meeter a placeholder compares Eq provisionally", Compare(pending, Arr(heap, { Int(heap, 1), Int(heap, 3) })) == Equality::Eq);
	placeholder->state = PlaceholderCell::State::Bound;
	placeholder->target = Int(heap, 2);
	r_cases.check("unification §2.2: a bound placeholder reads as its value", Equal(pending, Arr(heap, { Int(heap, 1), Int(heap, 2) })) && Compare(pending, Arr(heap, { Int(heap, 1), Int(heap, 3) })) == Equality::Neq);
	PlaceholderCell *linked = heap.make<PlaceholderCell>();
	linked->state = PlaceholderCell::State::Linked;
	linked->target = Value::from_cell(placeholder);
	r_cases.check("unification §2.2: a linked placeholder reads as its root's value", follow(Value::from_cell(linked)).same(Int(heap, 2)));

	RefCell *hidden = heap.make<RefCell>(Int(heap, 7));
	hidden->hidden = true;
	const Value through = Arr(heap, { Value::from_cell(hidden) }, true);
	r_cases.check("ops §3.1: a hidden variable in a slot is read through by indexing, equality and hashing",
			Index(through, Int(heap, 0)).same(Int(heap, 7)) && Equal(through, Arr(heap, { Int(heap, 7) })));
}

void HeapCases(Cases &r_cases) {
	Heap heap;
	const size_t before = heap.live_cell_count();
	Value kept = Arr(heap, { Str(heap, "x"), make_option(heap, Pow2(heap, 40)) });
	RootScope root(heap, &kept);
	for (int i = 0; i < 100; ++i) {
		Str(heap, "garbage");
	}
	ScopeCell *scope = heap.make<ScopeCell>();
	FunctionCell *function = heap.make<FunctionCell>();
	function->parent = scope;
	scope->captures.push_back(Value::from_cell(function));
	const size_t freed = heap.collect();
	r_cases.check("heap: collect frees the unreachable, cycles included, and keeps the rooted", freed == 102 && heap.live_cell_count() == before + 4 && Show(Index(kept, Int(heap, 0))) == "\"x\"");
	heap.add_permanent_root(Value::from_cell(heap.intern("kept name")));
	r_cases.check("heap: a second collect with nothing new frees nothing", heap.collect() == 0);
}

bool RunValueCases() {
	Cases cases;
	EncodingCases(cases);
	IntegerCases(cases);
	RationalCases(cases);
	EuclideanCases(cases);
	FloatPrintCases(cases);
	FloatCases(cases);
	FloatToIntCases(cases);
	CharAndStringCases(cases);
	ArrayCases(cases);
	MeltFreezeCases(cases);
	MapCases(cases);
	OptionCases(cases);
	EqualityCases(cases);
	HeapCases(cases);
	return cases.all_ok;
}

} // namespace

int main() {
	bool AllOk = true;
	AllOk &= Step("uv round trips", TestUvRoundTrip());
	AllOk &= Step("uv matches the textbook 300 encoding", TestUvKnownEncoding());
	AllOk &= Step("sv round trips", TestSvRoundTrip());
	AllOk &= Step("sv matches zigzag by hand", TestSvKnownEncoding());
	AllOk &= Step("a uv with a dangling continuation bit is refused", TestUvDanglingContinuationRefused());
	AllOk &= Step("a uv that never terminates is refused", TestUvNeverTerminatesRefused());
	AllOk &= Step("f64 round trips", TestF64RoundTrip());
	AllOk &= Step("a truncated f64 is refused", TestF64TruncationRefused());
	AllOk &= Step("str round trips", TestStrRoundTrip());
	AllOk &= Step("a str whose body is missing is refused", TestStrTruncationRefused());
	AllOk &= Step("ref decodes as a uv", TestRefIsUv());
	AllOk &= Step("a reader that has failed stays failed", TestFailureIsSticky());
	AllOk &= RunValueCases();
	return AllOk ? 0 : 1;
}
