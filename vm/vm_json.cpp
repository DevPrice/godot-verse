#include "vm_json.h"

#include <charconv>
#include <clocale>
#include <cmath>
#include <cstdlib>
#include <utility>
#if defined(__APPLE__)
#include <xlocale.h>
#endif

namespace vm {

namespace {

// Deep enough for any sidecar, shallow enough that a hostile file cannot exhaust the native stack.
constexpr int kMaxDepth = 256;

// Android's libc++ deletes the floating-point from_chars and Apple's gates it on iOS 26, so both
// take strtod. It must not see a comma-decimal locale the app set: Bionic has no such locale, and
// elsewhere the C locale is named explicitly. Defining VM_JSON_STRTOD tests this path anywhere.
#if defined(__ANDROID__) || defined(__APPLE__)
#define VM_JSON_STRTOD
#endif

#if !defined(VM_JSON_STRTOD)

// result_out_of_range leaves the value untouched, so which way it fell is read off the lexeme, which
// the parser has already held to JSON's grammar: a magnitude below one underflowed.
bool magnitude_below_one(const std::string &p_lexeme) {
	size_t at = p_lexeme[0] == '-' ? 1 : 0;
	long long integer_digits = 0;
	long long leading_zeros = 0;
	bool nonzero = false;
	bool fraction = false;
	for (; at < p_lexeme.size() && p_lexeme[at] != 'e' && p_lexeme[at] != 'E'; ++at) {
		if (p_lexeme[at] == '.') {
			fraction = true;
			continue;
		}
		integer_digits += fraction ? 0 : 1;
		nonzero = nonzero || p_lexeme[at] != '0';
		leading_zeros += nonzero ? 0 : 1;
	}
	long long exponent = 0;
	if (at < p_lexeme.size()) {
		++at;
		const bool negative = p_lexeme[at] == '-';
		at += p_lexeme[at] == '-' || p_lexeme[at] == '+' ? 1 : 0;
		for (; at < p_lexeme.size() && exponent < 1000000000; ++at) {
			exponent = exponent * 10 + (p_lexeme[at] - '0');
		}
		exponent = negative ? -exponent : exponent;
	}
	return integer_digits - 1 - leading_zeros + exponent < 0;
}

bool parse_double(const std::string &p_lexeme, double &r_value) {
	const char *end = p_lexeme.data() + p_lexeme.size();
	const std::from_chars_result result = std::from_chars(p_lexeme.data(), end, r_value);
	if (result.ec == std::errc::result_out_of_range) {
		r_value = std::copysign(magnitude_below_one(p_lexeme) ? 0.0 : HUGE_VAL, p_lexeme[0] == '-' ? -1.0 : 1.0);
		return true;
	}
	return result.ec == std::errc() && result.ptr == end;
}

#else

double strtod_c(const char *p_text, char **r_end) {
#if defined(__ANDROID__)
	return std::strtod(p_text, r_end);
#elif defined(_WIN32)
	static const _locale_t c_locale = _create_locale(LC_ALL, "C");
	return _strtod_l(p_text, r_end, c_locale);
#else
	static const locale_t c_locale = newlocale(LC_ALL_MASK, "C", locale_t(0));
	return strtod_l(p_text, r_end, c_locale);
#endif
}

// strtod already answers an underflow with a zero and an overflow with HUGE_VAL, but C does not
// promise the zero's sign.
bool parse_double(const std::string &p_lexeme, double &r_value) {
	char *end = nullptr;
	r_value = strtod_c(p_lexeme.c_str(), &end);
	if (r_value == 0.0) {
		r_value = std::copysign(0.0, p_lexeme[0] == '-' ? -1.0 : 1.0);
	}
	return end == p_lexeme.data() + p_lexeme.size();
}

#endif

class Parser {
public:
	Parser(std::string_view p_text, std::string &r_error) :
			text(p_text), error(r_error) {}

	bool parse_document(JsonValue &r_value) {
		skip_whitespace();
		if (!parse_value(r_value, 0)) {
			return false;
		}
		skip_whitespace();
		if (offset != text.size()) {
			return fail("unexpected text after the top-level value");
		}
		return true;
	}

private:
	std::string_view text;
	std::string &error;
	size_t offset = 0;

	bool fail(const char *p_what) {
		error = std::string(p_what) + " at byte " + std::to_string(offset);
		return false;
	}

	bool at_end() const { return offset >= text.size(); }
	char peek() const { return text[offset]; }

	void skip_whitespace() {
		while (!at_end()) {
			const char c = peek();
			if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
				return;
			}
			++offset;
		}
	}

	bool expect_literal(std::string_view p_literal) {
		if (text.substr(offset, p_literal.size()) != p_literal) {
			return fail("invalid literal");
		}
		offset += p_literal.size();
		return true;
	}

	bool parse_value(JsonValue &r_value, int p_depth) {
		if (at_end()) {
			return fail("unexpected end of input");
		}
		switch (peek()) {
			case '{':
				return parse_object(r_value, p_depth);
			case '[':
				return parse_array(r_value, p_depth);
			case '"':
				r_value.kind = JsonValue::Kind::String;
				return parse_string(r_value.text);
			case 't':
				r_value.kind = JsonValue::Kind::Bool;
				r_value.boolean = true;
				return expect_literal("true");
			case 'f':
				r_value.kind = JsonValue::Kind::Bool;
				r_value.boolean = false;
				return expect_literal("false");
			case 'n':
				r_value.kind = JsonValue::Kind::Null;
				return expect_literal("null");
			default:
				return parse_number(r_value);
		}
	}

	bool parse_object(JsonValue &r_value, int p_depth) {
		if (p_depth >= kMaxDepth) {
			return fail("nesting too deep");
		}
		r_value.kind = JsonValue::Kind::Object;
		++offset;
		skip_whitespace();
		if (!at_end() && peek() == '}') {
			++offset;
			return true;
		}
		while (true) {
			skip_whitespace();
			if (at_end() || peek() != '"') {
				return fail("expected a string key");
			}
			std::string key;
			if (!parse_string(key)) {
				return false;
			}
			skip_whitespace();
			if (at_end() || peek() != ':') {
				return fail("expected ':'");
			}
			++offset;
			skip_whitespace();
			JsonValue member;
			if (!parse_value(member, p_depth + 1)) {
				return false;
			}
			r_value.keys.push_back(std::move(key));
			r_value.items.push_back(std::move(member));
			skip_whitespace();
			if (at_end()) {
				return fail("unterminated object");
			}
			if (peek() == ',') {
				++offset;
				continue;
			}
			if (peek() == '}') {
				++offset;
				return true;
			}
			return fail("expected ',' or '}'");
		}
	}

	bool parse_array(JsonValue &r_value, int p_depth) {
		if (p_depth >= kMaxDepth) {
			return fail("nesting too deep");
		}
		r_value.kind = JsonValue::Kind::Array;
		++offset;
		skip_whitespace();
		if (!at_end() && peek() == ']') {
			++offset;
			return true;
		}
		while (true) {
			skip_whitespace();
			JsonValue element;
			if (!parse_value(element, p_depth + 1)) {
				return false;
			}
			r_value.items.push_back(std::move(element));
			skip_whitespace();
			if (at_end()) {
				return fail("unterminated array");
			}
			if (peek() == ',') {
				++offset;
				continue;
			}
			if (peek() == ']') {
				++offset;
				return true;
			}
			return fail("expected ',' or ']'");
		}
	}

	bool read_hex4(uint32_t &r_unit) {
		if (offset + 4 > text.size()) {
			return fail("truncated \\u escape");
		}
		r_unit = 0;
		for (int i = 0; i < 4; ++i) {
			const char c = text[offset++];
			r_unit <<= 4;
			if (c >= '0' && c <= '9') {
				r_unit |= uint32_t(c - '0');
			} else if (c >= 'a' && c <= 'f') {
				r_unit |= uint32_t(c - 'a' + 10);
			} else if (c >= 'A' && c <= 'F') {
				r_unit |= uint32_t(c - 'A' + 10);
			} else {
				return fail("invalid hex digit in \\u escape");
			}
		}
		return true;
	}

	static void append_utf8(std::string &r_out, uint32_t p_code_point) {
		if (p_code_point < 0x80) {
			r_out += char(p_code_point);
		} else if (p_code_point < 0x800) {
			r_out += char(0xC0 | (p_code_point >> 6));
			r_out += char(0x80 | (p_code_point & 0x3F));
		} else if (p_code_point < 0x10000) {
			r_out += char(0xE0 | (p_code_point >> 12));
			r_out += char(0x80 | ((p_code_point >> 6) & 0x3F));
			r_out += char(0x80 | (p_code_point & 0x3F));
		} else {
			r_out += char(0xF0 | (p_code_point >> 18));
			r_out += char(0x80 | ((p_code_point >> 12) & 0x3F));
			r_out += char(0x80 | ((p_code_point >> 6) & 0x3F));
			r_out += char(0x80 | (p_code_point & 0x3F));
		}
	}

	// One UTF-8 sequence copied through, refusing overlong forms, surrogates and anything past
	// U+10FFFF.
	bool copy_utf8_sequence(std::string &r_out) {
		const uint8_t lead = uint8_t(text[offset]);
		size_t length;
		uint32_t minimum;
		uint32_t code_point;
		if (lead >= 0xC2 && lead <= 0xDF) {
			length = 2;
			minimum = 0x80;
			code_point = lead & 0x1F;
		} else if (lead >= 0xE0 && lead <= 0xEF) {
			length = 3;
			minimum = 0x800;
			code_point = lead & 0x0F;
		} else if (lead >= 0xF0 && lead <= 0xF4) {
			length = 4;
			minimum = 0x10000;
			code_point = lead & 0x07;
		} else {
			return fail("invalid UTF-8 in a string");
		}
		if (offset + length > text.size()) {
			return fail("invalid UTF-8 in a string");
		}
		for (size_t i = 1; i < length; ++i) {
			const uint8_t byte = uint8_t(text[offset + i]);
			if ((byte & 0xC0) != 0x80) {
				return fail("invalid UTF-8 in a string");
			}
			code_point = (code_point << 6) | (byte & 0x3F);
		}
		if (code_point < minimum || code_point > 0x10FFFF || (code_point >= 0xD800 && code_point <= 0xDFFF)) {
			return fail("invalid UTF-8 in a string");
		}
		r_out.append(text.data() + offset, length);
		offset += length;
		return true;
	}

	bool parse_string(std::string &r_out) {
		++offset;
		while (true) {
			if (at_end()) {
				return fail("unterminated string");
			}
			const uint8_t c = uint8_t(peek());
			if (c == '"') {
				++offset;
				return true;
			}
			if (c < 0x20) {
				return fail("control character in a string");
			}
			if (c >= 0x80) {
				if (!copy_utf8_sequence(r_out)) {
					return false;
				}
				continue;
			}
			++offset;
			if (c != '\\') {
				r_out += char(c);
				continue;
			}
			if (at_end()) {
				return fail("unterminated escape");
			}
			const char escape = text[offset++];
			switch (escape) {
				case '"':
					r_out += '"';
					break;
				case '\\':
					r_out += '\\';
					break;
				case '/':
					r_out += '/';
					break;
				case 'b':
					r_out += '\b';
					break;
				case 'f':
					r_out += '\f';
					break;
				case 'n':
					r_out += '\n';
					break;
				case 'r':
					r_out += '\r';
					break;
				case 't':
					r_out += '\t';
					break;
				case 'u': {
					uint32_t unit;
					if (!read_hex4(unit)) {
						return false;
					}
					if (unit >= 0xDC00 && unit <= 0xDFFF) {
						return fail("unpaired low surrogate escape");
					}
					if (unit >= 0xD800 && unit <= 0xDBFF) {
						if (offset + 2 > text.size() || text[offset] != '\\' || text[offset + 1] != 'u') {
							return fail("unpaired high surrogate escape");
						}
						offset += 2;
						uint32_t low;
						if (!read_hex4(low)) {
							return false;
						}
						if (low < 0xDC00 || low > 0xDFFF) {
							return fail("unpaired high surrogate escape");
						}
						unit = 0x10000 + ((unit - 0xD800) << 10) + (low - 0xDC00);
					}
					append_utf8(r_out, unit);
					break;
				}
				default:
					return fail("invalid escape");
			}
		}
	}

	bool is_digit(size_t p_at) const { return p_at < text.size() && text[p_at] >= '0' && text[p_at] <= '9'; }

	bool parse_number(JsonValue &r_value) {
		const size_t start = offset;
		size_t at = offset;
		if (at < text.size() && text[at] == '-') {
			++at;
		}
		if (!is_digit(at)) {
			return fail("invalid value");
		}
		if (text[at] == '0') {
			++at;
			if (is_digit(at)) {
				return fail("leading zero in a number");
			}
		} else {
			while (is_digit(at)) {
				++at;
			}
		}
		if (at < text.size() && text[at] == '.') {
			++at;
			if (!is_digit(at)) {
				offset = at;
				return fail("expected a digit after '.'");
			}
			while (is_digit(at)) {
				++at;
			}
		}
		if (at < text.size() && (text[at] == 'e' || text[at] == 'E')) {
			++at;
			if (at < text.size() && (text[at] == '+' || text[at] == '-')) {
				++at;
			}
			if (!is_digit(at)) {
				offset = at;
				return fail("expected a digit in an exponent");
			}
			while (is_digit(at)) {
				++at;
			}
		}
		r_value.kind = JsonValue::Kind::Number;
		r_value.text = std::string(text.substr(start, at - start));
		if (!parse_double(r_value.text, r_value.number)) {
			return fail("invalid number");
		}
		offset = at;
		return true;
	}
};

} // namespace

const JsonValue *JsonValue::find(std::string_view p_key) const {
	if (kind != Kind::Object) {
		return nullptr;
	}
	for (size_t i = 0; i < keys.size(); ++i) {
		if (keys[i] == p_key) {
			return &items[i];
		}
	}
	return nullptr;
}

bool json_parse(std::string_view p_text, JsonValue &r_value, std::string &r_error) {
	r_value = JsonValue();
	Parser parser(p_text, r_error);
	return parser.parse_document(r_value);
}

bool json_to_int64(const JsonValue &p_value, int64_t &r_result) {
	if (p_value.kind != JsonValue::Kind::Number) {
		return false;
	}
	const std::string &lexeme = p_value.text;
	if (lexeme.find_first_of(".eE") != std::string::npos) {
		return false;
	}
	const std::from_chars_result result = std::from_chars(lexeme.data(), lexeme.data() + lexeme.size(), r_result);
	return result.ec == std::errc() && result.ptr == lexeme.data() + lexeme.size();
}

} // namespace vm
