// Standalone driver for vm/: the .vbc primitive reader (format.md §1), the values
// (spec/values.md), the JSON reader, the loader's refusals (format.md §9) and the sidecar reader
// (spec/sidecar.md). No Godot and no godot-cpp, like the lexer and module-map tests beside it. A
// case's name cites the spec table or section it checks.
#include "vbc_ops.gen.h"
#include "vbc_reader.h"
#include "verse_host_abi.h"
#include "vm_bigint.h"
#include "vm_cell.h"
#include "vm_equality.h"
#include "vm_file_reader.h"
#include "vm_heap.h"
#include "vm_interpreter.h"
#include "vm_json.h"
#include "vm_loader.h"
#include "vm_natives.h"
#include "vm_number.h"
#include "vm_runtime.h"
#include "vm_values.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <limits>
#include <map>
#include <string>
#include <string_view>
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

Value Bits(Heap &r_heap, const char *p_name, std::initializer_list<Value> p_arguments) {
	const std::vector<Value> arguments(p_arguments);
	NativeCall call(r_heap);
	call.arguments = arguments.data();
	call.argument_count = uint32_t(arguments.size());
	const NativeFn native = native_implementation(std::string("(/Verse.org/Verse/(/Verse.org/Verse:)") + p_name + ":)Native");
	return native(call) == Outcome::Ok ? call.result : Value::uninitialized();
}

bool IsWideHeapInt(Value p_value) {
	return is_cell_kind(p_value, CellKind::HeapInt) && cell_as<HeapIntCell>(p_value)->is_wide;
}

bool IsNarrowHeapInt(Value p_value) {
	return is_cell_kind(p_value, CellKind::HeapInt) && !cell_as<HeapIntCell>(p_value)->is_wide;
}

void Int64Cases(Cases &r_cases) {
	Heap heap;
	const Value max64 = Int(heap, INT64_MAX);
	const Value min64 = Int(heap, INT64_MIN);
	const Value two31 = Int(heap, int64_t(1) << 31);
	r_cases.check("int64: 2^31 from int64, from a BigInt and from int32 max + 1 are one value with one hash",
			IsNarrowHeapInt(two31) && Equal(two31, Pow2(heap, 31)) && Equal(two31, Add(heap, Int(heap, INT32_MAX), Int(heap, 1))) && IsNarrowHeapInt(Pow2(heap, 31)));
	r_cases.check("int64: -2^31 - 1 is a heap int and -2^31 an immediate",
			IsNarrowHeapInt(Int(heap, int64_t(INT32_MIN) - 1)) && Int(heap, INT32_MIN).is_int32() && Sub(heap, Int(heap, int64_t(INT32_MIN) - 1), Int(heap, -1)).is_int32());
	r_cases.check("int64: 2^62 + 2^62 overflows into a wide 2^63", IsWideHeapInt(Add(heap, Pow2(heap, 62), Pow2(heap, 62))) && Equal(Add(heap, Pow2(heap, 62), Pow2(heap, 62)), Pow2(heap, 63)));
	r_cases.check("int64: 2^63 - 1 from a wide 2^63 is narrow and equals int64 max", IsNarrowHeapInt(Sub(heap, Pow2(heap, 63), Int(heap, 1))) && Equal(Sub(heap, Pow2(heap, 63), Int(heap, 1)), max64));
	r_cases.check("int64: int64 min - 1 overflows and + 1 comes back narrow",
			IsWideHeapInt(Sub(heap, min64, Int(heap, 1))) && IsNarrowHeapInt(Add(heap, Sub(heap, min64, Int(heap, 1)), Int(heap, 1))) && Equal(Add(heap, Sub(heap, min64, Int(heap, 1)), Int(heap, 1)), min64));
	r_cases.check("int64: int64 min - int64 max overflows to -(2^64 - 1)", Show(Sub(heap, min64, max64)) == "-18446744073709551615");
	r_cases.check("int64: 2^32 * 2^31 overflows to 2^63, -2^32 * 2^31 is int64 min",
			IsWideHeapInt(Mul(heap, Pow2(heap, 32), two31)) && Equal(Mul(heap, Pow2(heap, 32), two31), Pow2(heap, 63)) && IsNarrowHeapInt(Mul(heap, Int(heap, -(int64_t(1) << 32)), two31)) && Equal(Mul(heap, Int(heap, -(int64_t(1) << 32)), two31), min64));
	r_cases.check("int64: 3037000500^2 overflows exactly", Show(Mul(heap, Int(heap, 3037000500), Int(heap, 3037000500))) == "9223372037000250000");
	r_cases.check("int64: 3037000499^2 stays narrow", IsNarrowHeapInt(Mul(heap, Int(heap, 3037000499), Int(heap, 3037000499))) && Show(Mul(heap, Int(heap, 3037000499), Int(heap, 3037000499))) == "9223372030926249001");
	r_cases.check("int64: int64 min * -1 and -1 * int64 min are wide 2^63", Equal(Mul(heap, min64, Int(heap, -1)), Pow2(heap, 63)) && Equal(Mul(heap, Int(heap, -1), min64), Pow2(heap, 63)));
	r_cases.check("int64: Neg of int64 min is wide, Neg of -2^40 narrow", IsWideHeapInt(Neg(heap, min64)) && Show(Neg(heap, Int(heap, -(int64_t(1) << 40)))) == "1099511627776");
	r_cases.check("int64: ordering across immediate, narrow and wide",
			Holds(OrderOp::Lt, Int(heap, -(int64_t(1) << 40)), Int(heap, 5)) && Holds(OrderOp::Gt, Int(heap, 3000000000), Int(heap, 2000000000)) &&
					Holds(OrderOp::Lt, max64, Pow2(heap, 63)) && Holds(OrderOp::Gt, min64, Sub(heap, min64, Int(heap, 1))) && Holds(OrderOp::Lte, max64, max64) && !Holds(OrderOp::Lt, max64, max64));
	r_cases.check("int64: a narrow and a wide heap int are never equal", Compare(max64, Pow2(heap, 63)) == Equality::Neq && Compare(Pow2(heap, 63), max64) == Equality::Neq);
	r_cases.check("int64: 5000000000 reached two ways is one map key",
			Equal(Int(heap, 5000000000), Add(heap, Pow2(heap, 32), Int(heap, 705032704))) &&
					Show(Lookup(MakeMap(heap, { Int(heap, 5000000000) }, { Int(heap, 7) }), Add(heap, Pow2(heap, 32), Int(heap, 705032704)))) == Show(Int(heap, 7)));
	r_cases.check("int64: printing and float conversion of narrow values",
			IntText(Int(heap, -5000000000)) == "-5000000000" && IntText(min64) == "-9223372036854775808" && int_to_float(max64) == 9223372036854775808.0 &&
					int_to_float(Int(heap, (int64_t(1) << 53) + 1)) == 9007199254740992.0 && int_to_float(Int(heap, (int64_t(1) << 53) + 3)) == 9007199254740996.0);

	r_cases.check("natives §4 int64: BitAnd/BitOr/BitXor of negative 64-bit operands",
			Show(Bits(heap, "BitAnd", { Int(heap, -1), Int(heap, int64_t(1) << 40) })) == "1099511627776" &&
					Show(Bits(heap, "BitOr", { Int(heap, -(int64_t(1) << 40)), Int(heap, 1) })) == "-1099511627775" &&
					Show(Bits(heap, "BitXor", { Int(heap, -1), Int(heap, 5000000000) })) == "-5000000001" &&
					Show(Bits(heap, "BitAnd", { Int(heap, -5000000000), Int(heap, -3) })) == "-5000000000");
	r_cases.check("natives §4 int64: the ends of int64 combine to immediates and each other",
			Equal(Bits(heap, "BitAnd", { min64, Int(heap, -1) }), min64) && Bits(heap, "BitXor", { min64, max64 }).is_int32() && Show(Bits(heap, "BitXor", { min64, max64 })) == "-1" &&
					Bits(heap, "BitAnd", { min64, max64 }).is_int32() && Show(Bits(heap, "BitAnd", { min64, max64 })) == "0" &&
					Show(Bits(heap, "BitOr", { Int(heap, 3000000000), Int(heap, -4000000000) })) == Show(Int(heap, int64_t(3000000000) | int64_t(-4000000000))));
	r_cases.check("natives §4 int64: BitNot at both ends and in between",
			Equal(Bits(heap, "BitNot", { max64 }), min64) && Equal(Bits(heap, "BitNot", { min64 }), max64) && Show(Bits(heap, "BitNot", { Int(heap, 5000000000) })) == "-5000000001" &&
					Show(Bits(heap, "BitNot", { Int(heap, INT32_MIN) })) == "2147483647");
	r_cases.check("natives §4 wide: an operand beyond int64 takes the exact path",
			Show(Bits(heap, "BitAnd", { Add(heap, Pow2(heap, 64), Int(heap, 5)), Int(heap, -1) })) == "18446744073709551621" &&
					Show(Bits(heap, "BitAnd", { Sub(heap, Pow2(heap, 64), Int(heap, 1)), Int(heap, -(int64_t(1) << 40)) })) == "18446742974197923840" &&
					Show(Bits(heap, "BitOr", { Neg(heap, Pow2(heap, 64)), max64 })) == "-9223372036854775809" &&
					Show(Bits(heap, "BitNot", { Pow2(heap, 63) })) == "-9223372036854775809");
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

	Heap tenured;
	const Value program = Arr(tenured, { Str(tenured, "loaded"), make_option(tenured, Pow2(tenured, 40)) });
	tenured.add_permanent_root(program);
	const Value table = Arr(tenured, {}, true);
	tenured.add_permanent_root(table);
	ObjectCell *global = tenured.make<ObjectCell>();
	tenured.add_permanent_root(Value::from_cell(global));
	for (int i = 0; i < 10; ++i) {
		Str(tenured, "garbage");
	}
	const size_t loaded = tenured.live_cell_count() - 10;
	tenured.tenure();
	r_cases.check("gc tenure: tenuring collects first, and every survivor is tenured; only the mutable array and the object are remembered",
			tenured.live_cell_count() == loaded && tenured.tenured_cell_count() == loaded && tenured.remembered_cell_count() == 2 &&
					program.as_cell()->tenured && program.as_cell()->marked);
	for (int i = 0; i < 100; ++i) {
		Str(tenured, "garbage");
	}
	r_cases.check("gc tenure: a collection frees exactly the garbage made since, and keeps the tenured program",
			tenured.collect() == 100 && tenured.live_cell_count() == loaded && Show(Index(program, Int(tenured, 0))) == "\"loaded\"");
	const Value in_table = Str(tenured, "in a tenured mutable array");
	cell_as<ArrayCell>(table)->append(in_table);
	const Value in_field = Str(tenured, "in a tenured object's field");
	global->field_values.push_back(in_field);
	const NameCell *late = tenured.intern("a name interned after tenure");
	tenured.collect();
	tenured.collect();
	r_cases.check("gc tenure: an untenured cell reached only from a remembered tenured cell, or a name interned since, survives",
			tenured.owns(in_table.as_cell()) && tenured.owns(in_field.as_cell()) && tenured.owns(late) &&
					tenured.find_interned("a name interned after tenure") == late && !late->marked);
	cell_as<ArrayCell>(table)->truncate(0);
	global->field_values.clear();
	r_cases.check("gc tenure: once the remembered cell lets go, the untenured cell is garbage again", tenured.collect() == 2);
}

bool RunValueCases() {
	Cases cases;
	EncodingCases(cases);
	IntegerCases(cases);
	Int64Cases(cases);
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

bool Parses(std::string_view p_text, JsonValue &r_value) {
	std::string error;
	return json_parse(p_text, r_value, error) && error.empty();
}

bool Refused(std::string_view p_text) {
	JsonValue value;
	std::string error;
	return !json_parse(p_text, value, error) && error.find("at byte") != std::string::npos;
}

void JsonCases(Cases &r_cases) {
	JsonValue value;
	r_cases.check("json: an object of every kind parses, keys in document order",
			Parses(" {\"a\": [1, -2.5e3, true, false, null], \"b\": {\"c\": \"d\"}, \"e\": []} \r\n", value) && value.is_object() &&
					value.keys.size() == 3 && value.keys[0] == "a" && value.find("a")->items.size() == 5 &&
					value.find("a")->items[1].number == -2500.0 && value.find("a")->items[2].boolean &&
					value.find("a")->items[4].is_null() && value.find("b")->find("c")->text == "d" && value.find("e")->items.empty());
	r_cases.check("json: escapes decode, \\u to UTF-8 and a surrogate pair to one code point",
			Parses("\"q\\\" b\\\\ s\\/ \\b\\f\\n\\r\\t \\u00e9 \\ud83d\\ude00\"", value) &&
					value.text == "q\" b\\ s/ \b\f\n\r\t \xC3\xA9 \xF0\x9F\x98\x80");
	r_cases.check("json: raw UTF-8 in a string passes through", Parses("\"\xE2\x82\xAC\"", value) && value.text == "\xE2\x82\xAC");
	r_cases.check("json: a number keeps its lexeme, so an int64 reads exactly",
			Parses("[9007199254740993, -0, 3, 3.0, 9223372036854775808]", value) && value.items[0].text == "9007199254740993" && [&] {
				int64_t number = 0;
				return json_to_int64(value.items[0], number) && number == 9007199254740993LL && json_to_int64(value.items[1], number) &&
						number == 0 && !json_to_int64(value.items[3], number) && !json_to_int64(value.items[4], number);
			}());
	r_cases.check("json: a repeated key is kept and find answers the first",
			Parses("{\"k\": 1, \"k\": 2}", value) && value.keys.size() == 2 && value.find("k")->number == 1.0);
	r_cases.check("json: a trailing comma is refused", Refused("[1, 2,]") && Refused("{\"a\": 1,}"));
	r_cases.check("json: a comment is refused", Refused("[1] // x") && Refused("/* x */ 1"));
	r_cases.check("json: a leading zero, a bare sign, a dangling point or exponent are refused",
			Refused("01") && Refused("-") && Refused("1.") && Refused("1e") && Refused(".5") && Refused("+1"));
	r_cases.check("json: NaN, Infinity, single quotes and a misspelt literal are refused",
			Refused("NaN") && Refused("Infinity") && Refused("'a'") && Refused("tru") && Refused("nul"));
	r_cases.check("json: a control character in a string is refused", Refused("\"a\tb\"") && Refused("\"a\nb\""));
	r_cases.check("json: invalid UTF-8 is refused: an overlong form, a stray continuation, a surrogate, a truncated sequence",
			Refused("\"\xC0\x80\"") && Refused("\"\x80\"") && Refused("\"\xED\xA0\x80\"") && Refused("\"\xE2\x82\""));
	r_cases.check("json: an unpaired surrogate escape is refused", Refused("\"\\ud800\"") && Refused("\"\\udc00\"") && Refused("\"\\ud800\\u0041\""));
	r_cases.check("json: an unknown escape and a short \\u are refused", Refused("\"\\x41\"") && Refused("\"\\u12\""));
	r_cases.check("json: text after the value, an empty document and an unterminated one are refused",
			Refused("{} x") && Refused("") && Refused("   ") && Refused("[1") && Refused("{\"a\"") && Refused("\"abc"));
	r_cases.check("json: a missing colon or a non-string key is refused", Refused("{\"a\" 1}") && Refused("{1: 2}"));
	r_cases.check("json: nesting past the limit is refused rather than recursing without bound",
			Refused(std::string(300, '[') + std::string(300, ']')) && Parses(std::string(100, '[') + std::string(100, ']'), value));
}

// A .vbc written by hand, one field at a time, in format.md's order.
class VbcBuilder {
public:
	std::vector<uint8_t> bytes;

	void u8(uint8_t p_value) { bytes.push_back(p_value); }
	void uv(uint64_t p_value) { AppendUv(bytes, p_value); }
	void sv(int64_t p_value) { AppendSv(bytes, p_value); }
	void str(std::string_view p_text) {
		uv(p_text.size());
		bytes.insert(bytes.end(), p_text.begin(), p_text.end());
	}
};

struct ProgramShape {
	const char *magic = "VBC1";
	uint64_t version = 1;
	uint64_t abi = VH_ABI_VERSION;
	std::string digest = vbc::kVbcOpsSchemaDigest;
	std::string host_id = "d78f612c313953e9";
	uint64_t generation = 1;
	uint64_t package_ref = 4;
	bool accessor_role = true;
	std::vector<uint64_t> extra_opcodes;
};

enum : uint64_t {
	kSidTask,
	kSidAccessor,
	kSidTaskRole,
	kSidAccessorRole,
	kSidPackage,
	kSidRoot,
	kSidProcedure,
	kSidFile,
	kSidNativeKey,
	kSidNativeName,
	kSidScriptClass,
	kSidNode2d,
	kSidEmpty,
	kSidCount,
};

// Seven cells: 0 a script class whose superclass is 6, 1 an enumeration of 2, 3 a package defining
// 4, 4 a procedure, 5 a native procedure, 6 a mirrored class `node2d`.
std::vector<uint8_t> BuildProgram(const ProgramShape &p_shape) {
	VbcBuilder b;
	for (int i = 0; i < 4; ++i) {
		b.u8(uint8_t(p_shape.magic[i]));
	}
	b.uv(p_shape.version);
	b.uv(p_shape.abi);
	b.str(p_shape.host_id);
	b.str("203d764");
	b.str(p_shape.digest);
	b.uv(p_shape.generation);

	const char *strings[kSidCount] = { "task", "accessor", "task_class", "accessor_enumerator", "GodotScripts_1", "/user@localhost", "(/user@localhost/x:)Race",
		"C:/x.verse", "(/Verse.org/Verse/(/Verse.org/Verse:)NoSuchNative(:float):)Native", "(/Verse.org/Verse:)NoSuchNative(:float)", "script_class", "node2d", "" };
	b.uv(kSidCount);
	for (const char *text : strings) {
		b.str(text);
	}

	b.uv(7);
	const auto class_cell = [&](uint64_t p_name, bool p_inherits) {
		b.u8(16);
		b.u8(0);
		b.uv(4096);
		b.uv(4);
		b.uv(kSidEmpty);
		b.uv(p_name);
		b.u8(0);
		b.uv(p_inherits ? 1 : 0);
		if (p_inherits) {
			b.uv(7);
		}
		b.uv(0);
		b.uv(0);
		b.uv(0);
		b.u8(0);
	};
	class_cell(kSidTask, true);

	b.u8(19);
	b.uv(kSidAccessor);
	b.uv(1);
	b.uv(3);

	b.u8(20);
	b.uv(2);
	b.uv(kSidAccessor);
	b.uv(0);

	b.u8(24);
	b.uv(kSidPackage);
	b.uv(kSidRoot);
	b.uv(2);
	b.uv(kSidProcedure);
	b.u8(5);
	b.uv(5);
	b.uv(kSidNativeKey);
	b.u8(5);
	b.uv(6);

	b.u8(12);
	b.uv(kSidProcedure);
	b.uv(kSidFile);
	b.uv(0);
	b.uv(2);
	b.uv(0);
	b.uv(0);
	b.uv(1);
	b.u8(1);
	b.sv(7);
	b.uv(2 + p_shape.extra_opcodes.size());
	// EndTask: Write r0 present, Switch absent, Value c0, Which r1, Signal absent. The two optional
	// registers are the operands whose u8 present flag a reader must never skip (format.md §5.1).
	b.uv(uint64_t(vbc::VbcOp::EndTask));
	b.u8(1);
	b.uv(0);
	b.u8(0);
	b.uv(1 + ((0 << 1) | 1));
	b.uv(1 + (1 << 1));
	b.u8(0);
	for (uint64_t opcode : p_shape.extra_opcodes) {
		b.uv(opcode);
	}
	b.uv(uint64_t(vbc::VbcOp::Return));
	b.uv(1 + ((0 << 1) | 1));
	b.uv(0);
	b.uv(1);
	b.uv(1);
	b.uv(12);
	b.uv(0);

	b.u8(13);
	b.uv(kSidNativeKey);
	b.uv(kSidNativeName);
	b.uv(1);

	class_cell(kSidNode2d, false);

	b.uv(1);
	b.uv(p_shape.package_ref);

	b.uv(p_shape.accessor_role ? 2 : 1);
	b.uv(kSidTaskRole);
	b.uv(1);
	if (p_shape.accessor_role) {
		b.uv(kSidAccessorRole);
		b.uv(3);
	}

	b.uv(2);
	b.u8(0);
	b.uv(kSidScriptClass);
	b.uv(1);
	b.u8(1);
	b.uv(kSidNode2d);
	b.uv(7);

	b.u8(0xE5);
	return b.bytes;
}

bool LoadRefused(const std::vector<uint8_t> &p_bytes, const char *p_phrase) {
	Heap heap;
	Program program;
	std::string error;
	const bool loaded = load_program(heap, p_bytes.data(), p_bytes.size(), "mem/program.vbc", program, error);
	return !loaded && error.rfind("mem/program.vbc ", 0) == 0 && error.find(p_phrase) != std::string::npos;
}

std::vector<uint8_t> WithExtraOp(uint64_t p_opcode) {
	ProgramShape shape;
	shape.extra_opcodes.push_back(p_opcode);
	return BuildProgram(shape);
}

void LoaderCases(Cases &r_cases) {
	const std::vector<uint8_t> good = BuildProgram(ProgramShape());
	Heap heap;
	Program program;
	std::string error;
	const bool loaded = load_program(heap, good.data(), good.size(), "mem/program.vbc", program, error);
	r_cases.check("loader: a hand-built program loads", loaded && error.empty());
	if (!loaded) {
		printf("[verse_vm_test]   %s\n", error.c_str());
		return;
	}

	const PackageCell *package = program.packages.size() == 1 ? program.packages[0] : nullptr;
	const ProcedureCell *procedure = package != nullptr && !package->definitions.empty() && package->definitions[0].value.is_cell()
			? cell_as<ProcedureCell>(package->definitions[0].value)
			: nullptr;
	r_cases.check("format §7: the package list names the package, its first definition a procedure",
			procedure != nullptr && procedure->kind == CellKind::Procedure && package->name->text == "GodotScripts_1");
	r_cases.check("format §5.1: every optional operand is a present flag then the operand, whatever its kind",
			procedure != nullptr && procedure->ops.size() == 2 && procedure->ops[0].opcode == uint16_t(vbc::VbcOp::EndTask) && [&] {
				const uint32_t *words = procedure->operand_words.data() + procedure->ops[0].operands;
				return words[0] == 0 && words[1] == kAbsentOperand && words[2] == 1 && words[3] == 2 && words[4] == kAbsentOperand;
			}());
	r_cases.check("loader: a constant operand indexes the constant pool, whose cells and ints are values",
			procedure != nullptr && procedure->constants.size() == 1 && procedure->constants[0].same(Value::from_int32(7)) &&
					procedure->ops[1].opcode == uint16_t(vbc::VbcOp::Return) && procedure->operand_words[procedure->ops[1].operands] == 1 &&
					procedure->lines.size() == 1 && procedure->lines[0].line == 12);

	const ClassIndexEntry *script = program.find_class(ClassOrigin::Script, "script_class");
	const ClassIndexEntry *mirrored = program.find_class(ClassOrigin::Mirrored, "node2d");
	r_cases.check("format §8: the class index finds a class by origin and name, forward references resolved",
			script != nullptr && mirrored != nullptr && script->class_cell->inherited.size() == 1 &&
					script->class_cell->inherited[0] == mirrored->class_cell && script->class_cell->package == package &&
					program.entry_for(mirrored->class_cell) == mirrored && program.find_class(ClassOrigin::Mirrored, "script_class") == nullptr);
	r_cases.check("format §7: the well-known roles are found",
			program.task_class == script->class_cell && program.accessor_enumerator != nullptr &&
					program.accessor_enumerator->enumeration->enumerators[0] == program.accessor_enumerator);

	const NativeProcedureCell *native = package != nullptr && package->definitions.size() == 2 && is_cell_kind(package->definitions[1].value, CellKind::NativeProcedure)
			? cell_as<NativeProcedureCell>(package->definitions[1].value)
			: nullptr;
	NativeCall native_call(heap);
	native_call.procedure = native;
	r_cases.check("natives §3.2: a native with no implementation loads bound to the stand-in, which raises naming its key",
			native != nullptr && !native->bound && program.native_count == 1 && program.unbound_native_count == 1 &&
					native->positional_count == 1 && native->implementation(native_call) == Outcome::Error &&
					native_call.error.diagnostic == "ErrRuntime_NativeInternal" &&
					native_call.error.message == "The native function (/Verse.org/Verse/(/Verse.org/Verse:)NoSuchNative(:float):)Native is not implemented by this runtime.");

	const NativeProcedureCell *missing = program.missing_procedure != nullptr ? static_cast<const NativeProcedureCell *>(program.missing_procedure->callee) : nullptr;
	NativeCall missing_call(heap);
	missing_call.procedure = missing;
	r_cases.check("calls §10.1: the built-in package holds the missing-procedure function, which raises",
			missing != nullptr && program.builtin_package != nullptr && program.builtin_package->definitions.size() == 1 &&
					program.missing_procedure->self.same(heap.false_value()) && missing->implementation(missing_call) == Outcome::Error &&
					missing_call.error.diagnostic == "ErrRuntime_InvalidFunctionCall" &&
					missing_call.error.message == "Attempted to call an uninitialized function.");

	std::vector<uint8_t> bytes = good;
	bytes[0] = 'X';
	r_cases.check("format §9: wrong magic is refused naming the file", LoadRefused(bytes, "does not begin with VBC1"));
	ProgramShape shape;
	shape.version = 2;
	r_cases.check("format §9: another format version is refused naming both", LoadRefused(BuildProgram(shape), "is format version 2; this runtime reads version 1"));
	shape = ProgramShape();
	shape.digest = "0123";
	r_cases.check("format §9: another op-schema digest is refused", LoadRefused(BuildProgram(shape), "another Verse op set (schema 0123)"));
	shape = ProgramShape();
	shape.package_ref = 99;
	r_cases.check("format §9: a ref out of range is refused", LoadRefused(BuildProgram(shape), "cell reference 99"));
	shape = ProgramShape();
	shape.package_ref = 1;
	r_cases.check("loader: a ref to a cell of the wrong kind is refused", LoadRefused(BuildProgram(shape), "names a class cell"));
	r_cases.check("format §9: an opcode out of range is refused", LoadRefused(WithExtraOp(uint64_t(vbc::kVbcOpCount)), "opcode 114"));
	r_cases.check("format §9: an inline-cache opcode is refused", LoadRefused(WithExtraOp(uint64_t(vbc::VbcOp::LoadFieldICOffset)), "inline-cache op LoadFieldICOffset"));
	bool all_refused = true;
	for (vbc::VbcOp op : { vbc::VbcOp::Mod, vbc::VbcOp::MutableAdd, vbc::VbcOp::NewMutableArrayWithCapacity, vbc::VbcOp::NewUnionVariant,
				 vbc::VbcOp::GetUnionVariantPayload, vbc::VbcOp::GetUnionVariantTag }) {
		all_refused &= LoadRefused(WithExtraOp(uint64_t(op)), (std::string("uses the op ") + vbc::kVbcOps[uint64_t(op)].name).c_str());
	}
	r_cases.check("format §9: the six unimplemented opcodes are refused by name", all_refused);
	bytes = good;
	bytes.resize(bytes.size() / 2);
	r_cases.check("format §9: a truncated file is refused", LoadRefused(bytes, "truncated"));
	bytes = good;
	bytes.back() = 0;
	r_cases.check("format §9: a missing end marker is refused", LoadRefused(bytes, "end marker is missing"));
	bytes = good;
	bytes.push_back(0);
	r_cases.check("loader: bytes after the end marker are refused", LoadRefused(bytes, "follow the end marker"));
	shape = ProgramShape();
	shape.accessor_role = false;
	r_cases.check("format §7: a missing well-known role is refused", LoadRefused(BuildProgram(shape), "names no accessor_enumerator definition"));
}

std::map<std::string, std::string> g_files;

bool MemoryReader(const char *p_path, std::vector<uint8_t> &r_out) {
	const auto found = g_files.find(p_path);
	if (found == g_files.end()) {
		return false;
	}
	r_out.assign(found->second.begin(), found->second.end());
	return true;
}

std::string SidecarText(int p_version, int p_abi, const char *p_host_id) {
	return std::string("{\"version\": ") + std::to_string(p_version) + ", \"abi\": " + std::to_string(p_abi) + ", \"hostId\": \"" + p_host_id +
			"\", \"engineCommit\": \"203d764\", \"generation\": 1, \"packages\": [\"/x\"],"
			" \"bindings\": [{\"verse\": \"mob\", \"script\": \"Mob\"}],"
			" \"engineSignals\": {\"shapes\": [], \"keys\": {}},"
			" \"classes\": {\"script_class\": {\"abstract\": true, \"published\": true, \"exportsHarvested\": true,"
			" \"methods\": [{\"name\": \"Fire\", \"decorated\": \"(/user@localhost/script_class:)Fire(:int,:float)\","
			" \"params\": [{\"name\": \"A\", \"type\": 2, \"tag\": 2, \"default\": false, \"class\": \"\", \"classKind\": 0},"
			" {\"name\": \"B\", \"type\": 3, \"tag\": 3, \"default\": true, \"class\": \"timer\", \"classKind\": 1}],"
			" \"required\": 1, \"result\": 0, \"resultTag\": 0, \"resultClass\": \"\", \"resultClassKind\": 0, \"canFail\": true,"
			" \"suspends\": false, \"virtual\": \"_ready\", \"line\": 3, \"column\": 4}],"
			" \"signals\": [], \"rpcs\": [], \"exports\": [],"
			" \"statics\": {\"members\": [{\"name\": \"Pair\", \"isFunction\": false, \"line\": 1, \"column\": 2,"
			" \"value\": {\"type\": 8, \"tag\": 0, \"v\": [{\"type\": 2, \"tag\": 2, \"v\": \"-9223372036854775808\"}, {\"type\": 9, \"tag\": 0, \"v\": {\"type\": 5, \"tag\": 4, \"v\": \"s\"}}]}}]}},"
			" \"hidden\": {\"abstract\": false, \"published\": false, \"exportsHarvested\": true, \"methods\": [], \"signals\": [], \"rpcs\": [], \"exports\": []}}}";
}

int32_t Boot(std::string &r_error) {
	Runtime runtime;
	r_error.clear();
	return runtime.boot("mem/Cooked", r_error);
}

std::string g_diagnostic;

void CaptureDiagnostic(void *, const vh_diagnostic *p_diagnostic) {
	g_diagnostic.assign(p_diagnostic->MessageUtf8, size_t(p_diagnostic->MessageLen));
}

void BootCases(Cases &r_cases) {
	vm_set_file_reader(&MemoryReader);
	const std::vector<uint8_t> program = BuildProgram(ProgramShape());
	const std::string program_text(program.begin(), program.end());
	std::string error;

	g_files.clear();
	r_cases.check("sidecar: a missing sidecar is refused with its sentence",
			Boot(error) == VH_ERR_INIT && error == "Verse data not found at mem/Cooked/verse_classes.json. The export is incomplete; export the project again.");
	g_files["mem/verse_classes.json"] = "{\"version\": 8,}";
	r_cases.check("sidecar: invalid JSON is refused with its sentence", Boot(error) == VH_ERR_INIT && error == "mem/verse_classes.json is not valid JSON");
	g_files["mem/verse_classes.json"] = SidecarText(7, VH_ABI_VERSION, "d78f612c313953e9");
	r_cases.check("sidecar: another version is refused with its sentence",
			Boot(error) == VH_ERR_INIT && error == "mem/verse_classes.json was written by sidecar version 7; this host reads version 8. Re-export the project.");
	g_files["mem/verse_classes.json"] = SidecarText(8, 11000, "abcdef0123");
	r_cases.check("sidecar: another abi is refused with the stamp sentence",
			Boot(error) == VH_ERR_INIT && error.find("cooked by a different build of godot-verse (cooked 11000/abcdef0, host " + std::to_string(VH_ABI_VERSION) + "/verse_vm)") != std::string::npos);
	g_files["mem/verse_classes.json"] = "{\"version\": 8, \"abi\": " + std::to_string(VH_ABI_VERSION) + "}";
	r_cases.check("sidecar: a missing field is refused naming the file", Boot(error) == VH_ERR_INIT && error.rfind("mem/verse_classes.json is not a valid class sidecar: ", 0) == 0);
	g_files["mem/verse_classes.json"] = SidecarText(8, VH_ABI_VERSION, "d78f612c313953e9");
	r_cases.check("loader: a missing program.vbc is refused with the missing-data sentence",
			Boot(error) == VH_ERR_INIT && error == "Verse data not found at mem/program.vbc. The export is incomplete; export the project again.");
	g_files["mem/program.vbc"] = program_text;
	g_files["mem/verse_classes.json"] = SidecarText(8, VH_ABI_VERSION, "0000000");
	r_cases.check("loader: a program.vbc from another cook than the sidecar is refused", Boot(error) == VH_ERR_INIT && error.find("were written by different cooks") != std::string::npos);
	g_files["mem/verse_classes.json"] = SidecarText(8, VH_ABI_VERSION, "d78f612c313953e9");

	Runtime runtime;
	error.clear();
	r_cases.check("runtime: boot finds both files beside the cooked directory", runtime.boot("mem/Cooked", error) == VH_OK && error.empty());
	const vh_method_desc *methods = nullptr;
	int32_t count = -1;
	r_cases.check("sidecar: vh_class_method_list's rows, parameters in order, strings NUL-terminated",
			runtime.method_list("script_class", &methods, &count) == VH_OK && count == 1 && methods[0].ParamCount == 2 &&
					std::string(methods[0].DecoratedUtf8) == "(/user@localhost/script_class:)Fire(:int,:float)" && methods[0].RequiredParamCount == 1 &&
					methods[0].CanFail == 1 && std::string(methods[0].GodotVirtualUtf8, size_t(methods[0].GodotVirtualLen)) == "_ready" &&
					methods[0].Params[1].HasDefault == 1 && std::string(methods[0].Params[1].ClassUtf8) == "timer" && methods[0].Params[1].ClassKind == 1);
	const vh_static_desc *statics = nullptr;
	r_cases.check("sidecar: a static's value nests, an int from its decimal string",
			runtime.static_list("script_class", &statics, &count) == VH_OK && count == 1 && statics[0].Value.Type == VH_TYPE_TUPLE &&
					statics[0].Value.Seq.Count == 2 && statics[0].Value.Seq.Items[0].Int == INT64_MIN &&
					statics[0].Value.Seq.Items[1].Option != nullptr &&
					std::string(statics[0].Value.Seq.Items[1].Option->String.Utf8, size_t(statics[0].Value.Seq.Items[1].Option->String.Len)) == "s");
	const char *base = nullptr;
	r_cases.check("runtime: vh_class_base_type walks to the mirrored superclass and answers its Godot name",
			runtime.base_type("script_class", &base) == VH_OK && std::string(base) == "Node2D");
	r_cases.check("runtime: an unpublished class is listed but has no class and no base type",
			!runtime.has_class("hidden") && runtime.base_type("hidden", &base) == VH_ERR_NOT_FOUND &&
					runtime.method_list("hidden", &methods, &count) == VH_OK && count == 0);
	r_cases.check("runtime: an unknown class is not found, with its list emptied",
			runtime.method_list("nope", &methods, &count) == VH_ERR_NOT_FOUND && methods == nullptr && count == 0 && !runtime.is_abstract("nope") &&
					runtime.is_abstract("script_class"));

	vh_init_desc desc = {};
	desc.StructSize = sizeof(desc);
	desc.AbiVersion = VH_ABI_VERSION;
	desc.Godot.StructSize = int32_t(offsetof(vh_godot_api, IsValid));
	desc.OnDiagnostic = &CaptureDiagnostic;
	desc.CookedDirUtf8 = "mem/Cooked";
	r_cases.check("abi: vh_init boots, vh_has_class answers, a second vh_init is VH_ERR_STATE",
			vh_init(&desc) == VH_OK && vh_has_class("script_class") == 1 && vh_has_class("hidden") == 0 && vh_init(&desc) == VH_ERR_STATE);
	vh_shutdown();
	r_cases.check("abi: after vh_shutdown the class reads answer VH_ERR_STATE", vh_class_method_list("script_class", &methods, &count) == VH_ERR_STATE && count == 0);
	desc.AbiVersion = (VH_ABI_VERSION_MAJOR + 1) * 1000;
	r_cases.check("abi: another major is VH_ERR_ABI", vh_init(&desc) == VH_ERR_ABI);
	desc.AbiVersion = VH_ABI_VERSION;
	desc.StructSize = int32_t(offsetof(vh_init_desc, CookedDirUtf8));
	g_diagnostic.clear();
	r_cases.check("abi: a descriptor too short to carry CookedDirUtf8 is refused with a sentence, not read past",
			vh_init(&desc) == VH_ERR_INIT && g_diagnostic.find("no cooked directory") != std::string::npos);
	desc.StructSize = sizeof(desc);
	g_files.erase("mem/program.vbc");
	g_diagnostic.clear();
	r_cases.check("abi: a refusal is a status and an error diagnostic carrying the sentence",
			vh_init(&desc) == VH_ERR_INIT && g_diagnostic == "Verse data not found at mem/program.vbc. The export is incomplete; export the project again.");
	vm_set_file_reader(nullptr);
}

// An operand as the loader decodes it: one word, or a variadic list (vm_cell.h, ProcedureCell).
struct Operand {
	bool list = false;
	std::vector<uint32_t> items;
};

Operand W(uint32_t p_word) {
	return Operand{ false, { p_word } };
}

Operand L(std::initializer_list<uint32_t> p_items) {
	return Operand{ true, std::vector<uint32_t>(p_items) };
}

// A register read as a `value` operand.
uint32_t R(uint32_t p_register) {
	return p_register << 1;
}

// A procedure written op by op.
class Asm {
public:
	Heap &heap;
	ProcedureCell *procedure;

	Asm(Heap &r_heap, const char *p_name, uint32_t p_registers, uint32_t p_positional) :
			heap(r_heap), procedure(r_heap.make<ProcedureCell>()) {
		procedure->name = heap.intern(p_name);
		procedure->file = "mem/test.verse";
		procedure->register_count = p_registers;
		procedure->positional_count = p_positional;
	}

	// A constant read as a `value` operand.
	uint32_t K(Value p_value) {
		procedure->constants.push_back(p_value);
		return (uint32_t(procedure->constants.size() - 1) << 1) | 1;
	}

	// A constant named by an immediate operand.
	uint32_t C(Value p_value) {
		procedure->constants.push_back(p_value);
		return uint32_t(procedure->constants.size() - 1);
	}

	void Op(vbc::VbcOp p_op, std::initializer_list<Operand> p_operands) {
		std::vector<uint32_t> &words = procedure->operand_words;
		const size_t base = words.size();
		procedure->ops.push_back(DecodedOp{ uint16_t(p_op), uint32_t(base) });
		words.resize(base + p_operands.size());
		size_t slot = 0;
		for (const Operand &operand : p_operands) {
			if (operand.list) {
				words[base + slot] = uint32_t(words.size());
				words.push_back(uint32_t(operand.items.size()));
				words.insert(words.end(), operand.items.begin(), operand.items.end());
			} else {
				words[base + slot] = operand.items[0];
			}
			++slot;
		}
	}

	void Named(const char *p_name, uint32_t p_register) {
		procedure->named_parameters.push_back(NamedParameter{ heap.intern(p_name), p_register });
	}

	Value Function(Value p_self = Value::uninitialized()) {
		FunctionCell *function = heap.make<FunctionCell>();
		function->callee = procedure;
		function->self = p_self;
		return Value::from_cell(function);
	}
};

std::vector<int64_t> g_log;
std::vector<std::string> g_printed;

Outcome LogNative(NativeCall &r_call) {
	g_log.push_back(int_value(follow(r_call.arguments[0])).to_int64());
	r_call.result = r_call.heap.false_value();
	return Outcome::Ok;
}

void CapturePrint(void *, const char *p_utf8, int32_t p_len) {
	g_printed.emplace_back(p_utf8, size_t(p_len));
}

Value NativeFunction(Heap &r_heap, const char *p_name, uint32_t p_count, NativeFn p_implementation) {
	NativeProcedureCell *native = r_heap.make<NativeProcedureCell>();
	native->binding_key = r_heap.intern(p_name);
	native->decorated_name = r_heap.intern(p_name);
	native->positional_count = p_count;
	native->implementation = p_implementation;
	native->bound = true;
	FunctionCell *function = r_heap.make<FunctionCell>();
	function->callee = native;
	function->self = r_heap.false_value();
	return Value::from_cell(function);
}

std::string Invoked(Interpreter &r_interpreter, Value p_function, std::vector<Value> p_arguments, std::vector<NamedArgument> p_named = {}) {
	r_interpreter.begin_entry();
	Value result;
	const Outcome outcome = r_interpreter.invoke(p_function, Value::uninitialized(), p_arguments, p_named, result);
	r_interpreter.end_entry(outcome == Outcome::Ok);
	switch (outcome) {
		case Outcome::Ok:
			return Show(result);
		case Outcome::Fail:
			return "<fail>";
		case Outcome::Yield:
			return "<suspended>";
		case Outcome::Unsupported:
			return "<not yet>";
		default:
			return "<error> " + r_interpreter.error().message_line();
	}
}

void CallCases(Cases &r_cases) {
	Heap heap;
	Program program;
	Interpreter interpreter(heap, program);
	using vbc::VbcOp;

	// Two(A, B) = A * 10 + B.
	Asm two(heap, "Two", 6, 2);
	two.Op(VbcOp::Mul, { W(4), W(R(2)), W(two.K(Int(heap, 10))) });
	two.Op(VbcOp::Add, { W(5), W(R(4)), W(R(3)) });
	two.Op(VbcOp::Return, { W(R(5)) });
	const Value two_fn = two.Function(heap.false_value());
	r_cases.check("calls §3: A = P passes each argument", Invoked(interpreter, two_fn, { Int(heap, 3), Int(heap, 5) }) == "35");
	r_cases.check("calls §3: A = 1, P = 2 unpacks a tuple", Invoked(interpreter, two_fn, { Arr(heap, { Int(heap, 7), Int(heap, 8) }) }) == "78");
	r_cases.check("calls §3: a tuple of the wrong length is an invariant violation",
			Invoked(interpreter, two_fn, { Arr(heap, { Int(heap, 7) }) }).find("VM invariant violated") != std::string::npos);
	r_cases.check("calls §3: A = 3, P = 2 is an invariant violation",
			Invoked(interpreter, two_fn, { Int(heap, 1), Int(heap, 2), Int(heap, 3) }).find("VM invariant violated") != std::string::npos);

	// One(T) = T(0) * 10 + T(1), indexing its one tuple parameter.
	Asm one(heap, "One", 7, 1);
	one.Op(VbcOp::Call, { W(3), W(R(2)), L({ one.K(Int(heap, 0)) }), L({}), L({}), W(0) });
	one.Op(VbcOp::Call, { W(4), W(R(2)), L({ one.K(Int(heap, 1)) }), L({}), L({}), W(0) });
	one.Op(VbcOp::Mul, { W(5), W(R(3)), W(one.K(Int(heap, 10))) });
	one.Op(VbcOp::Add, { W(6), W(R(5)), W(R(4)) });
	one.Op(VbcOp::Return, { W(R(6)) });
	const Value one_fn = one.Function(heap.false_value());
	r_cases.check("calls §3: P = 1, A = 2 boxes the arguments into one tuple", Invoked(interpreter, one_fn, { Int(heap, 3), Int(heap, 5) }) == "35");
	r_cases.check("calls §4.1: indexing a tuple past its end fails the call", Invoked(interpreter, one_fn, { Arr(heap, { Int(heap, 3) }) }) == "<fail>");

	// Length(T) of a tuple parameter given no arguments at all.
	Asm empty(heap, "Empty", 4, 1);
	empty.Op(VbcOp::Length, { W(3), W(R(2)) });
	empty.Op(VbcOp::Return, { W(R(3)) });
	r_cases.check("calls §3: P = 1, A = 0 passes the empty tuple", Invoked(interpreter, empty.Function(heap.false_value()), {}) == "0");

	// Named(?X, ?Y = 7) = X * 10 + Y: the default is the callee's own code, after JumpIfInitialized.
	Asm named(heap, "Named", 6, 0);
	named.Named("X", 2);
	named.Named("Y", 3);
	named.Op(VbcOp::JumpIfInitialized, { W(R(3)), W(3) });
	named.Op(VbcOp::Reset, { W(3), W(0) });
	named.Op(VbcOp::Move, { W(3), W(named.K(Int(heap, 7))) });
	named.Op(VbcOp::Mul, { W(4), W(R(2)), W(named.K(Int(heap, 10))) });
	named.Op(VbcOp::Add, { W(5), W(R(4)), W(R(3)) });
	named.Op(VbcOp::Return, { W(R(5)) });
	const Value named_fn = named.Function(heap.false_value());
	r_cases.check("calls §5.2: named arguments match by name whatever their order",
			Invoked(interpreter, named_fn, {}, { NamedArgument{ heap.intern("Y"), Int(heap, 5) }, NamedArgument{ heap.intern("X"), Int(heap, 6) } }) == "65");
	r_cases.check("calls §5.3-5.4: an unsupplied named parameter is uninitialized and takes its default",
			Invoked(interpreter, named_fn, {}, { NamedArgument{ heap.intern("X"), Int(heap, 6) } }) == "67");
	r_cases.check("calls §5.2: a named argument matching no parameter is ignored",
			Invoked(interpreter, named_fn, {}, { NamedArgument{ heap.intern("X"), Int(heap, 1) }, NamedArgument{ heap.intern("Z"), Int(heap, 9) } }) == "17");

	// Outer() calls Two(4, 2) through a Call op into a destination that already holds 42, then 43.
	Asm outer(heap, "Outer", 4, 0);
	const uint32_t two_k = outer.K(two_fn);
	outer.Op(VbcOp::Move, { W(2), W(outer.K(Int(heap, 42))) });
	outer.Op(VbcOp::Call, { W(2), W(two_k), L({ outer.K(Int(heap, 4)), outer.K(Int(heap, 2)) }), L({}), L({}), W(0) });
	outer.Op(VbcOp::Move, { W(3), W(outer.K(Int(heap, 43))) });
	outer.Op(VbcOp::Call, { W(3), W(two_k), L({ outer.K(Int(heap, 4)), outer.K(Int(heap, 2)) }), L({}), L({}), W(0) });
	outer.Op(VbcOp::Return, { W(R(3)) });
	r_cases.check("calls §5.6: a result is unified into Dest, and a mismatch fails in the caller",
			Invoked(interpreter, outer.Function(heap.false_value()), {}) == "<fail>");

	// Sum(N) = if (N <= 0) then 0 else N + Sum(N - 1), 100000 deep.
	Asm sum(heap, "Sum", 8, 1);
	const Value sum_fn = sum.Function(heap.false_value());
	sum.Op(VbcOp::GtFastFail, { W(3), W(4), W(R(2)), W(sum.K(Int(heap, 0))), W(2) });
	sum.Op(VbcOp::Jump, { W(3) });
	sum.Op(VbcOp::Return, { W(sum.K(Int(heap, 0))) });
	sum.Op(VbcOp::Sub, { W(5), W(R(2)), W(sum.K(Int(heap, 1))) });
	sum.Op(VbcOp::Call, { W(6), W(sum.K(sum_fn)), L({ R(5) }), L({}), L({}), W(0) });
	sum.Op(VbcOp::Add, { W(7), W(R(2)), W(R(6)) });
	sum.Op(VbcOp::Return, { W(R(7)) });
	r_cases.check("calls §9: 100000 nested Verse calls run on heap frames, not the native stack",
			Invoked(interpreter, sum_fn, { Int(heap, 100000) }) == "5000050000");

	// A closure reading a capture two scopes out, and CallWithSelf's receiver in register 0.
	Asm inner(heap, "Inner", 5, 0);
	inner.Op(VbcOp::LoadParentScope, { W(2), W(R(1)) });
	inner.Op(VbcOp::LoadCapture, { W(3), W(R(2)), W(1) });
	inner.Op(VbcOp::Add, { W(4), W(R(3)), W(R(0)) });
	inner.Op(VbcOp::Return, { W(R(4)) });
	Asm maker(heap, "Maker", 6, 0);
	maker.Op(VbcOp::NewScope, { W(2), W(maker.K(heap.false_value())), L({ maker.K(Int(heap, 1)), maker.K(Int(heap, 30)) }) });
	maker.Op(VbcOp::NewScope, { W(3), W(R(2)), L({}) });
	maker.Op(VbcOp::NewFunction, { W(4), W(maker.K(Value::from_cell(inner.procedure))), W(kAbsentOperand), W(R(3)) });
	maker.Op(VbcOp::CallWithSelf, { W(5), W(R(4)), W(maker.K(Int(heap, 12))), L({}), L({}), L({}), W(0) });
	maker.Op(VbcOp::Return, { W(R(5)) });
	r_cases.check("calls §7: a closure reaches its capture through LoadParentScope and LoadCapture; CallWithSelf binds Self",
			Invoked(interpreter, maker.Function(heap.false_value()), {}) == "42");
	Asm bound(heap, "Bound", 4, 0);
	bound.Op(VbcOp::NewFunction, { W(2), W(bound.K(Value::from_cell(inner.procedure))), W(bound.K(Int(heap, 1))), W(bound.K(heap.false_value())) });
	bound.Op(VbcOp::CallWithSelf, { W(3), W(R(2)), W(bound.K(Int(heap, 2))), L({}), L({}), L({}), W(0) });
	bound.Op(VbcOp::Return, { W(R(3)) });
	r_cases.check("calls §4.2: CallWithSelf on a function that has a receiver is an invariant violation",
			Invoked(interpreter, bound.Function(heap.false_value()), {}).find("already has a receiver") != std::string::npos);
}

void FailureAndEffectCases(Cases &r_cases) {
	Heap heap;
	Program program;
	Interpreter interpreter(heap, program);
	interpreter.godot.Print = &CapturePrint;
	using vbc::VbcOp;
	const Value print = NativeFunction(heap, "Print", 1, native_implementation("(/Godot.org/Godot/(/Godot.org/Godot:)Print(:[]char):)Native"));
	const Value err = NativeFunction(heap, "Err", 1, native_implementation("(/Verse.org/Verse/(/Verse.org/Verse:)Err(:[]char):)Native"));

	// Print "a"; a full context printing "lost" then failing; Print "b"; then the tail.
	const auto body = [&](bool p_fail_at_end, bool p_raise_at_end) {
		Asm code(heap, "Effects", 6, 0);
		const uint32_t print_k = code.K(print);
		code.Op(VbcOp::Call, { W(2), W(print_k), L({ code.K(Str(heap, "a")) }), L({}), L({}), W(0) });
		code.Op(VbcOp::BeginFailureContext, { W(5), W(0) });
		code.Op(VbcOp::Call, { W(3), W(print_k), L({ code.K(Str(heap, "lost")) }), L({}), L({}), W(0) });
		code.Op(VbcOp::Query, { W(4), W(code.K(heap.false_value())) });
		code.Op(VbcOp::EndFailureContext, { W(5), W(0) });
		code.Op(VbcOp::Call, { W(2), W(print_k), L({ code.K(Str(heap, "b")) }), L({}), L({}), W(0) });
		if (p_raise_at_end) {
			code.Op(VbcOp::Call, { W(3), W(code.K(err)), L({ code.K(Str(heap, "boom")) }), L({}), L({}), W(0) });
		}
		if (p_fail_at_end) {
			code.Op(VbcOp::Query, { W(4), W(code.K(heap.false_value())) });
		}
		code.Op(VbcOp::Return, { W(code.K(heap.false_value())) });
		return code.Function(heap.false_value());
	};
	g_printed.clear();
	Invoked(interpreter, body(false, false), {});
	r_cases.check("failure §7, §11: deferred Prints run in order at the root commit; a failed context's are dropped",
			g_printed == std::vector<std::string>{ "a", "b" });
	g_printed.clear();
	r_cases.check("failure §4: failing the root context declines the entry and drops every deferred Print",
			Invoked(interpreter, body(true, false), {}) == "<fail>" && g_printed.empty());
	g_printed.clear();
	const std::string raised = Invoked(interpreter, body(false, true), {});
	r_cases.check("failure §9.2: a raise drops every deferred Print and renders its message line",
			g_printed.empty() && raised == "<error> ErrorRequested: A runtime error was explicitly raised from user code. (User Message: 'boom')");
	r_cases.check("failure §9.2: a native's raise puts the native first in the frames, then the Verse frame",
			interpreter.error().frames.size() == 2 && interpreter.error().frames[0].path == "[native]" && interpreter.error().frames[1].function == "Effects");

	Asm fast(heap, "Fast", 5, 1);
	fast.Op(VbcOp::LtFastFail, { W(3), W(4), W(R(2)), W(fast.K(Int(heap, 10))), W(3) });
	fast.Op(VbcOp::EndFastFailureContext, { W(4), W(R(4)), W(3) });
	fast.Op(VbcOp::Return, { W(fast.K(Str(heap, "small"))) });
	fast.Op(VbcOp::Return, { W(fast.K(Str(heap, "big"))) });
	const Value fast_fn = fast.Function(heap.false_value());
	r_cases.check("failure §5: a fast-fail op falls through on success and jumps to OnFailure on failure, leaving the indicator fresh",
			Invoked(interpreter, fast_fn, { Int(heap, 3) }) == "\"small\"" && Invoked(interpreter, fast_fn, { Int(heap, 30) }) == "\"big\"");

	Asm parks(heap, "Parks", 5, 0);
	parks.Op(VbcOp::Add, { W(3), W(R(2)), W(parks.K(Int(heap, 1))) });
	parks.Op(VbcOp::Return, { W(R(3)) });
	const uint64_t before = interpreter.park_count;
	r_cases.check("unification §11.3: reading a register nothing wrote is the stage-1 park error, counted",
			Invoked(interpreter, parks.Function(heap.false_value()), {}).find("Stage-1 interpreter cannot wait: Add in Parks") != std::string::npos &&
					interpreter.park_count == before + 1);

	Asm unify(heap, "Unify", 4, 0);
	unify.Op(VbcOp::Move, { W(2), W(unify.K(Arr(heap, { Int(heap, 1), Int(heap, 2) }))) });
	unify.Op(VbcOp::Move, { W(2), W(unify.K(Arr(heap, { Int(heap, 1), Int(heap, 2) }))) });
	unify.Op(VbcOp::Move, { W(2), W(unify.K(Arr(heap, { Int(heap, 1), Int(heap, 3) }))) });
	unify.Op(VbcOp::Return, { W(R(2)) });
	r_cases.check("unification §3.3: a second Move into a filled register compares, and not equal fails",
			Invoked(interpreter, unify.Function(heap.false_value()), {}) == "<fail>");
	unify.procedure->ops.erase(unify.procedure->ops.begin() + 2);
	r_cases.check("unification §3.3: an equal second Move succeeds and leaves the register", Invoked(interpreter, unify.Function(heap.false_value()), {}) == "[1,2]");

	Asm yields(heap, "Yields", 3, 0);
	yields.Op(VbcOp::NewClass, {});
	r_cases.check("ops §0: an op a later task implements answers the not-yet outcome",
			Invoked(interpreter, yields.Function(heap.false_value()), {}).find("<not yet>") == 0);
}

void ConstructionCases(Cases &r_cases) {
	Heap heap;
	Program program;
	Interpreter interpreter(heap, program);
	using vbc::VbcOp;
	const Value log = NativeFunction(heap, "Log", 1, &LogNative);
	const NameCell *x = heap.intern("(/test/base:)X");
	const NameCell *describe = heap.intern("(/test/base:)Describe");
	SimpleTypeCell *any = heap.make<SimpleTypeCell>();
	const Value marker = Int(heap, 12774014);

	ClassCell *base = heap.make<ClassCell>();
	ClassCell *derived = heap.make<ClassCell>();
	ArchetypeCell *base_body = heap.make<ArchetypeCell>();
	ArchetypeCell *derived_body = heap.make<ArchetypeCell>();
	base->archetype = base_body;
	derived->archetype = derived_body;
	derived->inherited.push_back(base);
	base_body->owner = base;
	derived_body->owner = derived;
	derived_body->next = base_body;

	Asm base_describe(heap, "BaseDescribe", 2, 0);
	base_describe.Op(VbcOp::Return, { W(base_describe.K(Str(heap, "base"))) });
	Asm derived_describe(heap, "DerivedDescribe", 2, 0);
	derived_describe.Op(VbcOp::Return, { W(derived_describe.K(Str(heap, "derived"))) });
	base_body->entries.push_back(ArchetypeEntry{ x, nullptr, Value::from_cell(any), Value::uninitialized(), 0 });
	base_body->entries.push_back(ArchetypeEntry{ describe, nullptr, Value::uninitialized(), base_describe.Function(), 0 });
	derived_body->entries.push_back(ArchetypeEntry{ x, nullptr, Value::from_cell(any), Value::uninitialized(), 0 });
	derived_body->entries.push_back(ArchetypeEntry{ describe, nullptr, Value::uninitialized(), derived_describe.Function(), 0 });

	// Each class body: CreateField X; log the class's number; UnifyField X; then the superclass step.
	const auto constructor = [&](const char *p_name, int64_t p_value, Value p_super) {
		Asm code(heap, p_name, 9, 3);
		const uint32_t name = code.C(Value::from_cell(x));
		code.Op(VbcOp::CreateField, { W(5), W(R(2)), W(R(0)), W(name), W(4) });
		code.Op(VbcOp::Call, { W(6), W(code.K(log)), L({ code.K(Int(heap, p_value)) }), L({}), L({}), W(0) });
		code.Op(VbcOp::UnifyField, { W(R(0)), W(name), W(code.K(Int(heap, p_value))) });
		code.Op(VbcOp::Jump, { W(4) });
		if (p_super.is_uninitialized()) {
			code.Op(VbcOp::Return, { W(R(2)) });
		} else {
			code.Op(VbcOp::CallWithSelf, { W(7), W(code.K(p_super)), W(R(0)), L({ R(2), R(3), code.K(Value::uninitialized()) }), L({}), L({}), W(0) });
			code.Op(VbcOp::Return, { W(R(7)) });
		}
		return code.Function();
	};
	const auto blocks = [&](const char *p_name, int64_t p_value, Value p_super) {
		Asm code(heap, p_name, 5, 0);
		if (!p_super.is_uninitialized()) {
			code.Op(VbcOp::CallWithSelf, { W(2), W(code.K(p_super)), W(R(0)), L({}), L({}), L({}), W(0) });
		}
		code.Op(VbcOp::Call, { W(3), W(code.K(log)), L({ code.K(Int(heap, p_value)) }), L({}), L({}), W(0) });
		code.Op(VbcOp::Return, { W(code.K(heap.false_value())) });
		return code.Function();
	};
	const Value base_constructor = constructor("BaseConstructor", 1, Value::uninitialized());
	const Value base_blocks = blocks("BaseBlocks", 10, Value::uninitialized());
	base->constructor = cell_as<FunctionCell>(base_constructor);
	base->blocks = cell_as<FunctionCell>(base_blocks);
	derived->constructor = cell_as<FunctionCell>(constructor("DerivedConstructor", 2, base_constructor));
	derived->blocks = cell_as<FunctionCell>(blocks("DerivedBlocks", 20, base_blocks));

	const ClassLayout &layout = interpreter.layouts.get(derived);
	const LayoutField *x_field = layout.find(x);
	const LayoutField *describe_field = layout.find(describe);
	r_cases.check("objects §4.2-4.3: the subclass's entry is seen first; a data member is a slot, a method a constant",
			x_field != nullptr && x_field->kind == FieldKind::Slot && describe_field != nullptr && describe_field->kind == FieldKind::Constant &&
					cell_as<FunctionCell>(describe_field->value)->callee == derived_describe.procedure && layout.slot_names.size() == 1);

	g_log.clear();
	interpreter.begin_entry();
	Value object;
	const Outcome built = interpreter.construct(derived, 0, object);
	interpreter.end_entry(built == Outcome::Ok);
	const ObjectCell *made = built == Outcome::Ok ? cell_as<ObjectCell>(object) : nullptr;
	r_cases.check("objects §7.2, §7.4, §7.9: subclass initializer first, the base's skipped by CreateField, then blocks base-first",
			made != nullptr && g_log == std::vector<int64_t>{ 2, 10, 20 } && Show(made->field_values[x_field->slot]) == "2");
	Value method;
	r_cases.check("calls §6: a method resolves to the override, bound to the object",
			made != nullptr && interpreter.resolve_method(object, describe, method) && cell_as<FunctionCell>(method)->self.same(object) &&
					Invoked(interpreter, method, {}) == "\"derived\"");

	// derived{X := 7}: the archetype's initializer runs before any class body, so both jump.
	ArchetypeCell *expression = heap.make<ArchetypeCell>();
	expression->entries.push_back(ArchetypeEntry{ x, nullptr, Value::uninitialized(), Value::uninitialized(), 0 });
	Asm make(heap, "Make", 8, 0);
	const uint32_t name = make.C(Value::from_cell(x));
	make.Op(VbcOp::NewObject, { W(2), W(make.K(Value::from_cell(expression))), W(make.K(Value::from_cell(derived))) });
	make.Op(VbcOp::CreateField, { W(3), W(make.K(marker)), W(R(2)), W(name), W(3) });
	make.Op(VbcOp::UnifyField, { W(R(2)), W(name), W(make.K(Int(heap, 7))) });
	make.Op(VbcOp::CallWithSelf, { W(4), W(make.K(Value::from_cell(derived->constructor))), W(R(2)),
			L({ make.K(marker), make.K(Value::uninitialized()), make.K(Value::uninitialized()) }), L({}), L({}), W(0) });
	make.Op(VbcOp::UnifyNativeObject, { W(R(4)), W(R(2)) });
	make.Op(VbcOp::UnwrapNativeConstructorWrapper, { W(5), W(R(2)) });
	make.Op(VbcOp::LoadField, { W(6), W(R(5)), W(name) });
	make.Op(VbcOp::Return, { W(R(6)) });
	g_log.clear();
	r_cases.check("objects §4.2, §7.1: an archetype-supplied field wins, and UnifyNativeObject runs the blocks once",
			Invoked(interpreter, make.Function(heap.false_value()), {}) == "7" && g_log == std::vector<int64_t>{ 10, 20 });
}

RefCell *g_watched = nullptr;
Value g_reentered;

// Registers a compensation that logs its number times ten plus what g_watched holds when it runs.
Outcome CompensateNative(NativeCall &r_call) {
	const int64_t number = int_value(follow(r_call.arguments[0])).to_int64();
	r_call.interpreter->compensate([number] { g_log.push_back(number * 10 + int_value(follow(g_watched->content)).to_int64()); });
	r_call.result = r_call.heap.false_value();
	return Outcome::Ok;
}

// A native that calls back into Verse, as a Godot callback would: a nested entry whose outcome the
// native ignores, answering the host's status rather than the Verse one.
Outcome ReenterNative(NativeCall &r_call) {
	Interpreter &interpreter = *r_call.interpreter;
	interpreter.begin_entry();
	Value result;
	const Outcome outcome = interpreter.invoke(g_reentered, Value::uninitialized(), {}, {}, result);
	interpreter.end_entry(outcome == Outcome::Ok);
	r_call.result = r_call.heap.false_value();
	return Outcome::Ok;
}

std::vector<std::pair<int64_t, int>> g_released;

void CaptureRelease(void *, vh_handle p_handle, vh_bool p_discard) {
	g_released.emplace_back(int64_t(p_handle), int(p_discard));
}

// Stands in for VhAdoptOrMint's minting branch: handle 42, minted for the argument.
Outcome MintNative(NativeCall &r_call) {
	r_call.interpreter->record_mint(42, follow(r_call.arguments[0]).as_cell());
	r_call.result = Value::from_int32(42);
	return Outcome::Ok;
}

void PatchLabel(Asm &r_code, size_t p_op, uint32_t p_label) {
	r_code.procedure->operand_words[r_code.procedure->ops[p_op].operands] = p_label;
}

// Opens a full context, runs p_body in it, fails it or not, and returns false after it.
Value InContext(Heap &r_heap, const char *p_name, bool p_fail, const std::function<void(Asm &)> &p_body) {
	using vbc::VbcOp;
	Asm code(r_heap, p_name, 8, 0);
	code.Op(VbcOp::BeginFailureContext, { W(0), W(0) });
	p_body(code);
	if (p_fail) {
		code.Op(VbcOp::Query, { W(7), W(code.K(r_heap.false_value())) });
	}
	code.Op(VbcOp::EndFailureContext, { W(0), W(0) });
	PatchLabel(code, 0, uint32_t(code.procedure->ops.size()));
	code.Op(VbcOp::Return, { W(code.K(r_heap.false_value())) });
	return code.Function(r_heap.false_value());
}

void UndoLogCases(Cases &r_cases) {
	Heap heap;
	Program program;
	Interpreter interpreter(heap, program);
	using vbc::VbcOp;
	const Value compensate = NativeFunction(heap, "Compensate", 1, &CompensateNative);
	const Value reenter = NativeFunction(heap, "Reenter", 0, &ReenterNative);
	const Value err = NativeFunction(heap, "Err", 1, native_implementation("(/Verse.org/Verse/(/Verse.org/Verse:)Err(:[]char):)Native"));
	const auto content = [](const RefCell *p_var) { return Show(p_var->content); };
	const auto var_of = [&](int64_t p_value) { return heap.make<RefCell>(Int(heap, p_value)); };

	RefCell *var = var_of(0);
	const auto set_var = [&](Asm &r_code) { r_code.Op(VbcOp::RefSet, { W(r_code.K(Value::from_cell(var))), W(r_code.K(Int(heap, 1))) }); };
	Invoked(interpreter, InContext(heap, "RefSetFails", true, set_var), {});
	const bool undone = content(var) == "0";
	Invoked(interpreter, InContext(heap, "RefSetSucceeds", false, set_var), {});
	r_cases.check("failure §6.1: RefSet in a failed context is undone, in a committed one kept", undone && content(var) == "1");

	ClassCell *point = heap.make<ClassCell>();
	ArchetypeCell *point_body = heap.make<ArchetypeCell>();
	point->archetype = point_body;
	point_body->owner = point;
	SimpleTypeCell *any = heap.make<SimpleTypeCell>();
	const NameCell *x = heap.intern("(/test/point:)X");
	const NameCell *v = heap.intern("(/test/point:)V");
	point_body->entries.push_back(ArchetypeEntry{ x, nullptr, Value::from_cell(any), Value::uninitialized(), 0 });
	point_body->entries.push_back(ArchetypeEntry{ v, nullptr, Value::from_cell(any), Value::uninitialized(), 0 });
	const ClassLayout &point_layout = interpreter.layouts.get(point);
	ObjectCell *object = interpreter.layouts.new_object(heap, point_layout);
	RefCell *field_var = var_of(0);
	object->field_values[point_layout.find(x)->slot] = Int(heap, 0);
	object->field_values[point_layout.find(v)->slot] = Value::from_cell(field_var);
	Invoked(interpreter, InContext(heap, "SetFields", true, [&](Asm &r_code) {
		r_code.Op(VbcOp::SetField, { W(r_code.K(Value::from_cell(object))), W(r_code.C(Value::from_cell(x))), W(r_code.K(Int(heap, 5))) });
		r_code.Op(VbcOp::SetField, { W(r_code.K(Value::from_cell(object))), W(r_code.C(Value::from_cell(v))), W(r_code.K(Int(heap, 6))) });
	}),
			{});
	r_cases.check("failure §6.1: SetField of a plain field and of a var field are undone",
			Show(object->field_values[point_layout.find(x)->slot]) == "0" && content(field_var) == "0" &&
					object->field_values[point_layout.find(v)->slot].same(Value::from_cell(field_var)));

	const auto numbers = [&] { return make_array(heap, { Int(heap, 1), Int(heap, 2), Int(heap, 3) }, true); };
	const Value elements = numbers();
	Invoked(interpreter, InContext(heap, "CallSetArray", true, [&](Asm &r_code) {
		r_code.Op(VbcOp::CallSet, { W(r_code.K(elements)), W(r_code.K(Int(heap, 0))), W(r_code.K(Int(heap, 9))) });
		r_code.Op(VbcOp::CallSet, { W(r_code.K(elements)), W(r_code.K(Int(heap, 0))), W(r_code.K(Int(heap, 8))) });
	}),
			{});
	r_cases.check("failure §6.1: CallSet of a mutable array element written twice restores the first value", Show(elements) == "[1,2,3]");

	const Value appended = numbers();
	const Value collected = numbers();
	const auto array_add = [&](Value p_array, uint32_t p_transactional) {
		return [&, p_array, p_transactional](Asm &r_code) {
			r_code.Op(VbcOp::ArrayAdd, { W(2), W(r_code.K(p_array)), W(r_code.K(Int(heap, 4))), W(p_transactional) });
		};
	};
	Invoked(interpreter, InContext(heap, "ArrayAddTransactional", true, array_add(appended, 1)), {});
	Invoked(interpreter, InContext(heap, "ArrayAddCollecting", true, array_add(collected, 0)), {});
	r_cases.check("failure §6.1: ArrayAdd is undone when bTransactional, and not recorded otherwise",
			Show(appended) == "[1,2,3]" && Show(collected) == "[1,2,3,4]");

	const Value fast = numbers();
	Invoked(interpreter, InContext(heap, "FastAppend", true, [&](Asm &r_code) {
		r_code.Op(VbcOp::FastAppendToArray, { W(r_code.K(fast)), W(r_code.K(Arr(heap, { Int(heap, 4), Int(heap, 5) }))) });
	}),
			{});
	r_cases.check("failure §6.1: FastAppendToArray is undone to the old length", Show(fast) == "[1,2,3]");

	const Value frozen = numbers();
	Invoked(interpreter, InContext(heap, "MakeImmutable", true, [&](Asm &r_code) {
		r_code.Op(VbcOp::InPlaceMakeImmutable, { W(2), W(r_code.K(frozen)) });
	}),
			{});
	r_cases.check("failure §6.1: InPlaceMakeImmutable is undone to a mutable array", is_cell_kind(frozen, CellKind::MutableArray));

	Value letters = Value::uninitialized();
	melt(heap, MakeMap(heap, { Int(heap, 1), Int(heap, 2) }, { Str(heap, "a"), Str(heap, "b") }), letters);
	Invoked(interpreter, InContext(heap, "CallSetMap", true, [&](Asm &r_code) {
		r_code.Op(VbcOp::CallSet, { W(r_code.K(letters)), W(r_code.K(Int(heap, 1))), W(r_code.K(Str(heap, "A"))) });
		r_code.Op(VbcOp::CallSet, { W(r_code.K(letters)), W(r_code.K(Int(heap, 3))), W(r_code.K(Str(heap, "c"))) });
		r_code.Op(VbcOp::CallSet, { W(r_code.K(letters)), W(r_code.K(Int(heap, 0))), W(r_code.K(Str(heap, "z"))) });
		r_code.Op(VbcOp::CallSet, { W(r_code.K(letters)), W(r_code.K(Int(heap, 1))), W(r_code.K(Str(heap, "Z"))) });
	}),
			{});
	Value missing;
	r_cases.check("failure §6.1: map inserts and replaces are undone, the count, the order and the index with them",
			Show(letters) == "{1=>\"a\",2=>\"b\"}" && Fails(map_lookup(letters, Int(heap, 3), missing)) && Fails(map_lookup(letters, Int(heap, 0), missing)));

	// 0 Move r2 <- 5, then a failed context that resets r2 and trails 7 into it, then returns r2.
	Asm reset(heap, "Reset", 8, 0);
	reset.Op(VbcOp::Move, { W(2), W(reset.K(Int(heap, 5))) });
	reset.Op(VbcOp::BeginFailureContext, { W(6), W(0) });
	reset.Op(VbcOp::Reset, { W(2), W(0) });
	reset.Op(VbcOp::MoveTrailed, { W(2), W(reset.K(Int(heap, 7))) });
	reset.Op(VbcOp::Query, { W(3), W(reset.K(heap.false_value())) });
	reset.Op(VbcOp::EndFailureContext, { W(6), W(0) });
	reset.Op(VbcOp::Return, { W(R(2)) });
	r_cases.check("failure §6.2: Reset and MoveTrailed in a failed context are undone", Invoked(interpreter, reset.Function(heap.false_value()), {}) == "5");

	// A failed context that writes r2, then Move r2 <- 8 afterwards: 8 only if r2 is fresh again.
	const auto rewrite = [&](const char *p_name, const std::function<void(Asm &)> &p_write) {
		Asm code(heap, p_name, 8, 0);
		code.Op(VbcOp::Move, { W(2), W(code.K(Int(heap, 5))) });
		code.Op(VbcOp::BeginFailureContext, { W(0), W(0) });
		p_write(code);
		code.Op(VbcOp::Query, { W(3), W(code.K(heap.false_value())) });
		code.Op(VbcOp::EndFailureContext, { W(0), W(0) });
		PatchLabel(code, 1, uint32_t(code.procedure->ops.size()));
		code.Op(VbcOp::Move, { W(2), W(code.K(Int(heap, 8))) });
		code.Op(VbcOp::Return, { W(R(2)) });
		return Invoked(interpreter, code.Function(heap.false_value()), {});
	};
	r_cases.check("unification §4: ResetNonTrailed is not recorded, so the register stays fresh",
			rewrite("ResetNonTrailed", [&](Asm &r_code) { r_code.Op(VbcOp::ResetNonTrailed, { W(2), W(0) }); }) == "8");

	Asm seven(heap, "Seven", 2, 0);
	seven.Op(VbcOp::ReturnTrailed, { W(seven.K(Int(heap, 7))) });
	Asm plain_seven(heap, "PlainSeven", 2, 0);
	plain_seven.Op(VbcOp::Return, { W(plain_seven.K(Int(heap, 7))) });
	const auto call_into_fresh = [&](const char *p_name, Value p_callee) {
		Asm code(heap, p_name, 8, 0);
		code.Op(VbcOp::BeginFailureContext, { W(0), W(0) });
		code.Op(VbcOp::Call, { W(2), W(code.K(p_callee)), L({}), L({}), L({}), W(0) });
		code.Op(VbcOp::Query, { W(3), W(code.K(heap.false_value())) });
		code.Op(VbcOp::EndFailureContext, { W(0), W(0) });
		PatchLabel(code, 0, uint32_t(code.procedure->ops.size()));
		code.Op(VbcOp::Move, { W(2), W(code.K(Int(heap, 9))) });
		code.Op(VbcOp::Return, { W(R(2)) });
		return Invoked(interpreter, code.Function(heap.false_value()), {});
	};
	r_cases.check("failure §6.2: ReturnTrailed's store into the caller's register is undone; Return's is not",
			call_into_fresh("CallsTrailed", seven.Function(heap.false_value())) == "9" && call_into_fresh("CallsPlain", plain_seven.Function(heap.false_value())) == "<fail>");

	// r4 reads r2 and r5 reads r3, making both placeholders; Move r2 <- r3 links them in the context.
	Asm link(heap, "Link", 8, 0);
	link.Op(VbcOp::Move, { W(4), W(R(2)) });
	link.Op(VbcOp::Move, { W(5), W(R(3)) });
	link.Op(VbcOp::BeginFailureContext, { W(6), W(0) });
	link.Op(VbcOp::Move, { W(2), W(R(3)) });
	link.Op(VbcOp::Query, { W(6), W(link.K(heap.false_value())) });
	link.Op(VbcOp::EndFailureContext, { W(0), W(0) });
	link.Op(VbcOp::Move, { W(3), W(link.K(Int(heap, 1))) });
	link.Op(VbcOp::Move, { W(2), W(link.K(Int(heap, 2))) });
	link.Op(VbcOp::Return, { W(R(2)) });
	PatchLabel(link, 2, 6);
	r_cases.check("unification §2.4: a placeholder link made in a failed context is undone", Invoked(interpreter, link.Function(heap.false_value()), {}) == "2");

	// F writes Y = 1; G, inside it, writes Y = 2 and commits; F fails.
	RefCell *y = var_of(0);
	Asm nested(heap, "Nested", 8, 0);
	nested.Op(VbcOp::BeginFailureContext, { W(7), W(0) });
	nested.Op(VbcOp::RefSet, { W(nested.K(Value::from_cell(y))), W(nested.K(Int(heap, 1))) });
	nested.Op(VbcOp::BeginFailureContext, { W(5), W(1) });
	nested.Op(VbcOp::RefSet, { W(nested.K(Value::from_cell(y))), W(nested.K(Int(heap, 2))) });
	nested.Op(VbcOp::EndFailureContext, { W(5), W(1) });
	nested.Op(VbcOp::Query, { W(3), W(nested.K(heap.false_value())) });
	nested.Op(VbcOp::EndFailureContext, { W(7), W(0) });
	nested.Op(VbcOp::Return, { W(nested.K(heap.false_value())) });
	Invoked(interpreter, nested.Function(heap.false_value()), {});
	const bool merged = content(y) == "0";
	// F writes Y = 1; G writes Y = 2 and fails; F commits.
	Asm inner_fails(heap, "InnerFails", 8, 0);
	inner_fails.Op(VbcOp::BeginFailureContext, { W(7), W(0) });
	inner_fails.Op(VbcOp::RefSet, { W(inner_fails.K(Value::from_cell(y))), W(inner_fails.K(Int(heap, 1))) });
	inner_fails.Op(VbcOp::BeginFailureContext, { W(6), W(1) });
	inner_fails.Op(VbcOp::RefSet, { W(inner_fails.K(Value::from_cell(y))), W(inner_fails.K(Int(heap, 2))) });
	inner_fails.Op(VbcOp::Query, { W(3), W(inner_fails.K(heap.false_value())) });
	inner_fails.Op(VbcOp::EndFailureContext, { W(6), W(1) });
	inner_fails.Op(VbcOp::EndFailureContext, { W(7), W(0) });
	inner_fails.Op(VbcOp::Return, { W(inner_fails.K(heap.false_value())) });
	Invoked(interpreter, inner_fails.Function(heap.false_value()), {});
	r_cases.check("failure §7 (Nesting): a committed child's writes are undone with its failed parent; a failed child's alone under a committed parent",
			merged && content(y) == "1");

	// F writes Z = 5 and registers compensation 1; G registers 2 and commits; F fails.
	RefCell *z = var_of(0);
	g_watched = z;
	Asm compensated(heap, "Compensated", 8, 0);
	compensated.Op(VbcOp::BeginFailureContext, { W(8), W(0) });
	compensated.Op(VbcOp::RefSet, { W(compensated.K(Value::from_cell(z))), W(compensated.K(Int(heap, 5))) });
	compensated.Op(VbcOp::Call, { W(2), W(compensated.K(compensate)), L({ compensated.K(Int(heap, 1)) }), L({}), L({}), W(0) });
	compensated.Op(VbcOp::BeginFailureContext, { W(6), W(1) });
	compensated.Op(VbcOp::Call, { W(3), W(compensated.K(compensate)), L({ compensated.K(Int(heap, 2)) }), L({}), L({}), W(0) });
	compensated.Op(VbcOp::EndFailureContext, { W(6), W(1) });
	compensated.Op(VbcOp::Query, { W(4), W(compensated.K(heap.false_value())) });
	compensated.Op(VbcOp::EndFailureContext, { W(8), W(0) });
	compensated.Op(VbcOp::Return, { W(compensated.K(heap.false_value())) });
	g_log.clear();
	Invoked(interpreter, compensated.Function(heap.false_value()), {});
	r_cases.check("failure §7: an abort replays the log, then runs compensations newest first, a committed child's included",
			g_log == std::vector<int64_t>{ 20, 10 } && content(z) == "0");

	Asm kept(heap, "Kept", 4, 0);
	kept.Op(VbcOp::Call, { W(2), W(kept.K(compensate)), L({ kept.K(Int(heap, 3)) }), L({}), L({}), W(0) });
	kept.Op(VbcOp::Return, { W(kept.K(heap.false_value())) });
	g_log.clear();
	Invoked(interpreter, kept.Function(heap.false_value()), {});
	r_cases.check("failure §7: the root commit discards compensations unrun", g_log.empty());
	kept.procedure->ops.pop_back();
	kept.Op(VbcOp::Query, { W(3), W(kept.K(heap.false_value())) });
	kept.Op(VbcOp::Return, { W(kept.K(heap.false_value())) });
	r_cases.check("failure §4: a root that declines runs its compensations", Invoked(interpreter, kept.Function(heap.false_value()), {}) == "<fail>" && g_log == std::vector<int64_t>{ 30 });

	// Root writes W = 1 and registers 1; F writes W = 2, registers 2 and raises.
	RefCell *w = var_of(0);
	g_watched = w;
	Asm raises(heap, "Raises", 8, 0);
	raises.Op(VbcOp::RefSet, { W(raises.K(Value::from_cell(w))), W(raises.K(Int(heap, 1))) });
	raises.Op(VbcOp::Call, { W(2), W(raises.K(compensate)), L({ raises.K(Int(heap, 1)) }), L({}), L({}), W(0) });
	raises.Op(VbcOp::BeginFailureContext, { W(7), W(0) });
	raises.Op(VbcOp::RefSet, { W(raises.K(Value::from_cell(w))), W(raises.K(Int(heap, 2))) });
	raises.Op(VbcOp::Call, { W(3), W(raises.K(compensate)), L({ raises.K(Int(heap, 2)) }), L({}), L({}), W(0) });
	raises.Op(VbcOp::Call, { W(4), W(raises.K(err)), L({ raises.K(Str(heap, "boom")) }), L({}), L({}), W(0) });
	raises.Op(VbcOp::EndFailureContext, { W(7), W(0) });
	raises.Op(VbcOp::Return, { W(raises.K(heap.false_value())) });
	g_log.clear();
	const std::string raised = Invoked(interpreter, raises.Function(heap.false_value()), {});
	r_cases.check("failure §9.3: a raise inside a context is not caught by it and rolls back to the entry, innermost transaction first, each log before its compensations",
			raised.find("<error> ErrorRequested") == 0 && content(w) == "0" && g_log == std::vector<int64_t>{ 21, 10 });

	// Outer writes W = 1, calls a native that re-enters Verse, then writes W = 4.
	RefCell *second = var_of(0);
	Asm outer(heap, "Outer", 4, 0);
	outer.Op(VbcOp::RefSet, { W(outer.K(Value::from_cell(w))), W(outer.K(Int(heap, 1))) });
	outer.Op(VbcOp::Call, { W(2), W(outer.K(reenter)), L({}), L({}), L({}), W(0) });
	outer.Op(VbcOp::RefSet, { W(outer.K(Value::from_cell(w))), W(outer.K(Int(heap, 4))) });
	outer.Op(VbcOp::Return, { W(outer.K(heap.false_value())) });
	const Value outer_fn = outer.Function(heap.false_value());

	Asm inner_raise(heap, "InnerRaise", 4, 0);
	inner_raise.Op(VbcOp::RefSet, { W(inner_raise.K(Value::from_cell(second))), W(inner_raise.K(Int(heap, 3))) });
	inner_raise.Op(VbcOp::Call, { W(2), W(inner_raise.K(err)), L({ inner_raise.K(Str(heap, "inner")) }), L({}), L({}), W(0) });
	inner_raise.Op(VbcOp::Return, { W(inner_raise.K(heap.false_value())) });
	g_reentered = inner_raise.Function(heap.false_value());
	const std::string nested_raise = Invoked(interpreter, outer_fn, {});
	const std::vector<ErrorFrame> &frames = interpreter.error().frames;
	r_cases.check("failure §9.3: a raise in a nested entry rolls back the outer entry too, and stops it at the native",
			nested_raise.find("(User Message: 'inner')") != std::string::npos && content(w) == "0" && content(second) == "0");
	r_cases.check("failure §9.2: a nested raise's frames run through the re-entering native into the outer entry",
			frames.size() == 4 && frames[0].path == "[native]" && frames[1].function == "InnerRaise" && frames[2].function == "Reenter" &&
					frames[3].function == "Outer");

	Asm inner_decline(heap, "InnerDecline", 4, 0);
	inner_decline.Op(VbcOp::RefSet, { W(inner_decline.K(Value::from_cell(second))), W(inner_decline.K(Int(heap, 7))) });
	inner_decline.Op(VbcOp::Query, { W(2), W(inner_decline.K(heap.false_value())) });
	inner_decline.Op(VbcOp::Return, { W(inner_decline.K(heap.false_value())) });
	g_reentered = inner_decline.Function(heap.false_value());
	r_cases.check("failure §4: a nested entry that declines undoes only its own writes; the outer entry goes on and commits",
			Invoked(interpreter, outer_fn, {}) == "false" && content(w) == "4" && content(second) == "0");

	Asm inner_commit(heap, "InnerCommit", 4, 0);
	inner_commit.Op(VbcOp::RefSet, { W(inner_commit.K(Value::from_cell(second))), W(inner_commit.K(Int(heap, 7))) });
	inner_commit.Op(VbcOp::Return, { W(inner_commit.K(heap.false_value())) });
	g_reentered = inner_commit.Function(heap.false_value());
	Invoked(interpreter, InContext(heap, "ReenterThenFail", true, [&](Asm &r_code) {
		r_code.Op(VbcOp::Call, { W(2), W(r_code.K(reenter)), L({}), L({}), L({}), W(0) });
	}),
			{});
	r_cases.check("failure §7: a nested entry that commits joins the context its native was called in, and fails with it", content(second) == "0");

	interpreter.godot.ReleaseObject = &CaptureRelease;
	const Value mint = NativeFunction(heap, "Mint", 1, &MintNative);
	const Value peer_object = make_array(heap, {}, true);
	const auto mints = [&](Asm &r_code) { r_code.Op(VbcOp::Call, { W(2), W(r_code.K(mint)), L({ r_code.K(peer_object) }), L({}), L({}), W(0) }); };
	g_released.clear();
	Invoked(interpreter, InContext(heap, "MintFails", true, mints), {});
	const bool discarded = interpreter.minted_peers.count(42) == 0 && g_released == std::vector<std::pair<int64_t, int>>{ { 42, 1 } };
	g_released.clear();
	Invoked(interpreter, InContext(heap, "MintSucceeds", false, mints), {});
	r_cases.check("godot-natives §4.4, §8.12: a mint in an aborted transaction is discarded at once; a committed one keeps its row",
			discarded && g_released.empty() && interpreter.minted_peers.count(42) == 1);
	interpreter.minted_peers.clear();
}

// A class whose body archetype holds p_entries, with no constructor or blocks of its own.
ClassCell *MakeClass(Heap &r_heap, ClassKind p_kind, std::vector<const ClassCell *> p_inherited, std::vector<ArchetypeEntry> p_entries) {
	ClassCell *made = r_heap.make<ClassCell>();
	ArchetypeCell *body = r_heap.make<ArchetypeCell>();
	made->class_kind = p_kind;
	made->archetype = body;
	made->inherited = std::move(p_inherited);
	body->owner = made;
	if (!made->inherited.empty() && made->inherited[0]->class_kind != ClassKind::Interface) {
		body->next = made->inherited[0]->archetype;
	}
	body->entries = std::move(p_entries);
	return made;
}

void CastCases(Cases &r_cases) {
	Heap heap;
	Program program;
	Interpreter interpreter(heap, program);
	using vbc::VbcOp;

	ClassCell *iface = MakeClass(heap, ClassKind::Interface, {}, {});
	ClassCell *base = MakeClass(heap, ClassKind::Class, {}, {});
	ClassCell *derived = MakeClass(heap, ClassKind::Class, { base, iface }, {});
	ClassCell *point = MakeClass(heap, ClassKind::Struct, {}, {});
	const Value base_object = Value::from_cell(interpreter.layouts.new_object(heap, interpreter.layouts.get(base)));
	const Value derived_object = Value::from_cell(interpreter.layouts.new_object(heap, interpreter.layouts.get(derived)));
	const Value point_object = Value::from_cell(interpreter.layouts.new_object(heap, interpreter.layouts.get(point)));

	const auto bounded = [&](bool p_float, Value p_lower, Value p_upper) {
		BoundedTypeCell *type = heap.make<BoundedTypeCell>(p_float);
		type->lower = p_lower;
		type->upper = p_upper;
		return Value::from_cell(type);
	};
	// TypeCastFastFail, then "ok" with the value or "fails" at OnFailure.
	const auto cast = [&](Value p_type, Value p_value) {
		Asm code(heap, "Cast", 5, 0);
		code.Op(VbcOp::TypeCastFastFail, { W(2), W(3), W(code.K(p_type)), W(code.K(p_value)), W(3) });
		code.Op(VbcOp::NewArray, { W(4), L({ code.K(Str(heap, "ok")), R(2) }) });
		code.Op(VbcOp::Return, { W(R(4)) });
		code.Op(VbcOp::Return, { W(code.K(Str(heap, "fails"))) });
		return Invoked(interpreter, code.Function(heap.false_value()), {});
	};
	const auto full_cast = [&](Value p_type, Value p_value) {
		Asm code(heap, "FullCast", 4, 0);
		code.Op(VbcOp::Call, { W(2), W(code.K(p_type)), L({ code.K(p_value) }), L({}), L({}), W(0) });
		code.Op(VbcOp::Return, { W(R(2)) });
		return Invoked(interpreter, code.Function(heap.false_value()), {});
	};
	const Value class_type = Value::from_cell(base);
	r_cases.check("objects §13: a class admits its own objects and its subclasses', never a superclass's",
			cast(class_type, derived_object).find("\"ok\"") == 1 && cast(Value::from_cell(derived), base_object) == "\"fails\"" &&
					cast(class_type, base_object).find("\"ok\"") == 1);
	r_cases.check("objects §13: an interface admits an object whose class inherits it, and nothing else",
			cast(Value::from_cell(iface), derived_object).find("\"ok\"") == 1 && cast(Value::from_cell(iface), base_object) == "\"fails\"" &&
					cast(Value::from_cell(iface), Int(heap, 3)) == "\"fails\"");
	r_cases.check("objects §13, ops TypeCastFastFail: a cast to a struct is outside the contract",
			cast(Value::from_cell(point), point_object).find("VM invariant violated: a cast to a struct") != std::string::npos);

	const Value digit = bounded(false, Int(heap, 0), Int(heap, 9));
	r_cases.check("values §13 int type: 0 and 9 pass, -1 and 10 fail", cast(digit, Int(heap, 0)) == "[\"ok\",0]" && cast(digit, Int(heap, 9)) == "[\"ok\",9]" &&
			cast(digit, Int(heap, -1)) == "\"fails\"" && cast(digit, Int(heap, 10)) == "\"fails\"");
	r_cases.check("values §13 int type: a rational with denominator 1 passes as itself; 7/2 and a float fail",
			cast(digit, Div(heap, Int(heap, 6), Int(heap, 3))) == "[\"ok\",2/1]" && cast(digit, Div(heap, Int(heap, 7), Int(heap, 2))) == "\"fails\"" &&
					cast(digit, F(1.0)) == "\"fails\"");
	const Value natural = bounded(false, Int(heap, 0), Value::uninitialized());
	r_cases.check("values §13 int type: an uninitialized bound is unbounded, at any size",
			cast(natural, Pow2(heap, 70)) == "[\"ok\",1180591620717411303424]" && cast(natural, Neg(heap, Pow2(heap, 70))) == "\"fails\"" &&
					cast(digit, Pow2(heap, 70)) == "\"fails\"");

	const Value percent = bounded(true, F(0.0), F(1.0));
	r_cases.check("ops TypeCastFastFail float type: 0.5, 0.0, -0.0 and 1.0 pass; 2.0, -1.0, NaN and Inf fail",
			cast(percent, F(0.5)).find("ok") != std::string::npos && cast(percent, F(-0.0)).find("ok") != std::string::npos &&
					cast(percent, F(1.0)).find("ok") != std::string::npos && cast(percent, F(2.0)) == "\"fails\"" && cast(percent, F(-1.0)) == "\"fails\"" &&
					cast(percent, F(NAN)) == "\"fails\"" && cast(percent, F(HUGE_VAL)) == "\"fails\"");
	const Value unbounded = bounded(true, F(-HUGE_VAL), F(NAN));
	const Value to_infinity = bounded(true, F(-HUGE_VAL), F(HUGE_VAL));
	r_cases.check("values §13 float type: NaN only when the lower bound is -Inf and the upper NaN",
			cast(unbounded, F(NAN)).find("ok") != std::string::npos && cast(to_infinity, F(NAN)) == "\"fails\"" &&
					cast(to_infinity, F(HUGE_VAL)).find("ok") != std::string::npos && cast(unbounded, Int(heap, 1)) == "\"fails\"");
	r_cases.check("values §13 float type: bounds left uninitialized read as unbounded",
			cast(bounded(true, Value::uninitialized(), Value::uninitialized()), F(NAN)).find("ok") != std::string::npos);

	SimpleTypeCell *any = heap.make<SimpleTypeCell>();
	SimpleTypeCell *logic = heap.make<SimpleTypeCell>();
	logic->code = 3;
	TupleTypeCell *tuple = heap.make<TupleTypeCell>();
	ElementTypeCell *option = heap.make<ElementTypeCell>(CellKind::OptionType);
	r_cases.check("values §13: `any` admits everything", cast(Value::from_cell(any), Str(heap, "x")) == "[\"ok\",\"x\"]" &&
			cast(Value::from_cell(any), derived_object).find("ok") != std::string::npos);
	r_cases.check("values §13: every other type cell is outside the contract",
			cast(Value::from_cell(tuple), Int(heap, 1)).find("VM invariant violated: a cast to a tuple type") != std::string::npos &&
					cast(Value::from_cell(logic), heap.true_value()).find("VM invariant violated: a cast to a simple type") != std::string::npos &&
					cast(Value::from_cell(option), heap.false_value()).find("VM invariant violated: a cast to a option type") != std::string::npos &&
					cast(Int(heap, 3), Int(heap, 3)).find("a value that is not a type") != std::string::npos);
	r_cases.check("calls §4.1: Call on a type is the same test in a full context, failing the context",
			full_cast(digit, Int(heap, 7)) == "7" && full_cast(digit, Int(heap, 15)) == "<fail>" && full_cast(Value::from_cell(iface), derived_object) != "<fail>");
}

std::vector<std::string> g_events;

Outcome NoteNative(NativeCall &r_call) {
	g_events.push_back(Show(follow(r_call.arguments[0])));
	r_call.result = r_call.heap.false_value();
	return Outcome::Ok;
}

void AccessorCases(Cases &r_cases) {
	Heap heap;
	Program program;
	EnumeratorCell *accessor_enumerator = heap.make<EnumeratorCell>();
	program.accessor_enumerator = accessor_enumerator;
	Interpreter interpreter(heap, program);
	using vbc::VbcOp;
	const Value note = NativeFunction(heap, "Note", 1, &NoteNative);
	const Value marker = Int(heap, 12774014);
	RefCell *store = heap.make<RefCell>(Int(heap, 0));
	const NameCell *level = heap.intern("(/test/prop:)Level");
	const NameCell *other = heap.intern("(/test/prop:)Other");
	const NameCell *getter = heap.intern("(/test/prop:)LevelGetter(:accessor)");
	const NameCell *setter = heap.intern("(/test/prop:)LevelSetter(:accessor,:int)");
	const NameCell *deep_setter = heap.intern("(/test/prop:)LevelSetter(:accessor,:int,:int)");
	SimpleTypeCell *any = heap.make<SimpleTypeCell>();

	// The getter answers the store, or -1 if its first argument is not the accessor enumerator.
	Asm get(heap, "LevelGetter", 7, 1);
	get.Op(VbcOp::EqFastFail, { W(3), W(4), W(R(2)), W(get.K(Value::from_cell(accessor_enumerator))), W(3) });
	get.Op(VbcOp::Call, { W(5), W(get.K(note)), L({ get.K(Str(heap, "get")) }), L({}), L({}), W(0) });
	get.Op(VbcOp::Jump, { W(4) });
	get.Op(VbcOp::Return, { W(get.K(Int(heap, -1))) });
	get.Op(VbcOp::RefGet, { W(6), W(get.K(Value::from_cell(store))) });
	get.Op(VbcOp::Return, { W(R(6)) });
	Asm set(heap, "LevelSetter", 5, 2);
	set.Op(VbcOp::Call, { W(4), W(set.K(note)), L({ R(3) }), L({}), L({}), W(0) });
	set.Op(VbcOp::RefSet, { W(set.K(Value::from_cell(store))), W(R(3)) });
	set.Op(VbcOp::Return, { W(set.K(heap.false_value())) });
	Asm deep(heap, "DeepSetter", 7, 3);
	deep.Op(VbcOp::NewArray, { W(5), L({ deep.K(Str(heap, "step")), R(3), R(4) }) });
	deep.Op(VbcOp::Call, { W(6), W(deep.K(note)), L({ R(5) }), L({}), L({}), W(0) });
	deep.Op(VbcOp::Return, { W(deep.K(heap.false_value())) });

	AccessorCell *accessor = heap.make<AccessorCell>();
	accessor->getters = { getter };
	accessor->setters = { setter, deep_setter };
	ClassCell *prop = MakeClass(heap, ClassKind::Class, {}, {
		ArchetypeEntry{ level, nullptr, Value::from_cell(any), Value::from_cell(accessor), 0 },
		ArchetypeEntry{ other, nullptr, Value::from_cell(any), Value::uninitialized(), 0 },
		ArchetypeEntry{ getter, nullptr, Value::uninitialized(), get.Function(), 0 },
		ArchetypeEntry{ setter, nullptr, Value::uninitialized(), set.Function(), 0 },
		ArchetypeEntry{ deep_setter, nullptr, Value::uninitialized(), deep.Function(), 0 },
	});
	// The constructor: Level's CreateField (and with p_defers its InitializeVar, which the compiler
	// emits only for an archetype that sets the member), then Other's initializer noting "other".
	const auto constructor_body = [&](bool p_defers) {
		Asm code(heap, "PropConstructor", 8, 3);
		code.Op(VbcOp::CreateField, { W(5), W(R(2)), W(R(0)), W(code.C(Value::from_cell(level))), W(0) });
		if (p_defers) {
			code.Op(VbcOp::InitializeVar, { W(7), W(R(2)), W(R(0)), W(code.C(Value::from_cell(level))), W(code.K(Int(heap, 3))), W(kAbsentOperand), W(1) });
		}
		const size_t create_other = code.procedure->ops.size();
		code.Op(VbcOp::CreateField, { W(5), W(R(2)), W(R(0)), W(code.C(Value::from_cell(other))), W(0) });
		code.Op(VbcOp::Call, { W(6), W(code.K(note)), L({ code.K(Str(heap, "other")) }), L({}), L({}), W(0) });
		code.Op(VbcOp::UnifyField, { W(R(0)), W(code.C(Value::from_cell(other))), W(code.K(Int(heap, 1))) });
		code.Op(VbcOp::Return, { W(R(p_defers ? 7 : 2)) });
		const auto on_failure = [&](size_t p_op, size_t p_label) {
			code.procedure->operand_words[code.procedure->ops[p_op].operands + 4] = uint32_t(p_label);
		};
		on_failure(0, create_other);
		on_failure(create_other, code.procedure->ops.size() - 1);
		return code.Function();
	};
	Asm blocks(heap, "PropBlocks", 4, 0);
	blocks.Op(VbcOp::Call, { W(2), W(blocks.K(note)), L({ blocks.K(Str(heap, "block")) }), L({}), L({}), W(0) });
	blocks.Op(VbcOp::Return, { W(blocks.K(heap.false_value())) });
	prop->constructor = cell_as<FunctionCell>(constructor_body(false));
	prop->blocks = cell_as<FunctionCell>(blocks.Function());

	ObjectCell *object = interpreter.layouts.new_object(heap, interpreter.layouts.get(prop));
	const Value object_value = Value::from_cell(object);
	const LayoutField *level_field = interpreter.layouts.get(prop).find(level);
	r_cases.check("objects §4.4, §16: an accessor entry is an accessor in the layout, not a slot or a constant",
			level_field != nullptr && level_field->kind == FieldKind::Accessor);

	Asm read(heap, "ReadLevel", 6, 0);
	read.Op(VbcOp::LoadField, { W(2), W(read.K(object_value)), W(read.C(Value::from_cell(level))) });
	read.Op(VbcOp::RefGet, { W(3), W(R(2)) });
	read.Op(VbcOp::Freeze, { W(4), W(R(3)) });
	read.Op(VbcOp::Return, { W(R(4)) });
	store->content = Int(heap, 41);
	g_events.clear();
	r_cases.check("objects §16, ops RefGet/Freeze: LoadField answers a reference, RefGet passes it, Freeze calls the getter with the accessor enumerator",
			Invoked(interpreter, read.Function(heap.false_value()), {}) == "41" && g_events == std::vector<std::string>{ "\"get\"" });

	Asm write(heap, "WriteLevel", 6, 0);
	write.Op(VbcOp::LoadField, { W(2), W(write.K(object_value)), W(write.C(Value::from_cell(level))) });
	write.Op(VbcOp::RefCallDomain, { W(3), W(R(2)), W(write.K(Int(heap, 5))) });
	write.Op(VbcOp::Melt, { W(4), W(R(3)) });
	write.Op(VbcOp::RefSet, { W(R(2)), W(R(4)) });
	write.Op(VbcOp::FreezeIfAccessor, { W(5), W(R(2)) });
	write.Op(VbcOp::Return, { W(R(5)) });
	g_events.clear();
	r_cases.check("objects §16, ops RefSet/FreezeIfAccessor: a write calls the setter with the value, a FreezeIfAccessor the getter",
			Invoked(interpreter, write.Function(heap.false_value()), {}) == "5" && g_events == std::vector<std::string>{ "5", "\"get\"" });

	Asm field_write(heap, "SetFieldLevel", 4, 0);
	field_write.Op(VbcOp::SetField, { W(field_write.K(object_value)), W(field_write.C(Value::from_cell(level))), W(field_write.K(Int(heap, 6))) });
	field_write.Op(VbcOp::Return, { W(field_write.K(heap.false_value())) });
	g_events.clear();
	Invoked(interpreter, field_write.Function(heap.false_value()), {});
	r_cases.check("objects §7.7: SetField of an accessor member calls the setter", g_events == std::vector<std::string>{ "6" } && Show(store->content) == "6");

	Asm path(heap, "DeepWrite", 6, 0);
	path.Op(VbcOp::LoadField, { W(2), W(path.K(object_value)), W(path.C(Value::from_cell(level))) });
	path.Op(VbcOp::Call, { W(3), W(R(2)), L({ path.K(Int(heap, 7)) }), L({}), L({}), W(0) });
	path.Op(VbcOp::RefSet, { W(R(3)), W(path.K(Int(heap, 9))) });
	path.Op(VbcOp::CallSet, { W(R(2)), W(path.K(Int(heap, 8))), W(path.K(Int(heap, 10))) });
	path.Op(VbcOp::Return, { W(path.K(heap.false_value())) });
	g_events.clear();
	Invoked(interpreter, path.Function(heap.false_value()), {});
	r_cases.check("ops §15.1 item 7, CallSet: Call on an accessor reference and CallSet through one extend its path; the setter taking n parameters is at index n - 2",
			g_events == std::vector<std::string>{ "[\"step\",7,9]", "[\"step\",8,10]" });

	// prop{Level := 9, Other := 1}: Level's setter is deferred past Other's initializer, and runs
	// before the blocks.
	ArchetypeCell *expression = heap.make<ArchetypeCell>();
	expression->entries.push_back(ArchetypeEntry{ level, nullptr, Value::uninitialized(), Value::uninitialized(), 0 });
	expression->entries.push_back(ArchetypeEntry{ other, nullptr, Value::uninitialized(), Value::uninitialized(), 0 });
	Asm make(heap, "MakeProp", 10, 0);
	const uint32_t level_name = make.C(Value::from_cell(level));
	const uint32_t other_name = make.C(Value::from_cell(other));
	make.Op(VbcOp::NewObject, { W(2), W(make.K(Value::from_cell(expression))), W(make.K(Value::from_cell(prop))) });
	make.Op(VbcOp::CreateField, { W(3), W(make.K(marker)), W(R(2)), W(level_name), W(2) });
	make.Op(VbcOp::InitializeVar, { W(4), W(make.K(marker)), W(R(2)), W(level_name), W(make.K(Int(heap, 9))), W(kAbsentOperand), W(1) });
	make.Op(VbcOp::CreateField, { W(3), W(R(4)), W(R(2)), W(other_name), W(6) });
	make.Op(VbcOp::Call, { W(5), W(make.K(note)), L({ make.K(Str(heap, "archetype other")) }), L({}), L({}), W(0) });
	make.Op(VbcOp::UnifyField, { W(R(2)), W(other_name), W(make.K(Int(heap, 1))) });
	make.Op(VbcOp::CallWithSelf, { W(6), W(make.K(Value::from_cell(prop->constructor))), W(R(2)),
			L({ R(4), make.K(Value::uninitialized()), make.K(Value::uninitialized()) }), L({}), L({}), W(0) });
	make.Op(VbcOp::UnifyNativeObject, { W(R(6)), W(R(2)) });
	make.Op(VbcOp::Call, { W(7), W(make.K(note)), L({ make.K(Str(heap, "done")) }), L({}), L({}), W(0) });
	make.Op(VbcOp::Return, { W(make.K(heap.false_value())) });
	g_events.clear();
	Invoked(interpreter, make.Function(heap.false_value()), {});
	r_cases.check("objects §7.6, §7.8: InitializeVar defers an accessor's setter; UnifyNativeObject runs it after every initializer and before the blocks",
			g_events == std::vector<std::string>{ "\"archetype other\"", "9", "\"block\"", "\"done\"" });

	ClassCell *deferring = MakeClass(heap, ClassKind::Class, {}, prop->archetype->entries);
	deferring->constructor = cell_as<FunctionCell>(constructor_body(true));
	deferring->blocks = prop->blocks;
	g_events.clear();
	interpreter.begin_entry();
	Value built;
	const Outcome outcome = interpreter.construct(deferring, 0, built);
	interpreter.end_entry(outcome == Outcome::Ok);
	r_cases.check("objects §7.11: a host-built object runs the setters its constructor deferred, then its blocks",
			outcome == Outcome::Ok && g_events == std::vector<std::string>{ "\"other\"", "3", "\"block\"" });
	g_events.clear();
	interpreter.begin_entry();
	const Outcome unblocked = interpreter.construct(deferring, 0, built, false);
	interpreter.end_entry(unblocked == Outcome::Ok);
	r_cases.check("objects §8.3: a class default object runs the deferred setters and no blocks",
			unblocked == Outcome::Ok && g_events == std::vector<std::string>{ "\"other\"", "3" });
}

void NativeFieldCases(Cases &r_cases) {
	Heap heap;
	Program program;
	Interpreter interpreter(heap, program);
	using vbc::VbcOp;
	SimpleTypeCell *any = heap.make<SimpleTypeCell>();
	const NameCell *lane = heap.intern("(/test/box:)I0");
	const NameCell *handle = heap.intern("(/test/box:)Handle");
	const NameCell *plain = heap.intern("(/test/box:)Plain");
	ClassCell *box = MakeClass(heap, ClassKind::Struct, {}, {
		ArchetypeEntry{ lane, nullptr, Value::from_cell(any), Value::uninitialized(), 1 },
		ArchetypeEntry{ handle, nullptr, Value::from_cell(any), Value::uninitialized(), 1 | 32 },
		ArchetypeEntry{ plain, nullptr, Value::from_cell(any), Value::uninitialized(), 0 },
	});
	const auto store = [&](const NameCell *p_name, Value p_value) {
		ObjectCell *object = interpreter.layouts.new_object(heap, interpreter.layouts.get(box));
		Asm code(heap, "Store", 4, 0);
		const uint32_t name = code.C(Value::from_cell(p_name));
		code.Op(VbcOp::CreateField, { W(2), W(code.K(Int(heap, 12774014))), W(code.K(Value::from_cell(object))), W(name), W(3) });
		code.Op(VbcOp::UnifyField, { W(code.K(Value::from_cell(object))), W(name), W(code.K(p_value)) });
		code.Op(VbcOp::LoadField, { W(3), W(code.K(Value::from_cell(object))), W(name) });
		code.Op(VbcOp::Return, { W(R(3)) });
		return Invoked(interpreter, code.Function(heap.false_value()), {});
	};
	const std::string exceeds =
			"<error> ErrRuntime_GeneratedNativeInternal: An internal runtime error occurred in (generated) native code that was called from Verse. There is no other information available. (Value exceeds the range of a 64 bit integer.)";
	r_cases.check("objects §9.2: an int that fits in 64 bits stores in a native field and reads back", store(lane, Sub(heap, Pow2(heap, 63), Int(heap, 1))) == "9223372036854775807" &&
			store(lane, Neg(heap, Pow2(heap, 63))) == "-9223372036854775808");
	r_cases.check("objects §9.2: storing 2^70 into a native int field raises the 64-bit range error", store(lane, Pow2(heap, 70)) == exceeds && store(lane, Pow2(heap, 63)) == exceeds);
	r_cases.check("objects §9.2: a field without the native flag holds any int", store(plain, Pow2(heap, 70)) == "1180591620717411303424");
	r_cases.check("objects §9.2: float, string and logic always fit", store(lane, F(-0.5)) == "-0.500000" && store(lane, Str(heap, "s")) == "\"s\"" && store(lane, heap.true_value()) == "true");

	// A native `var`: InitializeVar converts, and so does every later RefSet through the variable.
	const auto var_store = [&](Value p_initial, Value p_later) {
		ObjectCell *object = interpreter.layouts.new_object(heap, interpreter.layouts.get(box));
		Asm code(heap, "VarStore", 6, 0);
		const uint32_t name = code.C(Value::from_cell(handle));
		code.Op(VbcOp::InitializeVar, { W(2), W(code.K(Int(heap, 12774014))), W(code.K(Value::from_cell(object))), W(name), W(code.K(p_initial)), W(kAbsentOperand), W(1) });
		code.Op(VbcOp::LoadField, { W(3), W(code.K(Value::from_cell(object))), W(name) });
		code.Op(VbcOp::RefSet, { W(R(3)), W(code.K(p_later)) });
		code.Op(VbcOp::RefGet, { W(4), W(R(3)) });
		code.Op(VbcOp::Return, { W(R(4)) });
		return Invoked(interpreter, code.Function(heap.false_value()), {});
	};
	r_cases.check("objects §9.2: a native var converts on initialization and on every write",
			var_store(Int(heap, 1), Int(heap, 2)) == "2" && var_store(Pow2(heap, 64), Int(heap, 2)) == exceeds && var_store(Int(heap, 1), Pow2(heap, 64)) == exceeds);
}

void StructConstantCases(Cases &r_cases) {
	Heap heap;
	Program program;
	Interpreter interpreter(heap, program);
	SimpleTypeCell *any = heap.make<SimpleTypeCell>();
	const NameCell *x = heap.intern("(/test/pair:)X");
	const NameCell *y = heap.intern("(/test/pair:)Y");
	ClassCell *pair = MakeClass(heap, ClassKind::Struct, {}, {
		ArchetypeEntry{ x, nullptr, Value::from_cell(any), Value::uninitialized(), 0 },
		ArchetypeEntry{ y, nullptr, Value::uninitialized(), Int(heap, 5), 0 },
	});
	const ClassLayout &layout = interpreter.layouts.get(pair);
	ObjectCell *laid_out = interpreter.layouts.new_object(heap, layout);
	laid_out->field_values[layout.find(x)->slot] = Int(heap, 1);
	const auto stored = [&](int64_t p_y) {
		ObjectCell *object = heap.make<ObjectCell>();
		object->object_class = pair;
		object->layout = &layout;
		object->field_names = { x, y };
		object->field_values = { Int(heap, 1), Int(heap, p_y) };
		return Value::from_cell(object);
	};
	r_cases.check("objects §10.2: a field one struct holds as its class's constant equals the same value held in a slot, either way round",
			layout.find(y)->kind == FieldKind::Constant && Compare(Value::from_cell(laid_out), stored(5)) == Equality::Eq &&
					Compare(stored(5), Value::from_cell(laid_out)) == Equality::Eq);
	r_cases.check("objects §10.2: and differs from another value in a slot", Compare(Value::from_cell(laid_out), stored(6)) == Equality::Neq &&
			Compare(stored(6), Value::from_cell(laid_out)) == Equality::Neq);
}

// Stand-ins for event(t)'s Await and Signal (T4.3): Wait(K) suspends the calling task under key K,
// with a hook that takes it off again; Signal(K, V) completes every task waiting under K, oldest
// first, each running to its next stop before the next.
std::map<int64_t, std::vector<TaskCell *>> g_waiting;

void LeaveWaiting(TaskCell *p_task, Cell *) {
	for (auto &entry : g_waiting) {
		entry.second.erase(std::remove(entry.second.begin(), entry.second.end(), p_task), entry.second.end());
	}
}

Outcome WaitNative(NativeCall &r_call) {
	TaskCell *waiter = r_call.interpreter->current_task();
	g_waiting[int_value(follow(r_call.arguments[0])).to_int64()].push_back(waiter);
	waiter->defer_hooks.push_back(TaskHook{ &LeaveWaiting, nullptr });
	return Outcome::Yield;
}

Outcome SignalNative(NativeCall &r_call) {
	std::vector<TaskCell *> batch;
	batch.swap(g_waiting[int_value(follow(r_call.arguments[0])).to_int64()]);
	for (TaskCell *waiter : batch) {
		if (r_call.interpreter->complete(waiter, follow(r_call.arguments[1])) != Outcome::Ok) {
			break;
		}
	}
	r_call.result = r_call.heap.false_value();
	return Outcome::Ok;
}

Value NativeProcedure(Heap &r_heap, const char *p_key, uint32_t p_count) {
	NativeProcedureCell *native = r_heap.make<NativeProcedureCell>();
	native->binding_key = r_heap.intern(p_key);
	native->decorated_name = r_heap.intern(p_key);
	native->positional_count = p_count;
	native->implementation = native_implementation(p_key);
	native->bound = native->implementation != nullptr;
	return Value::from_cell(native);
}

Outcome InvokeValue(Interpreter &r_interpreter, Value p_function, Value &r_result) {
	r_interpreter.begin_entry();
	const Outcome outcome = r_interpreter.invoke(p_function, Value::uninitialized(), {}, {}, r_result);
	r_interpreter.end_entry(outcome == Outcome::Ok || outcome == Outcome::Yield);
	return outcome;
}

bool Logged(std::initializer_list<int64_t> p_expected) {
	const bool same = g_log == std::vector<int64_t>(p_expected);
	if (!same) {
		printf("    log:");
		for (int64_t entry : g_log) {
			printf(" %lld", static_cast<long long>(entry));
		}
		printf("\n");
	}
	g_log.clear();
	return same;
}

void TaskCases(Cases &r_cases) {
	Heap heap;
	Program program;
	Interpreter interpreter(heap, program);
	using vbc::VbcOp;
	const Value log = NativeFunction(heap, "Log", 1, &LogNative);
	const Value wait = NativeFunction(heap, "Wait", 1, &WaitNative);
	const Value signal = NativeFunction(heap, "Signal", 2, &SignalNative);
	const Value await = NativeProcedure(heap, "(/Verse.org/Concurrency/task/Await:)Native", 0);
	const Value cancel = NativeProcedure(heap, "(/Verse.org/Concurrency/task/Cancel:)Native", 0);
	const auto Log = [&](Asm &r_code, uint32_t p_scratch, int64_t p_value) {
		r_code.Op(VbcOp::Call, { W(p_scratch), W(r_code.K(log)), L({ r_code.K(Int(heap, p_value)) }), L({}), L({}), W(0) });
	};
	const auto Wait = [&](Asm &r_code, uint32_t p_dest, int64_t p_key) {
		r_code.Op(VbcOp::Call, { W(p_dest), W(r_code.K(wait)), L({ r_code.K(Int(heap, p_key)) }), L({}), L({}), W(1) });
	};
	const auto EndTask = [&](Asm &r_code, uint32_t p_write, uint32_t p_switch, uint32_t p_value, uint32_t p_which, uint32_t p_signal) {
		r_code.Op(VbcOp::EndTask, { W(p_write), W(p_switch), W(p_value), W(p_which), W(p_signal) });
	};
	const auto Signaler = [&](int64_t p_key, int64_t p_value) {
		Asm code(heap, "Signaler", 4, 0);
		code.Op(VbcOp::Call, { W(2), W(code.K(signal)), L({ code.K(Int(heap, p_key)), code.K(Int(heap, p_value)) }), L({}), L({}), W(0) });
		Log(code, 3, 99);
		code.Op(VbcOp::Return, { W(code.K(heap.false_value())) });
		return code.Function(heap.false_value());
	};
	const uint32_t none = kAbsentOperand;
	g_log.clear();
	g_waiting.clear();

	// TBody: Log 1; Wait(5); Log 50; EndTask 6. Two awaiters log 71 and 72 after T.Await().
	Asm body(heap, "TBody", 4, 0);
	Log(body, 3, 1);
	Wait(body, 2, 5);
	Log(body, 3, 50);
	EndTask(body, none, none, body.K(Int(heap, 6)), none, none);
	body.procedure->unwind_edges.push_back(UnwindEdge{ 0, 2, 3 });
	const auto Awaiter = [&](int64_t p_id) {
		Asm code(heap, "Awaiter", 5, 1);
		code.Op(VbcOp::CallWithSelf, { W(3), W(code.K(await)), W(R(2)), L({}), L({}), L({}), W(1) });
		Log(code, 4, p_id);
		EndTask(code, none, none, R(3), none, none);
		code.procedure->unwind_edges.push_back(UnwindEdge{ 0, 1, 2 });
		return code.Function(heap.false_value());
	};
	Asm spawner(heap, "Spawner", 8, 0);
	Log(spawner, 5, 0);
	spawner.Op(VbcOp::CallTask, { W(2), W(none), W(spawner.K(body.Function(heap.false_value()))), L({}) });
	Log(spawner, 5, 3);
	spawner.Op(VbcOp::CallTask, { W(3), W(none), W(spawner.K(Awaiter(71))), L({ R(2) }) });
	spawner.Op(VbcOp::CallTask, { W(4), W(none), W(spawner.K(Awaiter(72))), L({ R(2) }) });
	spawner.Op(VbcOp::Return, { W(spawner.K(heap.false_value())) });
	Value result;
	r_cases.check("tasks §6.4: a spawned body runs at once until it suspends, then the op after the spawn runs",
			InvokeValue(interpreter, spawner.Function(heap.false_value()), result) == Outcome::Ok && Logged({ 0, 1, 3 }) && g_waiting[5].size() == 1);
	TaskCell *spawned = g_waiting[5].empty() ? nullptr : g_waiting[5][0];
	InvokeValue(interpreter, Signaler(5, 0), result);
	r_cases.check("tasks §4.3, §5.4 step 8: a finishing task resumes its awaiters oldest first, each to its next stop, before its yield-to point",
			Logged({ 50, 71, 72, 99 }) && spawned != nullptr && spawned->has_result && spawned->finished && spawned->phase == TaskCell::Phase::Active);

	// Two arms end at once; the first EndTask's writes win, and WaitSemaphore after both signals
	// does not suspend.
	Asm first(heap, "FirstWins", 10, 0);
	first.Op(VbcOp::NewSemaphore, { W(2) });
	first.Op(VbcOp::Move, { W(3), W(none) });
	first.Op(VbcOp::Move, { W(4), W(none) });
	first.Op(VbcOp::SelfTask, { W(5) });
	first.Op(VbcOp::BeginTask, { W(6), W(R(5)), W(1), W(6) });
	EndTask(first, 3, 4, first.K(Int(heap, 10)), first.K(Int(heap, 0)), R(2));
	first.Op(VbcOp::BeginTask, { W(7), W(R(5)), W(1), W(8) });
	EndTask(first, 3, 4, first.K(Int(heap, 20)), first.K(Int(heap, 1)), R(2));
	first.Op(VbcOp::WaitSemaphore, { W(R(2)), W(2) });
	first.Op(VbcOp::NewArray, { W(8), L({ R(3), R(4) }) });
	first.Op(VbcOp::Return, { W(R(8)) });
	r_cases.check("tasks §5.4 step 2.3: EndTask's Write and Switch are first-writer-wins; §5.6: a wait already signalled does not suspend",
			Invoked(interpreter, first.Function(heap.false_value()), {}) == "[10,0]");

	// race{Arm 1; Arm 2; Arm 3} as §6.3 compiles it, the wrapper a root task; each arm logs i,
	// waits under 10 + i, logs 20 + i, and has a defer logging 100 + i.
	Asm race(heap, "Race", 14, 0);
	race.Op(VbcOp::NewSemaphore, { W(2) });
	race.Op(VbcOp::Move, { W(3), W(none) });
	race.Op(VbcOp::BeginTask, { W(9), W(none), W(1), W(33) });
	race.Op(VbcOp::SelfTask, { W(4) });
	for (uint32_t arm = 1; arm <= 3; ++arm) {
		const uint32_t start = uint32_t(race.procedure->ops.size());
		race.Op(VbcOp::JumpIfInitialized, { W(R(3)), W(31) });
		race.Op(VbcOp::BeginTask, { W(4 + arm), W(R(4)), W(1), W(start + 9) });
		Log(race, 8, arm);
		Wait(race, 10 + arm, 10 + arm);
		Log(race, 8, 20 + arm);
		Log(race, 8, 100 + arm);
		EndTask(race, 3, none, race.K(Int(heap, arm)), none, R(2));
		Log(race, 8, 100 + arm);
		race.Op(VbcOp::Jump, { W(start + 6) });
		race.procedure->unwind_edges.push_back(UnwindEdge{ start + 2, start + 3, start + 7 });
	}
	race.Op(VbcOp::WaitSemaphore, { W(R(2)), W(1) });
	EndTask(race, none, none, race.K(heap.false_value()), none, none);
	race.Op(VbcOp::Return, { W(race.K(heap.false_value())) });
	InvokeValue(interpreter, race.Function(heap.false_value()), result);
	r_cases.check("tasks §6.5 B3_Start: every arm starts in source order and runs to its first suspension", Logged({ 1, 2, 3 }));
	InvokeValue(interpreter, Signaler(12, 0), result);
	r_cases.check("tasks §6.5 B3_SignalMiddle: the winner's end and defer, then the losers cancelled newest first, before the signaller continues",
			Logged({ 22, 102, 103, 101, 99 }) && g_waiting[11].empty() && g_waiting[13].empty());

	// F2_Cancel: a task suspended two frames down, each frame with defers, cancelled from outside.
	Asm inner(heap, "FInner", 4, 0);
	Wait(inner, 2, 7);
	inner.Op(VbcOp::Return, { W(inner.K(heap.false_value())) });
	Log(inner, 3, 3);
	Log(inner, 3, 2);
	inner.Op(VbcOp::ResumeUnwind, {});
	inner.procedure->unwind_edges.push_back(UnwindEdge{ 0, 0, 2 });
	Asm outer(heap, "FOuter", 4, 0);
	outer.Op(VbcOp::Call, { W(2), W(outer.K(inner.Function(heap.false_value()))), L({}), L({}), L({}), W(1) });
	outer.Op(VbcOp::Return, { W(outer.K(heap.false_value())) });
	Log(outer, 3, 1);
	outer.Op(VbcOp::ResumeUnwind, {});
	outer.procedure->unwind_edges.push_back(UnwindEdge{ 0, 0, 2 });
	Asm cancelled(heap, "CancelledBody", 3, 0);
	cancelled.Op(VbcOp::Call, { W(2), W(cancelled.K(outer.Function(heap.false_value()))), L({}), L({}), L({}), W(1) });
	EndTask(cancelled, none, none, cancelled.K(heap.false_value()), none, none);
	cancelled.procedure->unwind_edges.push_back(UnwindEdge{ 0, 0, 1 });
	Asm canceler(heap, "Canceler", 6, 0);
	canceler.Op(VbcOp::CallTask, { W(2), W(none), W(canceler.K(cancelled.Function(heap.false_value()))), L({}) });
	Log(canceler, 3, 0);
	canceler.Op(VbcOp::CallWithSelf, { W(4), W(canceler.K(cancel)), W(R(2)), L({}), L({}), L({}), W(1) });
	Log(canceler, 3, 9);
	canceler.Op(VbcOp::Return, { W(R(2)) });
	const Outcome cancel_outcome = InvokeValue(interpreter, canceler.Function(heap.false_value()), result);
	r_cases.check("tasks §7.4 F2_Cancel: Cancel unwinds a suspended task synchronously, defers innermost first across frames, then returns",
			cancel_outcome == Outcome::Ok && Logged({ 0, 3, 2, 1, 9 }) && g_waiting[7].empty());
	const auto Query = [&](const char *p_name) {
		NativeCall call(heap);
		call.self = result;
		return Interpreter::task_native(std::string("(/Verse.org/Concurrency/task/(/Verse.org/Concurrency/task:)") + p_name + ":)Native")(call) == Outcome::Ok;
	};
	r_cases.check("tasks §3 F2_Cancel: a cancelled task answers Canceled, Settled and Interrupted, never Completed or Active",
			is_cell_kind(result, CellKind::Task) && Query("Canceled") && Query("Settled") && Query("Interrupted") && !Query("Completed") &&
					!Query("Active") && !Query("Canceling") && !Query("Unsettled") && !Query("Uninterrupted"));

	// H2_Signal: a child cancels its running parent; the parent carries on to its next suspension
	// point, where it unwinds the child (still waiting in Cancel) and then itself.
	Asm child(heap, "ChildBody", 5, 1);
	child.Op(VbcOp::CallWithSelf, { W(3), W(child.K(cancel)), W(R(2)), L({}), L({}), L({}), W(1) });
	Log(child, 4, 300);
	EndTask(child, none, none, child.K(heap.false_value()), none, none);
	Log(child, 4, 200);
	child.Op(VbcOp::Jump, { W(2) });
	child.procedure->unwind_edges.push_back(UnwindEdge{ 0, 1, 3 });
	Asm parent(heap, "ParentBody", 6, 0);
	parent.Op(VbcOp::SelfTask, { W(2) });
	parent.Op(VbcOp::CallTask, { W(3), W(R(2)), W(parent.K(child.Function(heap.false_value()))), L({ R(2) }) });
	Log(parent, 4, 5);
	Wait(parent, 5, 3);
	EndTask(parent, none, none, parent.K(heap.false_value()), none, none);
	Log(parent, 4, 100);
	parent.Op(VbcOp::Jump, { W(4) });
	parent.procedure->unwind_edges.push_back(UnwindEdge{ 0, 3, 5 });
	Asm starter(heap, "Starter", 3, 0);
	starter.Op(VbcOp::CallTask, { W(2), W(none), W(starter.K(parent.Function(heap.false_value()))), L({}) });
	starter.Op(VbcOp::Return, { W(R(2)) });
	InvokeValue(interpreter, starter.Function(heap.false_value()), result);
	r_cases.check("tasks §7.5 H2_Signal: a running target carries on to its next suspension point; the descendant's Cancel never returns",
			Logged({ 5, 200, 100 }) && g_waiting[3].empty() && is_cell_kind(result, CellKind::Task) &&
					cell_as<TaskCell>(result)->phase == TaskCell::Phase::Canceled);

	// A spawned task in a scope that is then terminated: no defer, hooks run, never resumed.
	Asm sleeper(heap, "SleeperBody", 4, 0);
	Wait(sleeper, 2, 8);
	EndTask(sleeper, none, none, sleeper.K(heap.false_value()), none, none);
	Log(sleeper, 3, 555);
	sleeper.Op(VbcOp::Jump, { W(1) });
	sleeper.procedure->unwind_edges.push_back(UnwindEdge{ 0, 0, 2 });
	Asm spawn_sleeper(heap, "SpawnSleeper", 3, 0);
	spawn_sleeper.Op(VbcOp::CallTask, { W(2), W(none), W(spawn_sleeper.K(sleeper.Function(heap.false_value()))), L({}) });
	spawn_sleeper.Op(VbcOp::Return, { W(R(2)) });
	ContentScopeCell *scope = interpreter.make_scope();
	interpreter.active_scope = scope;
	InvokeValue(interpreter, spawn_sleeper.Function(heap.false_value()), result);
	const bool grouped = scope->group.size() == 1 && g_waiting[8].size() == 1;
	interpreter.terminate_scope(scope);
	r_cases.check("tasks §8.2-8.3: terminating a scope cancels its root tasks without running a defer, and runs their native hooks",
			grouped && scope->terminated && scope->group.empty() && g_waiting[8].empty() && Logged({}) &&
					cell_as<TaskCell>(result)->phase == TaskCell::Phase::Canceled && cell_as<TaskCell>(result)->finished);

	// A raise terminates the active scope, and the task suspended under it is never resumed.
	const Value err = NativeFunction(heap, "Err", 1, native_implementation("(/Verse.org/Verse/(/Verse.org/Verse:)Err(:[]char):)Native"));
	ContentScopeCell *raising = interpreter.make_scope();
	interpreter.active_scope = raising;
	InvokeValue(interpreter, spawn_sleeper.Function(heap.false_value()), result);
	TaskCell *doomed = cell_as<TaskCell>(result);
	Asm raiser(heap, "Raiser", 3, 0);
	raiser.Op(VbcOp::Call, { W(2), W(raiser.K(err)), L({ raiser.K(Str(heap, "boom")) }), L({}), L({}), W(0) });
	raiser.Op(VbcOp::Return, { W(raiser.K(heap.false_value())) });
	const Outcome raised = InvokeValue(interpreter, raiser.Function(heap.false_value()), result);
	r_cases.check("tasks §8.4 R2_Raise: a raise terminates the active scope; its suspended task is canceled with no defer",
			raised == Outcome::Error && raising->terminated && doomed->phase == TaskCell::Phase::Canceled && g_waiting[8].empty() && Logged({}));
	interpreter.active_scope = nullptr;

	// §4.4: an entry whose own task suspends answers Yield with no result.
	Asm suspends(heap, "Suspends", 3, 0);
	Wait(suspends, 2, 9);
	suspends.Op(VbcOp::Return, { W(R(2)) });
	r_cases.check("tasks §4.4: an entry function that suspends answers the suspended outcome, with no result",
			Invoked(interpreter, suspends.Function(heap.false_value()), {}) == "<suspended>" && g_waiting[9].size() == 1);
	InvokeValue(interpreter, Signaler(9, 4), result);
	r_cases.check("tasks §11: completing the entry task's suspension runs it on to its Return", Logged({ 99 }) && g_waiting[9].empty());
	g_waiting.clear();
}

double g_now = 0.0;
std::string g_runtime_error;

double FakeClock() {
	return g_now;
}

void CaptureRuntimeError(void *, const vh_runtime_error *p_error) {
	g_runtime_error.assign(p_error->MessageUtf8, size_t(p_error->MessageLen));
}

// spec/natives.md §7, godot-natives.md §10 and spec/tasks.md §5.7 with the real natives: event(t)
// on a bare object (its awaiters live in native state), Sleep woken by Runtime::tick on a clock
// this test moves, and await, batch and live writes over hand-written bytecode shaped the way the
// compiler shapes them (tests/vm_conformance/tasks_live.verse has the compiled forms).
void AwaitEventSleepCases(Cases &r_cases) {
	Runtime runtime;
	Heap &heap = runtime.heap;
	// This case holds its cells in C++ locals, which are no roots, so the tick must not collect.
	heap.min_collect_trigger = SIZE_MAX;
	Interpreter &interpreter = runtime.interpreter;
	interpreter.clock = &FakeClock;
	runtime.on_runtime_error = &CaptureRuntimeError;
	using vbc::VbcOp;
	const uint32_t none = kAbsentOperand;
	const Value no = heap.false_value();
	const Value log = NativeFunction(heap, "Log", 1, &LogNative);
	const Value await = NativeProcedure(heap, "(/Verse.org/Verse/event/Await:)Native", 0);
	const Value signal = NativeProcedure(heap, "(/Verse.org/Verse/event/(/Verse.org/Verse/signalable:)Signal(:payload):)Native", 1);
	const Value cancel = NativeProcedure(heap, "(/Verse.org/Concurrency/task/Cancel:)Native", 0);
	const Value sleep = NativeFunction(heap, "Sleep", 1, native_implementation("(/Godot.org/Godot/Sleep(:float):)Native"));
	const Value err = NativeFunction(heap, "Err", 1, native_implementation("(/Verse.org/Verse/(/Verse.org/Verse:)Err(:[]char):)Native"));
	const auto Log = [&](Asm &r_code, uint32_t p_scratch, int64_t p_value) {
		r_code.Op(VbcOp::Call, { W(p_scratch), W(r_code.K(log)), L({ r_code.K(Int(heap, p_value)) }), L({}), L({}), W(0) });
	};
	const auto Sleep = [&](Asm &r_code, uint32_t p_dest, double p_seconds) {
		r_code.Op(VbcOp::Call, { W(p_dest), W(r_code.K(sleep)), L({ r_code.K(F(p_seconds)) }), L({}), L({}), W(1) });
	};
	const auto EndTask = [&](Asm &r_code) {
		r_code.Op(VbcOp::EndTask, { W(none), W(none), W(r_code.K(no)), W(none), W(none) });
	};
	const auto Spawn = [&](Asm &r_body) {
		Asm code(heap, "Spawn", 3, 0);
		code.Op(VbcOp::CallTask, { W(2), W(none), W(code.K(r_body.Function(no))), L({}) });
		code.Op(VbcOp::Return, { W(R(2)) });
		Value task;
		InvokeValue(interpreter, code.Function(no), task);
		return task;
	};
	Value result;
	g_log.clear();

	ObjectCell *event = heap.make<ObjectCell>();
	const Value event_value = Value::from_cell(event);
	const auto Signal = [&](int64_t p_value) {
		Asm code(heap, "Signaler", 4, 0);
		code.Op(VbcOp::CallWithSelf, { W(2), W(code.K(signal)), W(code.K(event_value)), L({ code.K(Int(heap, p_value)) }), L({}), L({}), W(0) });
		Log(code, 3, 99);
		code.Op(VbcOp::Return, { W(code.K(no)) });
		InvokeValue(interpreter, code.Function(no), result);
	};
	Asm reawaiter(heap, "Reawaiter", 4, 0);
	reawaiter.Op(VbcOp::ResetNonTrailed, { W(2), W(0) });
	reawaiter.Op(VbcOp::CallWithSelf, { W(2), W(reawaiter.K(await)), W(reawaiter.K(event_value)), L({}), L({}), L({}), W(1) });
	reawaiter.Op(VbcOp::Call, { W(3), W(reawaiter.K(log)), L({ R(2) }), L({}), L({}), W(0) });
	reawaiter.Op(VbcOp::ResetNonTrailed, { W(3), W(0) });
	reawaiter.Op(VbcOp::Jump, { W(0) });
	Spawn(reawaiter);
	Spawn(reawaiter);
	Signal(1);
	r_cases.check("natives §7.2 D2: Signal resumes each awaiter once, oldest first, each to its next stop; one that awaits again waits for the next",
			Logged({ 1, 1, 99 }));
	Signal(2);
	r_cases.check("natives §7.2: the next Signal finds the awaiters the last one's resumptions added", Logged({ 2, 2, 99 }));

	// Sleep: Log 1; Sleep(0); Log 2; Sleep(5); Log 3.
	g_now = 100.0;
	Asm sleeper(heap, "Sleeper", 6, 0);
	Log(sleeper, 5, 1);
	Sleep(sleeper, 2, 0.0);
	Log(sleeper, 5, 2);
	Sleep(sleeper, 3, 5.0);
	Log(sleeper, 5, 3);
	EndTask(sleeper);
	Spawn(sleeper);
	vh_tick_stats stats = {};
	r_cases.check("godot-natives §10: Sleep(0.0) suspends", Logged({ 1 }) && interpreter.sleepers.size() == 1);
	runtime.tick(stats);
	r_cases.check("godot-natives §10: a due sleeper wakes at the tick, and one that sleeps again is not woken by the same tick",
			Logged({ 2 }) && stats.JobsRun == 1 && stats.Sleeping == 1);
	g_now = 104.5;
	stats = {};
	runtime.tick(stats);
	r_cases.check("godot-natives §10: a sleeper whose deadline has not passed stays asleep", Logged({}) && stats.JobsRun == 0 && stats.Sleeping == 1);
	g_now = 1000.0;
	stats = {};
	runtime.tick(stats);
	r_cases.check("godot-natives §10: a long gap wakes it once", Logged({ 3 }) && stats.JobsRun == 1 && stats.Sleeping == 0);

	Asm negative(heap, "Negative", 4, 0);
	Log(negative, 3, 7);
	Sleep(negative, 2, -1.0);
	Log(negative, 3, 8);
	negative.Op(VbcOp::Return, { W(negative.K(no)) });
	r_cases.check("spec/tasks.md §11.1 S2: Sleep of a negative duration does not suspend",
			Invoked(interpreter, negative.Function(no), {}) == "false" && Logged({ 7, 8 }) && interpreter.sleepers.empty());

	g_now = 0.0;
	const auto Napper = [&](double p_seconds, int64_t p_id) {
		Asm code(heap, "Napper", 4, 0);
		Sleep(code, 2, p_seconds);
		Log(code, 3, p_id);
		EndTask(code);
		Spawn(code);
	};
	Napper(3.0, 31);
	Napper(1.0, 11);
	g_now = 10.0;
	stats = {};
	runtime.tick(stats);
	r_cases.check("godot-natives §10: sleepers due in one tick wake earliest deadline first", Logged({ 11, 31 }) && stats.JobsRun == 2);

	// Sleep(1); on cancellation the landing pad logs 41 and ends the task.
	Asm cancelled(heap, "CancelledSleeper", 4, 0);
	Sleep(cancelled, 2, 1.0);
	EndTask(cancelled);
	Log(cancelled, 3, 41);
	cancelled.Op(VbcOp::Jump, { W(1) });
	cancelled.procedure->unwind_edges.push_back(UnwindEdge{ 0, 0, 2 });
	Asm canceler(heap, "SleepCanceler", 5, 0);
	canceler.Op(VbcOp::CallTask, { W(2), W(none), W(canceler.K(cancelled.Function(no))), L({}) });
	canceler.Op(VbcOp::CallWithSelf, { W(3), W(canceler.K(cancel)), W(R(2)), L({}), L({}), L({}), W(1) });
	Log(canceler, 4, 9);
	canceler.Op(VbcOp::Return, { W(canceler.K(no)) });
	Invoked(interpreter, canceler.Function(no), {});
	r_cases.check("spec/tasks.md §11.1 S1: cancelling a sleeper unwinds it at once; its wake stays counted until due",
			Logged({ 41, 9 }) && interpreter.sleepers.size() == 1);
	g_now = 20.0;
	stats = {};
	runtime.tick(stats);
	r_cases.check("spec/tasks.md §11.1 S1: the cancelled sleeper's wake does nothing", Logged({}) && stats.Sleeping == 0);

	Asm raiser(heap, "SleepThenRaise", 4, 0);
	Sleep(raiser, 2, 0.0);
	raiser.Op(VbcOp::Call, { W(3), W(raiser.K(err)), L({ raiser.K(Str(heap, "woke")) }), L({}), L({}), W(0) });
	EndTask(raiser);
	Spawn(raiser);
	Napper(0.0, 77);
	g_runtime_error.clear();
	stats = {};
	runtime.tick(stats);
	r_cases.check("godot-natives §10: a raise in one sleeper is reported and the others still wake",
			g_runtime_error.find("User Message: 'woke'") != std::string::npos && Logged({ 77 }) && stats.JobsRun == 2);

	// await{Ref > Threshold}; Log Id -- the compiled shape: a failure context over the condition,
	// AwaitSuccess, and on failure EndAwait and a Yield back to the await point.
	const auto Awaiter = [&](bool p_element, int64_t p_threshold, int64_t p_id) {
		Asm code(heap, "Awaiter", 6, 1);
		code.Op(VbcOp::BeginAwait, {});
		code.Op(VbcOp::BeginFailureContext, { W(9), W(0) });
		code.Op(VbcOp::ResetNonTrailed, { W(3), W(0) });
		if (p_element) {
			code.Op(VbcOp::Call, { W(3), W(R(2)), L({ code.K(Int(heap, 0)) }), L({}), L({}), W(0) });
		} else {
			code.Op(VbcOp::RefGet, { W(3), W(R(2)) });
		}
		code.Op(VbcOp::ResetNonTrailed, { W(4), W(0) });
		code.Op(VbcOp::Gt, { W(4), W(R(3)), W(code.K(Int(heap, p_threshold))) });
		code.Op(VbcOp::AwaitSuccess, {});
		code.Op(VbcOp::EndFailureContext, { W(11), W(0) });
		code.Op(VbcOp::Jump, { W(11) });
		code.Op(VbcOp::EndAwait, {});
		code.Op(VbcOp::Yield, { W(1) });
		Log(code, 5, p_id);
		EndTask(code);
		return code.Function(no);
	};
	Asm vars(heap, "AwaitVars", 5, 0);
	vars.Op(VbcOp::NewRef, { W(2), W(none) });
	vars.Op(VbcOp::RefSet, { W(R(2)), W(vars.K(Int(heap, 0))) });
	vars.Op(VbcOp::CallTask, { W(3), W(none), W(vars.K(Awaiter(false, 2, 71))), L({ R(2) }) });
	vars.Op(VbcOp::CallTask, { W(4), W(none), W(vars.K(Awaiter(false, 10, 72))), L({ R(2) }) });
	vars.Op(VbcOp::Return, { W(R(2)) });
	Value variable;
	InvokeValue(interpreter, vars.Function(no), variable);
	Asm write(heap, "Write", 5, 2);
	write.Op(VbcOp::RefSet, { W(R(2)), W(R(3)) });
	Log(write, 4, 99);
	write.Op(VbcOp::Return, { W(write.K(no)) });
	const Value write_fn = write.Function(no);
	r_cases.check("tasks §5.7 F1: an await's first evaluation always fails, so it waits", Logged({}));
	Invoked(interpreter, write_fn, { variable, Int(heap, 1) });
	r_cases.check("tasks §5.7 F2: a write re-evaluates each registered await; still false, it waits again", Logged({ 99 }));
	Invoked(interpreter, write_fn, { variable, Int(heap, 3) });
	r_cases.check("tasks §5.7 F3: a write that makes the condition true resumes the task inside the write", Logged({ 71, 99 }));
	Asm batch(heap, "Batch", 4, 1);
	batch.Op(VbcOp::BeginBatch, {});
	batch.Op(VbcOp::RefSet, { W(R(2)), W(batch.K(Int(heap, 11))) });
	Log(batch, 3, 50);
	batch.Op(VbcOp::RefSet, { W(R(2)), W(batch.K(Int(heap, 12))) });
	batch.Op(VbcOp::EndBatch, {});
	Log(batch, 3, 99);
	batch.Op(VbcOp::Return, { W(batch.K(no)) });
	Invoked(interpreter, batch.Function(no), { variable });
	r_cases.check("tasks §5.7 F5: writes inside a batch resume their awaiters once, at the outermost EndBatch", Logged({ 50, 72, 99 }));

	Asm elements(heap, "AwaitElement", 4, 0);
	elements.Op(VbcOp::NewMutableArray, { W(2), L({ elements.K(Int(heap, 0)), elements.K(Int(heap, 0)) }) });
	elements.Op(VbcOp::CallTask, { W(3), W(none), W(elements.K(Awaiter(true, 2, 81))), L({ R(2) }) });
	elements.Op(VbcOp::Return, { W(R(2)) });
	Value array;
	InvokeValue(interpreter, elements.Function(no), array);
	Asm set_element(heap, "SetElement", 6, 3);
	set_element.Op(VbcOp::CallSet, { W(R(2)), W(R(3)), W(R(4)) });
	Log(set_element, 5, 99);
	set_element.Op(VbcOp::Return, { W(set_element.K(no)) });
	Invoked(interpreter, set_element.Function(no), { array, Int(heap, 1), Int(heap, 9) });
	r_cases.check("ops §3.1 A02: a write to another element does not wake an element's awaiter", Logged({ 99 }));
	Invoked(interpreter, set_element.Function(no), { array, Int(heap, 0), Int(heap, 3) });
	Value element;
	r_cases.check("ops §3.1 A03: an element read under an await registers the element, and CallSet on it wakes the awaiter",
			Logged({ 81, 99 }) && array_index(array, Int(heap, 0), element) == Outcome::Ok && Show(element) == "3");
	Value frozen;
	r_cases.check("ops §3.1: freezing reads through the hidden variable an element now stands in",
			freeze(heap, array, frozen) == Outcome::Ok && Show(frozen) == "[3,9]" && is_cell_kind(cell_as<ArrayCell>(array)->get(0), CellKind::Ref));

	// Live writes: tasks parked on a second event, whose cancellation logs 60 + id.
	ObjectCell *parking = heap.make<ObjectCell>();
	const auto Parked = [&](int64_t p_id) {
		Asm code(heap, "Parked", 4, 0);
		code.Op(VbcOp::CallWithSelf, { W(2), W(code.K(await)), W(code.K(Value::from_cell(parking))), L({}), L({}), L({}), W(1) });
		EndTask(code);
		Log(code, 3, 60 + p_id);
		code.Op(VbcOp::Jump, { W(1) });
		code.procedure->unwind_edges.push_back(UnwindEdge{ 0, 0, 2 });
		return cell_as<TaskCell>(Spawn(code));
	};
	TaskCell *first = Parked(1);
	TaskCell *second = Parked(2);
	Asm live(heap, "LiveWrite", 6, 3);
	live.Op(VbcOp::RefSetLive, { W(R(2)), W(R(3)), W(R(4)) });
	Log(live, 5, 99);
	live.Op(VbcOp::Return, { W(live.K(no)) });
	const Value live_fn = live.Function(no);
	Invoked(interpreter, live_fn, { variable, Int(heap, 1), Value::from_cell(first) });
	Invoked(interpreter, live_fn, { variable, Int(heap, 2), Value::from_cell(first) });
	r_cases.check("ops §3.2: a live write makes its Task the variable's live task; the same task again cancels nothing",
			Logged({ 99, 99 }) && cell_as<RefCell>(variable)->live_task.same(Value::from_cell(first)) && first->phase == TaskCell::Phase::Active);
	Invoked(interpreter, live_fn, { variable, Int(heap, 3), Value::from_cell(second) });
	r_cases.check("ops §3.2: a live write by another task cancels the variable's live task first",
			Logged({ 61, 99 }) && first->phase == TaskCell::Phase::Canceled && cell_as<RefCell>(variable)->live_task.same(Value::from_cell(second)));
	Invoked(interpreter, write_fn, { variable, Int(heap, 4) });
	r_cases.check("ops §3.2 L03: an ordinary write ends the binding by cancelling the live task",
			Logged({ 62, 99 }) && second->phase == TaskCell::Phase::Canceled && cell_as<RefCell>(variable)->live_task.is_uninitialized() &&
					Show(cell_as<RefCell>(variable)->content) == "4");
	g_now = 0.0;
}

std::vector<int64_t> g_released_refs;

void CaptureReleaseRef(void *, int64_t p_ref) {
	g_released_refs.push_back(p_ref);
}

Outcome ChurnNative(NativeCall &r_call) {
	for (int64_t index = 0; index < 200; ++index) {
		make_array(r_call.heap, { Int(r_call.heap, index), Str(r_call.heap, "churn") }, false);
	}
	r_call.result = r_call.heap.false_value();
	return Outcome::Ok;
}

vh_handle MintHandle(void *, const char *, int32_t) {
	return 77;
}

const char *MirroredName(std::string_view) {
	return "Node2D";
}

// What the collector cases share: a Runtime whose procedures are pinned the way a loaded program's
// are, so the only unrooted cells are the ones a case means to drop. `inst` is an immutable
// [event, counter variable] pair, the shape an instance's fields give a method.
struct GcRig {
	Runtime runtime;
	Heap &heap = runtime.heap;
	Interpreter &interpreter = runtime.interpreter;
	Value no = heap.false_value();
	Value log;
	Value churn;
	Value await;
	Value signal;
	Value sleep;
	Value listener;
	Value napper;
	Value fire;
	Value read;

	GcRig() {
		using vbc::VbcOp;
		const uint32_t none = kAbsentOperand;
		interpreter.clock = &FakeClock;
		log = Pin(NativeFunction(heap, "Log", 1, &LogNative));
		churn = Pin(NativeFunction(heap, "Churn", 0, &ChurnNative));
		await = Pin(NativeProcedure(heap, "(/Verse.org/Verse/event/Await:)Native", 0));
		signal = Pin(NativeProcedure(heap, "(/Verse.org/Verse/event/(/Verse.org/Verse/signalable:)Signal(:payload):)Native", 1));
		sleep = Pin(NativeFunction(heap, "Sleep", 1, native_implementation("(/Godot.org/Godot/Sleep(:float):)Native")));

		// Listener(Inst): loop { V := Inst[0].Await(); Inst[1] += V; Log(Inst[1]) }.
		Asm body(heap, "Listener", 9, 1);
		for (uint32_t reg = 3; reg <= 8; ++reg) {
			body.Op(VbcOp::ResetNonTrailed, { W(reg), W(0) });
		}
		body.Op(VbcOp::Call, { W(3), W(R(2)), L({ body.K(Int(heap, 0)) }), L({}), L({}), W(0) });
		body.Op(VbcOp::CallWithSelf, { W(4), W(body.K(await)), W(R(3)), L({}), L({}), L({}), W(1) });
		body.Op(VbcOp::Call, { W(5), W(R(2)), L({ body.K(Int(heap, 1)) }), L({}), L({}), W(0) });
		body.Op(VbcOp::RefGet, { W(6), W(R(5)) });
		body.Op(VbcOp::Add, { W(7), W(R(6)), W(R(4)) });
		body.Op(VbcOp::RefSet, { W(R(5)), W(R(7)) });
		body.Op(VbcOp::Call, { W(8), W(body.K(log)), L({ R(7) }), L({}), L({}), W(0) });
		body.Op(VbcOp::Jump, { W(0) });
		listener = Starter(body.Function(no));

		// Napper(Inst): Sleep(1.0); Inst[1] += 100; Log(Inst[1]).
		Asm nap(heap, "Napper", 8, 1);
		nap.Op(VbcOp::Call, { W(3), W(nap.K(sleep)), L({ nap.K(F(1.0)) }), L({}), L({}), W(1) });
		nap.Op(VbcOp::Call, { W(4), W(R(2)), L({ nap.K(Int(heap, 1)) }), L({}), L({}), W(0) });
		nap.Op(VbcOp::RefGet, { W(5), W(R(4)) });
		nap.Op(VbcOp::Add, { W(6), W(R(5)), W(nap.K(Int(heap, 100))) });
		nap.Op(VbcOp::RefSet, { W(R(4)), W(R(6)) });
		nap.Op(VbcOp::Call, { W(7), W(nap.K(log)), L({ R(6) }), L({}), L({}), W(0) });
		nap.Op(VbcOp::EndTask, { W(none), W(none), W(nap.K(no)), W(none), W(none) });
		napper = Starter(nap.Function(no));

		// Fire(Inst, V): Churn(); Inst[0].Signal(V).
		Asm fires(heap, "Fire", 7, 2);
		fires.Op(VbcOp::Call, { W(6), W(fires.K(churn)), L({}), L({}), L({}), W(0) });
		fires.Op(VbcOp::Call, { W(4), W(R(2)), L({ fires.K(Int(heap, 0)) }), L({}), L({}), W(0) });
		fires.Op(VbcOp::CallWithSelf, { W(5), W(fires.K(signal)), W(R(4)), L({ R(3) }), L({}), L({}), W(0) });
		fires.Op(VbcOp::Return, { W(fires.K(no)) });
		fire = Pin(fires.Function(no));

		// Read(Inst): Log(Inst[1]).
		Asm reads(heap, "Read", 6, 1);
		reads.Op(VbcOp::Call, { W(3), W(R(2)), L({ reads.K(Int(heap, 1)) }), L({}), L({}), W(0) });
		reads.Op(VbcOp::RefGet, { W(4), W(R(3)) });
		reads.Op(VbcOp::Call, { W(5), W(reads.K(log)), L({ R(4) }), L({}), L({}), W(0) });
		reads.Op(VbcOp::Return, { W(reads.K(no)) });
		read = Pin(reads.Function(no));
	}

	Value Pin(Value p_value) {
		heap.add_permanent_root(p_value);
		return p_value;
	}

	// Spawn(Inst): answers the task running p_body(Inst).
	Value Starter(Value p_body) {
		using vbc::VbcOp;
		Asm code(heap, "Spawn", 4, 1);
		code.Op(VbcOp::CallTask, { W(3), W(kAbsentOperand), W(code.K(p_body)), L({ R(2) }) });
		code.Op(VbcOp::Return, { W(R(3)) });
		return Pin(code.Function(no));
	}

	Value Inst(ObjectCell *p_event, RefCell *p_counter) {
		return Arr(heap, { Value::from_cell(p_event), Value::from_cell(p_counter) });
	}

	// A call into p_scope, as vh_instance_call makes one; p_scope null is no scope at all.
	Value Enter(Value p_function, std::vector<Value> p_arguments, Value *p_scope = nullptr) {
		if (p_scope != nullptr) {
			runtime.activate_scope(*p_scope);
		}
		interpreter.begin_entry();
		Value result;
		const Outcome outcome = interpreter.invoke(p_function, Value::uninitialized(), p_arguments, {}, result);
		interpreter.end_entry(outcome == Outcome::Ok || outcome == Outcome::Yield);
		interpreter.active_scope = nullptr;
		return outcome == Outcome::Ok ? result : Value();
	}
};

// Design §7.3: each root kind keeps what it holds, and nothing else is kept.
void CollectorCases(Cases &r_cases) {
	using vbc::VbcOp;
	g_log.clear();
	g_now = 0.0;
	{
		GcRig rig;
		Value scope;
		rig.heap.add_handle_root(&scope);
		ObjectCell *event = rig.heap.make<ObjectCell>();
		RefCell *counter = rig.heap.make<RefCell>(Int(rig.heap, 0));
		const Value task = rig.Enter(rig.listener, { rig.Inst(event, counter) }, &scope);
		const bool suspended = is_cell_kind(task, CellKind::Task) && cell_as<TaskCell>(task)->resume_frame != nullptr;
		const FrameCell *frame = suspended ? cell_as<TaskCell>(task)->resume_frame : nullptr;
		rig.Enter(rig.churn, {});
		const size_t freed = rig.heap.collect();
		const bool kept = rig.heap.owns(task.as_cell()) && rig.heap.owns(frame) && rig.heap.owns(event) && rig.heap.owns(counter);
		rig.Enter(rig.fire, { rig.Inst(event, counter), Int(rig.heap, 5) });
		r_cases.check("gc: a suspended task held only by its content scope's group keeps its frame and what the frame holds; the rest is freed",
				suspended && freed >= 400 && kept && Logged({ 5 }));

		ObjectCell *lost_event = rig.heap.make<ObjectCell>();
		const Value lost = rig.Enter(rig.listener, { rig.Inst(lost_event, rig.heap.make<RefCell>(Int(rig.heap, 0))) });
		const bool lost_is_task = is_cell_kind(lost, CellKind::Task);
		rig.heap.collect();
		r_cases.check("gc: a suspended task in no group, awaiting an event nothing reaches, is collected with the event",
				lost_is_task && !rig.heap.owns(lost.as_cell()) && !rig.heap.owns(lost_event));
	}
	{
		GcRig rig;
		RefCell *counter = rig.heap.make<RefCell>(Int(rig.heap, 7));
		const Value task = rig.Enter(rig.napper, { rig.Inst(rig.heap.make<ObjectCell>(), counter) });
		rig.heap.collect();
		const bool kept = is_cell_kind(task, CellKind::Task) && rig.heap.owns(task.as_cell()) && rig.heap.owns(counter) && rig.interpreter.sleepers.size() == 1;
		g_now = 2.0;
		vh_tick_stats stats = {};
		rig.runtime.tick(stats);
		r_cases.check("gc: a sleeper, reached only from the sleeper list, survives and wakes with its frame", kept && stats.JobsRun == 1 && Logged({ 107 }));
		g_now = 0.0;
	}
	{
		GcRig rig;
		const uint32_t none = kAbsentOperand;
		// await{Ref > 2}; Log 71 -- the compiled shape AwaitEventSleepCases describes.
		Asm awaiter(rig.heap, "Awaiter", 6, 1);
		awaiter.Op(VbcOp::BeginAwait, {});
		awaiter.Op(VbcOp::BeginFailureContext, { W(9), W(0) });
		awaiter.Op(VbcOp::ResetNonTrailed, { W(3), W(0) });
		awaiter.Op(VbcOp::RefGet, { W(3), W(R(2)) });
		awaiter.Op(VbcOp::ResetNonTrailed, { W(4), W(0) });
		awaiter.Op(VbcOp::Gt, { W(4), W(R(3)), W(awaiter.K(Int(rig.heap, 2))) });
		awaiter.Op(VbcOp::AwaitSuccess, {});
		awaiter.Op(VbcOp::EndFailureContext, { W(11), W(0) });
		awaiter.Op(VbcOp::Jump, { W(11) });
		awaiter.Op(VbcOp::EndAwait, {});
		awaiter.Op(VbcOp::Yield, { W(1) });
		awaiter.Op(VbcOp::Call, { W(5), W(awaiter.K(rig.log)), L({ awaiter.K(Int(rig.heap, 71)) }), L({}), L({}), W(0) });
		awaiter.Op(VbcOp::EndTask, { W(none), W(none), W(awaiter.K(rig.no)), W(none), W(none) });
		Asm vars(rig.heap, "AwaitVars", 4, 0);
		vars.Op(VbcOp::NewRef, { W(2), W(none) });
		vars.Op(VbcOp::RefSet, { W(R(2)), W(vars.K(Int(rig.heap, 0))) });
		vars.Op(VbcOp::CallTask, { W(3), W(none), W(vars.K(awaiter.Function(rig.no))), L({ R(2) }) });
		vars.Op(VbcOp::Return, { W(R(2)) });
		Asm write(rig.heap, "Write", 4, 2);
		write.Op(VbcOp::RefSet, { W(R(2)), W(R(3)) });
		write.Op(VbcOp::Return, { W(write.K(rig.no)) });
		const Value write_fn = rig.Pin(write.Function(rig.no));
		Value variable = rig.Enter(rig.Pin(vars.Function(rig.no)), {});
		rig.heap.add_handle_root(&variable);
		const bool registered = is_cell_kind(variable, CellKind::Ref) && cell_as<RefCell>(variable)->awaiting.size() == 1;
		const TaskCell *task = registered ? cell_as<RefCell>(variable)->awaiting[0].task : nullptr;
		const FrameCell *frame = registered ? cell_as<RefCell>(variable)->awaiting[0].frame : nullptr;
		rig.heap.collect();
		const bool kept = registered && rig.heap.owns(task) && rig.heap.owns(frame);
		rig.Enter(write_fn, { variable, Int(rig.heap, 3) });
		r_cases.check("gc: a task reached only through an await registration survives, and the write it waits for resumes it", kept && Logged({ 71 }));
		rig.heap.remove_handle_root(&variable);
	}
	{
		GcRig rig;
		Value object;
		Value scope;
		rig.heap.add_handle_root(&object);
		rig.heap.add_handle_root(&scope);
		ObjectCell *instance = rig.heap.make<ObjectCell>();
		object = Value::from_cell(instance);
		ObjectCell *event = rig.heap.make<ObjectCell>();
		RefCell *counter = rig.heap.make<RefCell>(Int(rig.heap, 0));
		const Value fields = rig.Inst(event, counter);
		instance->field_values.push_back(fields);
		const Value task = rig.Enter(rig.listener, { fields }, &scope);
		rig.heap.collect();
		const bool held = rig.heap.owns(instance) && rig.heap.owns(task.as_cell()) && rig.heap.owns(fields.as_cell());
		rig.interpreter.terminate_scope(cell_as<ContentScopeCell>(scope));
		rig.heap.remove_handle_root(&object);
		rig.heap.remove_handle_root(&scope);
		rig.heap.collect();
		r_cases.check("gc: a released instance -- both handle roots dropped, its scope terminated -- is collected with its tasks and fields",
				held && !rig.heap.owns(instance) && !rig.heap.owns(task.as_cell()) && !rig.heap.owns(fields.as_cell()) && !rig.heap.owns(event) &&
						!rig.heap.owns(counter));
	}
	{
		GcRig rig;
		rig.interpreter.godot.ReleaseRef = &CaptureReleaseRef;
		g_released_refs.clear();
		Value wrapper = Value::from_cell(rig.heap.make<ObjectCell>());
		RootScope root(rig.heap, &wrapper);
		rig.interpreter.adopt_ref(wrapper.as_cell(), 42);
		rig.interpreter.adopt_ref(rig.heap.make<ObjectCell>(), 0);
		rig.heap.collect();
		const bool held = g_released_refs.empty() && rig.interpreter.adopted_refs.size() == 1;
		wrapper = Value();
		rig.heap.collect();
		rig.heap.collect();
		r_cases.check("godot-natives §3.2: a collected godot_ref hands its id to ReleaseRef exactly once; a live one and Ref 0 release nothing",
				held && g_released_refs == std::vector<int64_t>{ 42 } && rig.interpreter.adopted_refs.empty());
	}
	{
		GcRig rig;
		ClassCell *node2d = MakeClass(rig.heap, ClassKind::Class, {}, {});
		rig.Pin(Value::from_cell(node2d));
		rig.runtime.program.classes.push_back(ClassIndexEntry{ ClassOrigin::Mirrored, rig.heap.intern("node2d"), node2d });
		rig.runtime.program.classes_by_cell.emplace(node2d, 0);
		rig.interpreter.mirrored_godot_name = &MirroredName;
		rig.interpreter.godot.InstantiateClass = &MintHandle;
		rig.interpreter.godot.ReleaseObject = &CaptureRelease;
		const NativeFn adopt_or_mint = native_implementation("(/Godot.org/Godot/(/Godot.org/Godot:)VhAdoptOrMint(:(/Godot.org/Godot:)vh_object):)Native");
		const auto Peer = [&](ObjectCell *p_object) {
			const Value self = Value::from_cell(p_object);
			NativeCall call(rig.heap);
			call.interpreter = &rig.interpreter;
			call.arguments = &self;
			call.argument_count = 1;
			rig.interpreter.begin_entry();
			const Outcome outcome = adopt_or_mint(call);
			rig.interpreter.end_entry(outcome == Outcome::Ok);
			return outcome == Outcome::Ok ? int_value(call.result).to_int64() : -1;
		};
		ObjectCell *minted = rig.heap.make<ObjectCell>();
		minted->object_class = node2d;
		ObjectCell *adopted = rig.heap.make<ObjectCell>();
		adopted->object_class = node2d;
		rig.interpreter.adopting_object = adopted;
		rig.interpreter.adopting_handle = 5;
		const int64_t adopted_handle = Peer(adopted);
		const int64_t minted_handle = Peer(minted);
		Value held = Value::from_cell(minted);
		RootScope root(rig.heap, &held);
		g_released.clear();
		rig.heap.collect();
		const bool live = g_released.empty() && rig.interpreter.minted_peers.count(77) == 1 && !rig.heap.owns(adopted);
		r_cases.check("godot-natives §4.4: an adopted peer's object is collected and releases nothing -- the scene owns it", adopted_handle == 5 && live);
		held = Value();
		rig.heap.collect();
		r_cases.check("godot-natives §4.4: a collected minting object removes its row and calls ReleaseObject once, not as a discard",
				minted_handle == 77 && g_released == std::vector<std::pair<int64_t, int>>{ { 77, 0 } } && rig.interpreter.minted_peers.empty());
	}
	{
		GcRig rig;
		rig.interpreter.begin_entry();
		const bool refused = !rig.runtime.collect_garbage();
		rig.interpreter.end_entry(true);
		r_cases.check("design §7.3: no collection while an entry is running; one outside it collects", refused && rig.runtime.collect_garbage());
		rig.heap.min_collect_trigger = 100;
		const size_t before = rig.heap.collection_count();
		vh_tick_stats stats = {};
		rig.runtime.tick(stats);
		const bool idle = rig.heap.collection_count() == before;
		rig.Enter(rig.churn, {});
		rig.Enter(rig.churn, {});
		rig.runtime.tick(stats);
		r_cases.check("design §7.3: vh_tick collects once enough has been allocated since the last collection, and not before",
				idle && rig.heap.collection_count() == before + 1);
	}
	{
		GcRig rig;
		// Wrapper() = CanCallerAccessEpicInternal_Impl(); Caller() = Wrapper(); Outer() = Caller().
		const Value impl = rig.Pin(NativeFunction(rig.heap, "(/Verse.org/Verse:)CanCallerAccessEpicInternal_Impl", 0,
				native_implementation("(/Verse.org/Verse/(/Verse.org/Verse:)CanCallerAccessEpicInternal_Impl:)Native")));
		const auto Calls = [&](const char *p_name, Value p_callee, uint32_t p_flags) {
			Asm code(rig.heap, p_name, 3, 0);
			code.procedure->flags = p_flags;
			code.Op(VbcOp::Call, { W(2), W(code.K(p_callee)), L({}), L({}), L({}), W(0) });
			code.Op(VbcOp::Return, { W(R(2)) });
			return rig.Pin(code.Function(rig.no));
		};
		const Value wrapper = Calls("CanCallerAccessEpicInternal", impl, 1);
		const Value caller = Calls("Caller", wrapper, 1);
		const auto Answer = [&](Value p_function) {
			const Value result = rig.Enter(p_function, {});
			return is_cell_kind(result, CellKind::True) ? "true" : is_cell_kind(result, CellKind::False) ? "false" : "?";
		};
		r_cases.check("natives §5.9: the frame three up from the native decides -- the entry's native code may, a procedure by its flag",
				std::string(Answer(caller)) == "true" && std::string(Answer(Calls("Plain", caller, 0))) == "false" &&
						std::string(Answer(Calls("Internal", caller, 1))) == "true");
		r_cases.check("natives §5.9: with no frame three up, the answer is false", std::string(Answer(wrapper)) == "false");
	}
	{
		GcRig rig;
		const NativeFn epoch = native_implementation("(/Verse.org/Verse/(/Verse.org/Verse:)GetSecondsSinceEpoch:)Native");
		const NativeFn random_int = native_implementation("(/Verse.org/Random/(/Verse.org/Random:)GetRandomInt(:int,:int):)Native");
		NativeCall first(rig.heap);
		first.interpreter = &rig.interpreter;
		NativeCall second(rig.heap);
		second.interpreter = &rig.interpreter;
		NativeCall bare(rig.heap);
		r_cases.check("natives §5.6: GetSecondsSinceEpoch answers the runtime's one sample, every call",
				epoch(first) == Outcome::Ok && epoch(second) == Outcome::Ok && first.result.same(second.result) &&
						first.result.as_float() == rig.interpreter.epoch_seconds && epoch(bare) == Outcome::Invalid);
		std::mt19937_64 expected = rig.interpreter.random;
		const Value bounds[] = { Int(rig.heap, 0), Int(rig.heap, 1023) };
		NativeCall draw(rig.heap);
		draw.interpreter = &rig.interpreter;
		draw.arguments = bounds;
		draw.argument_count = 2;
		r_cases.check("natives §6: GetRandomInt draws from the runtime's own generator",
				random_int(draw) == Outcome::Ok && int_value(draw.result).to_int64() == int64_t(expected() & 1023));
	}
	{
		GcRig rig;
		ObjectCell *event = rig.heap.make<ObjectCell>();
		RefCell *counter = rig.heap.make<RefCell>(Int(rig.heap, 0));
		const Value inst = rig.Pin(rig.Inst(event, counter));
		rig.heap.tenure();
		g_log.clear();
		const Value task = rig.Enter(rig.listener, { inst });
		rig.Enter(rig.churn, {});
		const size_t freed = rig.heap.collect();
		const bool kept = is_cell_kind(task, CellKind::Task) && rig.heap.owns(task.as_cell()) && !task.as_cell()->tenured;
		rig.Enter(rig.fire, { inst, Int(rig.heap, 4) });
		r_cases.check("gc tenure: a task reached only through a tenured event's awaiters survives, and the event's Signal resumes it",
				freed >= 400 && kept && Logged({ 4 }));
	}
	{
		GcRig rig;
		rig.interpreter.godot.ReleaseRef = &CaptureReleaseRef;
		g_released_refs.clear();
		ObjectCell *wrapper = rig.heap.make<ObjectCell>();
		rig.Pin(Value::from_cell(wrapper));
		rig.heap.tenure();
		rig.interpreter.adopt_ref(wrapper, 8);
		rig.heap.collect();
		r_cases.check("gc tenure: a weak table's sweep reads a tenured cell as live", g_released_refs.empty() && rig.interpreter.adopted_refs.size() == 1);
	}
	{
		auto rig = std::make_unique<GcRig>();
		rig->interpreter.godot.ReleaseRef = &CaptureReleaseRef;
		rig->interpreter.godot.ReleaseObject = &CaptureRelease;
		g_released_refs.clear();
		g_released.clear();
		Value peer = Value::from_cell(rig->heap.make<ObjectCell>());
		rig->heap.add_handle_root(&peer);
		rig->interpreter.minted_peers[77] = peer.as_cell();
		rig->interpreter.adopt_ref(peer.as_cell(), 9);
		rig.reset();
		r_cases.check("vh_shutdown: tearing the runtime down releases no peer and no ref the consumer has already dropped",
				g_released.empty() && g_released_refs.empty());
	}
}

// The conformance workload's shape -- a listener task, a sleeper, signals, churn -- run with a full
// collection after every entry and without one. The two logs must agree.
// With p_tenure the instance -- its event and its variable -- is tenured with the procedures, so
// everything the run hangs off them is reached only through the remembered set.
std::vector<int64_t> GcWorkload(bool p_collect, bool p_tenure = false) {
	GcRig rig;
	rig.heap.min_collect_trigger = p_collect ? 1 : SIZE_MAX;
	g_log.clear();
	g_now = 0.0;
	Value scope;
	Value inst;
	rig.heap.add_handle_root(&scope);
	rig.heap.add_handle_root(&inst);
	inst = rig.Inst(rig.heap.make<ObjectCell>(), rig.heap.make<RefCell>(Int(rig.heap, 0)));
	if (p_tenure) {
		rig.heap.tenure();
	}
	const auto Entry = [&](Value p_function, std::vector<Value> p_arguments) {
		rig.Enter(p_function, p_arguments, &scope);
		if (p_collect) {
			rig.runtime.collect_garbage();
		}
	};
	Entry(rig.listener, { inst });
	Entry(rig.napper, { inst });
	for (int64_t value = 1; value <= 5; ++value) {
		Entry(rig.fire, { inst, Int(rig.heap, value) });
		Entry(rig.churn, {});
	}
	g_now = 2.0;
	vh_tick_stats stats = {};
	rig.runtime.tick(stats);
	Entry(rig.fire, { inst, Int(rig.heap, 10) });
	Entry(rig.read, { inst });
	g_now = 0.0;
	std::vector<int64_t> logged;
	logged.swap(g_log);
	if (p_collect) {
		logged.push_back(int64_t(rig.heap.collection_count() >= 14 ? 1 : 0));
	}
	return logged;
}

void CollectorStressCases(Cases &r_cases) {
	const std::vector<int64_t> plain = GcWorkload(false);
	std::vector<int64_t> stressed = GcWorkload(true);
	const bool collected = !stressed.empty() && stressed.back() == 1;
	if (!stressed.empty()) {
		stressed.pop_back();
	}
	r_cases.check("gc stress: a workload collecting after every entry and at every tick logs what it logs with no collection",
			collected && plain == stressed && plain == std::vector<int64_t>{ 1, 3, 6, 10, 15, 115, 125, 125 });
	std::vector<int64_t> tenured = GcWorkload(true, true);
	const bool tenured_collected = !tenured.empty() && tenured.back() == 1;
	if (!tenured.empty()) {
		tenured.pop_back();
	}
	r_cases.check("gc stress: the same workload over a tenured instance logs the same", tenured_collected && tenured == plain);
}

bool RunInterpreterCases() {
	Cases cases;
	CallCases(cases);
	FailureAndEffectCases(cases);
	ConstructionCases(cases);
	UndoLogCases(cases);
	CastCases(cases);
	AccessorCases(cases);
	NativeFieldCases(cases);
	StructConstantCases(cases);
	TaskCases(cases);
	AwaitEventSleepCases(cases);
	CollectorCases(cases);
	CollectorStressCases(cases);
	return cases.all_ok;
}

bool RunLoaderCases() {
	Cases cases;
	JsonCases(cases);
	LoaderCases(cases);
	BootCases(cases);
	return cases.all_ok;
}

// `verse_vm_test --vbc <program.vbc> [procedure substring]`: loads a real cook and prints what it
// holds, and the op names of every procedure whose name contains the substring -- the loader's
// side of tools/vbc_dump.py --proc.
int DumpProgram(const char *p_path, const char *p_procedure) {
	std::vector<uint8_t> bytes;
	if (!vm_default_file_reader(p_path, bytes)) {
		printf("cannot read %s\n", p_path);
		return 1;
	}
	Heap heap;
	Program program;
	std::string error;
	if (!load_program(heap, bytes.data(), bytes.size(), p_path, program, error)) {
		printf("%s\n", error.c_str());
		return 1;
	}
	printf("cells %zu procedures %zu ops %zu natives %zu unbound %zu classes %zu packages %zu\n", program.cell_count, program.procedure_count,
			program.op_count, program.native_count, program.unbound_native_count, program.classes.size(), program.packages.size());
	if (p_procedure == nullptr) {
		return 0;
	}
	for (const PackageCell *package : program.packages) {
		for (const PackageDefinition &definition : package->definitions) {
			if (!is_cell_kind(definition.value, CellKind::Procedure)) {
				continue;
			}
			const ProcedureCell *procedure = cell_as<ProcedureCell>(definition.value);
			if (procedure->name->text.find(p_procedure) == std::string::npos) {
				continue;
			}
			printf("procedure %s: %zu ops\n", procedure->name->text.c_str(), procedure->ops.size());
			for (size_t index = 0; index < procedure->ops.size(); ++index) {
				printf("  %5zu %s\n", index, vbc::kVbcOps[procedure->ops[index].opcode].name);
			}
		}
	}
	return 0;
}

// `verse_vm_test --gc-bench <program.vbc>`: what a collection costs over a real cook, idle and after
// a burst of garbage, with the program untenured and then tenured as Runtime::boot leaves it -- the
// numbers Heap::min_collect_trigger was chosen from.
int CollectorBench(const char *p_path) {
	std::vector<uint8_t> bytes;
	if (!vm_default_file_reader(p_path, bytes)) {
		printf("cannot read %s\n", p_path);
		return 1;
	}
	Heap heap;
	Program program;
	std::string error;
	if (!load_program(heap, bytes.data(), bytes.size(), p_path, program, error)) {
		printf("%s\n", error.c_str());
		return 1;
	}
	const auto Timed = [&]() {
		const double started = monotonic_seconds();
		const size_t freed = heap.collect();
		return std::make_pair(freed, (monotonic_seconds() - started) * 1000.0);
	};
	const auto Bursts = [&](const char *p_label) {
		const auto idle = Timed();
		printf("%s: idle collection: %zu freed, %.2f ms\n", p_label, idle.first, idle.second);
		for (const size_t garbage : { size_t(16384), size_t(65536), size_t(262144) }) {
			for (size_t index = 0; index < garbage; ++index) {
				Str(heap, "garbage");
			}
			const auto burst = Timed();
			printf("%s: after %zu garbage cells: %zu freed, %.2f ms\n", p_label, garbage, burst.first, burst.second);
		}
	};
	const auto first = Timed();
	printf("first collection: %zu freed, %zu live, %.2f ms\n", first.first, heap.live_cell_count(), first.second);
	Bursts("untenured");
	const double started = monotonic_seconds();
	heap.tenure();
	printf("tenure: %zu tenured, %zu remembered, %.2f ms\n", heap.tenured_cell_count(), heap.remembered_cell_count(),
			(monotonic_seconds() - started) * 1000.0);
	Bursts("tenured");
	return 0;
}

} // namespace

int main(int argc, char **argv) {
	if (argc >= 3 && std::strcmp(argv[1], "--vbc") == 0) {
		return DumpProgram(argv[2], argc >= 4 ? argv[3] : nullptr);
	}
	if (argc >= 3 && std::strcmp(argv[1], "--gc-bench") == 0) {
		return CollectorBench(argv[2]);
	}
	// Unbuffered, so a case that never returns is the line after the last one printed.
	setvbuf(stdout, nullptr, _IONBF, 0);
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
	AllOk &= RunLoaderCases();
	AllOk &= RunInterpreterCases();
	return AllOk ? 0 : 1;
}
