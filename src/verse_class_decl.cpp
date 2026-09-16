#include "verse_class_decl.h"

#include "verse_lexer.h"

#include <vector>

namespace {

constexpr const char *GlobalClassAttribute = "global_class";
constexpr const char *ToolAttribute = "tool";
constexpr const char *IconAttribute = "icon";
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

// The one string inside an attribute's parentheses -- `@icon("res://a.svg")` gives `res://a.svg`.
//
// Empty for an attribute written without one, and for anything that is not a plain quoted literal:
// an attribute's argument is evaluated by the compiler and this is a text scan, so the only thing
// it can honestly read is a literal. A path built from an expression is not a case Godot can be
// told about anyway -- it asks for the icon before anything has been compiled.
//
// No escape handling, deliberately: a `res://` path containing a quote is not reachable from
// Godot's filesystem, so treating a backslash as ordinary keeps Windows paths readable instead.
std::string take_quoted_argument(const std::string &p_line, size_t p_pos) {
	skip_spaces(p_line, p_pos);
	if (p_pos >= p_line.size() || p_line[p_pos] != '(') {
		return std::string();
	}
	p_pos++;
	skip_spaces(p_line, p_pos);
	if (p_pos >= p_line.size() || p_line[p_pos] != '"') {
		return std::string();
	}
	p_pos++;
	const size_t start = p_pos;
	while (p_pos < p_line.size() && p_line[p_pos] != '"') {
		p_pos++;
	}
	if (p_pos >= p_line.size()) {
		return std::string();
	}
	return p_line.substr(start, p_pos - start);
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

std::string verse_snake_case(const std::string &p_verse_name) {
	std::string result;
	result.reserve(p_verse_name.size() + 4);

	for (size_t i = 0; i < p_verse_name.size(); i++) {
		const char c = p_verse_name[i];
		const bool is_upper = c >= 'A' && c <= 'Z';
		if (is_upper && i > 0) {
			const char prev = p_verse_name[i - 1];
			const bool prev_lower = prev >= 'a' && prev <= 'z';
			const bool prev_upper = prev >= 'A' && prev <= 'Z';
			const bool next_lower = i + 1 < p_verse_name.size()
					&& p_verse_name[i + 1] >= 'a' && p_verse_name[i + 1] <= 'z';
			if (prev_lower || (prev_upper && next_lower)) {
				result.push_back('_');
			}
		}
		result.push_back(is_upper ? static_cast<char>(c - 'A' + 'a') : c);
	}
	return result;
}

VerseClassDecl verse_scan_class_decl(const std::string &p_source, const std::string &p_file_stem) {
	VerseClassDecl decl;
	bool pending_global = false;
	bool pending_tool = false;
	std::string pending_icon;
	// Where the pending `@global_class` was written, so a warning about an inert one lands on the
	// attribute rather than on the class it failed to register.
	int pending_global_line = -1;
	// The file's own class, once seen. The scan no longer stops there: every top-level class after
	// it still has to be looked at, because an inert `@global_class` can be written below the
	// script's class as easily as above it.
	bool found_the_class = false;

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
			const std::string attribute = take_identifier(line, pos);
			if (attribute == GlobalClassAttribute) {
				pending_global = true;
				pending_global_line = row;
			} else if (attribute == ToolAttribute) {
				pending_tool = true;
			} else if (attribute == IconAttribute) {
				pending_icon = take_quoted_argument(line, pos);
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
			pending_tool = false;
			pending_icon.clear();
			continue;
		}
		skip_spaces(line, pos);
		size_t after_class = pos;
		if (take_identifier(line, after_class) != "class") {
			pending_global = false;
			pending_tool = false;
			pending_icon.clear();
			continue;
		}
		pos = after_class;

		// Every other top-level class in the file is somebody else's -- a helper, a base, a
		// parametric one -- and skipping it is the whole of what modules changed here. Note the
		// attributes above *it* go with it, so pending_global clears with it too.
		//
		// With no stem to match, the first class is the file's: a caller with no file in hand still
		// gets one answer, and every class after it is one of the others.
		const bool is_the_class = p_file_stem.empty() ? !found_the_class : name == p_file_stem;
		if (!is_the_class) {
			// The attribute asks for something Godot has nowhere to put. Recorded rather than
			// dropped, because silently ignoring a request is the defect -- see the struct.
			if (pending_global) {
				decl.inert_global_classes.push_back({ name, pending_global_line });
			}
			pending_global = false;
			pending_tool = false;
			pending_icon.clear();
			pending_global_line = -1;
			continue;
		}

		decl.name = name;
		decl.line = row;
		decl.is_abstract = take_specifiers(line, pos);
		decl.base = take_first_super(line, pos);
		decl.is_global = pending_global;
		decl.is_tool = pending_tool;
		decl.icon_path = pending_icon;
		found_the_class = true;
		pending_global = false;
		pending_tool = false;
		pending_icon.clear();
		pending_global_line = -1;
	}

	return decl;
}
