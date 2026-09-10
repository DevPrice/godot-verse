#include "verse_class_decl.h"

#include "verse_lexer.h"

#include <vector>

namespace {

constexpr const char *GlobalClassAttribute = "global_class";
constexpr const char *AbstractSpecifier = "abstract";

bool is_ident_start(char p_c) {
	return (p_c >= 'a' && p_c <= 'z') || (p_c >= 'A' && p_c <= 'Z') || p_c == '_';
}

bool is_ident_char(char p_c) {
	return is_ident_start(p_c) || (p_c >= '0' && p_c <= '9');
}

std::vector<std::string> split_lines(const std::string &p_source) {
	std::vector<std::string> lines;
	std::string current;
	for (char c : p_source) {
		if (c == '\n') {
			lines.push_back(current);
			current.clear();
		} else if (c != '\r') {
			current.push_back(c);
		}
	}
	lines.push_back(current);
	return lines;
}

void skip_spaces(const std::string &p_line, size_t &r_pos) {
	while (r_pos < p_line.size() && (p_line[r_pos] == ' ' || p_line[r_pos] == '\t')) {
		r_pos++;
	}
}

std::string take_identifier(const std::string &p_line, size_t &r_pos) {
	if (r_pos >= p_line.size() || !is_ident_start(p_line[r_pos])) {
		return std::string();
	}
	const size_t start = r_pos;
	while (r_pos < p_line.size() && is_ident_char(p_line[r_pos])) {
		r_pos++;
	}
	return p_line.substr(start, r_pos - start);
}

bool take_literal(const std::string &p_line, size_t &r_pos, const char *p_text) {
	size_t pos = r_pos;
	for (const char *c = p_text; *c != '\0'; c++) {
		if (pos >= p_line.size() || p_line[pos] != *c) {
			return false;
		}
		pos++;
	}
	r_pos = pos;
	return true;
}

// `<abstract>` and friends. Reports whether the run contained `abstract`; a specifier list this
// does not understand is skipped rather than refused, because an unknown one says nothing about
// whether the class is a global class.
bool take_specifiers(const std::string &p_line, size_t &r_pos) {
	bool abstract = false;
	while (r_pos < p_line.size() && p_line[r_pos] == '<') {
		r_pos++;
		const std::string specifier = take_identifier(p_line, r_pos);
		if (specifier == AbstractSpecifier) {
			abstract = true;
		}
		while (r_pos < p_line.size() && p_line[r_pos] != '>') {
			r_pos++;
		}
		if (r_pos < p_line.size()) {
			r_pos++;
		}
	}
	return abstract;
}

// The first name in `(node2d)` or `(node2d, some_interface)`. Godot has one base type per node,
// so the rest of the list is Verse interfaces the registry has no place for.
std::string take_first_super(const std::string &p_line, size_t &r_pos) {
	if (r_pos >= p_line.size() || p_line[r_pos] != '(') {
		return std::string();
	}
	r_pos++;
	skip_spaces(p_line, r_pos);
	return take_identifier(p_line, r_pos);
}

// The kind the lexer gave column 0, or Text when the line is blank or starts indented. Only a
// definition at column 0 is the file's top-level class, so an indented line never matches.
bool first_token_at_column_zero(const std::vector<VerseToken> &p_tokens, VerseTokenKind &r_kind) {
	if (p_tokens.empty() || p_tokens[0].column != 0) {
		return false;
	}
	r_kind = p_tokens[0].kind;
	return true;
}

} // namespace

namespace {

// Godot spells its dimensional suffixes `2D` and `3D` -- Node2D, Camera3D, Sprite2D. A Verse name
// carries them lowercase and nothing in the text marks the boundary, so the rule is deliberately
// narrow: a `d` closing a word with a digit directly before it, and nothing else. `add` and `hud`
// keep their last letter, and `vector2i` is left alone because Godot spells that one lowercase too.
void uppercase_dimension_suffix(std::string &r_result, size_t p_word_start) {
	const size_t len = r_result.size();
	if (len - p_word_start >= 2 && r_result[len - 1] == 'd' &&
			r_result[len - 2] >= '0' && r_result[len - 2] <= '9') {
		r_result[len - 1] = 'D';
	}
}

} // namespace

std::string verse_pascal_case(const std::string &p_verse_name) {
	std::string result;
	result.reserve(p_verse_name.size());

	size_t word_start = 0;
	bool at_word_start = true;
	for (char c : p_verse_name) {
		if (c == '_') {
			if (!at_word_start) {
				uppercase_dimension_suffix(result, word_start);
			}
			at_word_start = true;
			continue;
		}
		if (at_word_start) {
			word_start = result.size();
			result.push_back(c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c);
		} else {
			result.push_back(c);
		}
		at_word_start = false;
	}
	if (!at_word_start) {
		uppercase_dimension_suffix(result, word_start);
	}
	return result;
}

VerseClassDecl verse_scan_class_decl(const std::string &p_source) {
	VerseClassDecl decl;
	bool pending_global = false;

	VerseLexState state;
	std::vector<VerseToken> tokens;

	int row = -1;
	for (const std::string &line : split_lines(p_source)) {
		row++;
		tokens.clear();
		verse_lex_line(line, state, tokens);

		VerseTokenKind kind = VerseTokenKind::Text;
		if (!first_token_at_column_zero(tokens, kind)) {
			continue;
		}
		// A comment between an attribute and its definition is ordinary, so this does not clear
		// what is pending -- unlike a definition that turns out not to be a class, below.
		if (kind == VerseTokenKind::Comment) {
			continue;
		}

		if (kind == VerseTokenKind::Attribute) {
			size_t pos = 1; // Past the '@'.
			if (take_identifier(line, pos) == GlobalClassAttribute) {
				pending_global = true;
			}
			continue;
		}

		size_t pos = 0;
		const std::string name = take_identifier(line, pos);
		if (name.empty()) {
			continue;
		}
		skip_spaces(line, pos);
		// A `var` or a plain `:` definition is not a class, and neither is `x := "class"` -- the
		// literal has to follow the `:=` for this to be a class definition at all.
		if (!take_literal(line, pos, ":=")) {
			pending_global = false;
			continue;
		}
		skip_spaces(line, pos);
		size_t after_class = pos;
		if (take_identifier(line, after_class) != "class") {
			pending_global = false;
			continue;
		}
		pos = after_class;

		decl.name = name;
		decl.line = row;
		decl.is_abstract = take_specifiers(line, pos);
		decl.base = take_first_super(line, pos);
		decl.is_global = pending_global;
		return decl;
	}

	return decl;
}
