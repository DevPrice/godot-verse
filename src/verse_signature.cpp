#include "verse_signature.h"

namespace {

std::string trimmed(const std::string &p_text) {
	size_t begin = 0;
	size_t end = p_text.size();
	while (begin < end && (p_text[begin] == ' ' || p_text[begin] == '\t')) {
		begin++;
	}
	while (end > begin && (p_text[end - 1] == ' ' || p_text[end - 1] == '\t')) {
		end--;
	}
	return p_text.substr(begin, end - begin);
}

// The depth change one character makes, counting every bracket family together: a comma or colon
// is a separator only where all of them are balanced, so a type's own `(`, `[` or `{` shields what
// it contains.
int nesting_delta(char p_c) {
	if (p_c == '(' || p_c == '[' || p_c == '{') {
		return 1;
	}
	if (p_c == ')' || p_c == ']' || p_c == '}') {
		return -1;
	}
	return 0;
}

// The index of the first top-level occurrence of p_c in [p_begin, p_end), or std::string::npos.
size_t find_top_level(const std::string &p_text, char p_c, size_t p_begin, size_t p_end) {
	int depth = 0;
	for (size_t i = p_begin; i < p_end; i++) {
		depth += nesting_delta(p_text[i]);
		if (depth == 0 && p_text[i] == p_c) {
			return i;
		}
	}
	return std::string::npos;
}

// One `Name:type` parameter. A type may hold a colon of its own -- a function-type parameter is
// `Pred:(:int)->logic` -- so the split is at the first top-level colon, which is the one between
// the name and the type.
VerseSignatureParam parse_param(const std::string &p_param) {
	VerseSignatureParam param;
	const size_t colon = find_top_level(p_param, ':', 0, p_param.size());
	if (colon == std::string::npos) {
		param.name = trimmed(p_param);
		return param;
	}
	param.name = trimmed(p_param.substr(0, colon));
	param.type = trimmed(p_param.substr(colon + 1));
	return param;
}

} // namespace

VerseSignature verse_parse_signature(const std::string &p_signature) {
	VerseSignature out;

	const size_t open = p_signature.find('(');
	if (open == std::string::npos) {
		return out;
	}

	// The matching close of the parameter list, by paren depth alone: a parameter's own brackets or
	// braces do not close it, but its nested parens (a function-type parameter) do have to balance.
	size_t close = std::string::npos;
	int depth = 0;
	for (size_t i = open; i < p_signature.size(); i++) {
		const char c = p_signature[i];
		if (c == '(') {
			depth++;
		} else if (c == ')') {
			if (--depth == 0) {
				close = i;
				break;
			}
		}
	}
	if (close == std::string::npos) {
		return out;
	}

	// The parameters, split at top-level commas. An empty list has no parameters rather than one
	// empty one.
	const std::string inside = trimmed(p_signature.substr(open + 1, close - open - 1));
	if (!inside.empty()) {
		size_t begin = 0;
		while (begin <= inside.size()) {
			const size_t comma = find_top_level(inside, ',', begin, inside.size());
			const size_t end = comma == std::string::npos ? inside.size() : comma;
			out.params.push_back(parse_param(inside.substr(begin, end - begin)));
			if (comma == std::string::npos) {
				break;
			}
			begin = comma + 1;
		}
	}

	// The tail is the effect specifiers, then the top-level `:` before the result type. The result
	// may be a function type with a colon of its own, so the first top-level colon is the divider.
	const size_t colon = find_top_level(p_signature, ':', close + 1, p_signature.size());
	if (colon == std::string::npos) {
		return out;
	}
	out.result_type = trimmed(p_signature.substr(colon + 1));

	// The specifiers sit between the closing paren and that colon, as `<a><b>`. Drop the angle
	// brackets and join with spaces, so `<suspends><decides>` reads as `suspends decides` -- the
	// shape Godot's qualifier renderer expects.
	const std::string effects = p_signature.substr(close + 1, colon - close - 1);
	std::string token;
	for (char c : effects) {
		if (c == '<') {
			token.clear();
		} else if (c == '>') {
			if (!token.empty()) {
				if (!out.specifiers.empty()) {
					out.specifiers += ' ';
				}
				out.specifiers += token;
			}
			token.clear();
		} else {
			token += c;
		}
	}

	return out;
}
