#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// A strict RFC 8259 reader, for the class sidecar (spec/sidecar.md) and nothing larger. Strict means
// it refuses what a lenient reader would guess at: a trailing comma, a comment, a leading zero, a
// bare control character or invalid UTF-8 in a string, an unpaired surrogate escape, anything after
// the one top-level value.
//
// Duplicate object keys are kept, in document order, and find() answers the first: a sidecar's
// `types.members` lists a subclass's member before a superclass member of the same name, and the
// first is the one a lookup from the subclass means (spec/sidecar.md, declared types).
namespace vm {

struct JsonValue {
	enum class Kind : uint8_t {
		Null,
		Bool,
		Number,
		String,
		Array,
		Object,
	};

	Kind kind = Kind::Null;
	bool boolean = false;
	double number = 0.0;
	// A string's contents, or a number's lexeme as written, so an integer too wide for a double
	// can still be read exactly.
	std::string text;
	// An array's elements, or an object's values beside `keys`.
	std::vector<JsonValue> items;
	std::vector<std::string> keys;

	bool is_null() const { return kind == Kind::Null; }
	bool is_bool() const { return kind == Kind::Bool; }
	bool is_number() const { return kind == Kind::Number; }
	bool is_string() const { return kind == Kind::String; }
	bool is_array() const { return kind == Kind::Array; }
	bool is_object() const { return kind == Kind::Object; }

	const JsonValue *find(std::string_view p_key) const;
};

// On failure answers false and a sentence naming the byte offset; r_value is then unspecified.
bool json_parse(std::string_view p_text, JsonValue &r_value, std::string &r_error);

// A number that is an integer in int64 range, written without a fraction or an exponent.
bool json_to_int64(const JsonValue &p_value, int64_t &r_result);

} // namespace vm
