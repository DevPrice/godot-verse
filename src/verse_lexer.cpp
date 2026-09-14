#include "verse_lexer.h"

#include "verse_keywords.h"

#include <cctype>
#include <iterator>

namespace {

bool is_ident_start(char p_c) {
	return std::isalpha(static_cast<unsigned char>(p_c)) || p_c == '_';
}

bool is_ident_continue(char p_c) {
	return std::isalnum(static_cast<unsigned char>(p_c)) || p_c == '_';
}

int leading_indent_columns(const std::string &p_line) {
	int col = 0;
	for (char c : p_line) {
		if (c == ' ') {
			col += 1;
		} else if (c == '\t') {
			col = (col / 4 + 1) * 4;
		} else {
			break;
		}
	}
	return col;
}

bool is_blank_line(const std::string &p_line) {
	for (char c : p_line) {
		if (c != ' ' && c != '\t' && c != '\r') {
			return false;
		}
	}
	return true;
}

char char_at(const std::string &p_line, int p_pos) {
	return p_pos >= 0 && p_pos < (int)p_line.size() ? p_line[p_pos] : '\0';
}

bool at_indent_comment_open(const std::string &p_line, int p_col) {
	return char_at(p_line, p_col) == '<' && char_at(p_line, p_col + 1) == '#' && char_at(p_line, p_col + 2) == '>';
}

bool at_block_comment_open(const std::string &p_line, int p_col) {
	return char_at(p_line, p_col) == '<' && char_at(p_line, p_col + 1) == '#' && char_at(p_line, p_col + 2) != '>';
}

bool at_line_comment_open(const std::string &p_line, int p_col) {
	return char_at(p_line, p_col) == '#' && char_at(p_line, p_col + 1) != '>';
}

int scan_identifier_end(const std::string &p_line, int p_start) {
	int end = p_start + 1;
	while (end < (int)p_line.size() && is_ident_continue(p_line[end])) {
		end += 1;
	}
	return end;
}

int scan_number_end(const std::string &p_line, int p_start) {
	const int len = (int)p_line.size();
	int end = p_start;

	if (char_at(p_line, end) == '0' && (char_at(p_line, end + 1) == 'x' || char_at(p_line, end + 1) == 'X')) {
		end += 2;
		while (end < len && std::isxdigit(static_cast<unsigned char>(p_line[end]))) {
			end += 1;
		}
		return end;
	}

	while (end < len && std::isdigit(static_cast<unsigned char>(p_line[end]))) {
		end += 1;
	}
	if (char_at(p_line, end) == '.' && std::isdigit(static_cast<unsigned char>(char_at(p_line, end + 1)))) {
		end += 1;
		while (end < len && std::isdigit(static_cast<unsigned char>(p_line[end]))) {
			end += 1;
		}
	}
	if (char_at(p_line, end) == 'e' || char_at(p_line, end) == 'E') {
		int exp_end = end + 1;
		if (char_at(p_line, exp_end) == '+' || char_at(p_line, exp_end) == '-') {
			exp_end += 1;
		}
		if (std::isdigit(static_cast<unsigned char>(char_at(p_line, exp_end)))) {
			while (exp_end < len && std::isdigit(static_cast<unsigned char>(p_line[exp_end]))) {
				exp_end += 1;
			}
			end = exp_end;
		}
	}
	return end;
}

bool at_attribute(const std::string &p_line, int p_pos, int &r_end) {
	if (!is_ident_start(char_at(p_line, p_pos + 1))) {
		return false;
	}
	const int end = scan_identifier_end(p_line, p_pos + 1);
	if (char_at(p_line, end) != '>') {
		return false;
	}
	r_end = end + 1;
	return true;
}

// `Print(` and `GetPosition[` are calls, and so is a definition like `Ready<override>()`, where
// specifiers sit between the name and its parameter list. `[` counts because a <decides> call is
// spelled with brackets.
bool is_call_position(const std::string &p_line, int p_pos) {
	int pos = p_pos;
	int end = 0;
	while (char_at(p_line, pos) == '<' && at_attribute(p_line, pos, end)) {
		pos = end;
	}
	return char_at(p_line, pos) == '(' || char_at(p_line, pos) == '[';
}

// A `/`-led run at the start of a Verse path (`/Godot.org/Godot`) rather than a division: the
// slash must open a token -- preceded by start of line, whitespace, `{`, `(` or `,` -- and lead
// straight into a label with no space, which `A / B` never does.
bool at_verse_path_start(const std::string &p_line, int p_pos) {
	if (char_at(p_line, p_pos) != '/' || !is_ident_start(char_at(p_line, p_pos + 1))) {
		return false;
	}
	if (p_pos == 0) {
		return true;
	}
	const char prev = p_line[(size_t)(p_pos - 1)];
	return prev == ' ' || prev == '\t' || prev == '\r' || prev == '{' || prev == '(' || prev == ',';
}

int scan_verse_path_end(const std::string &p_line, int p_start) {
	const int len = (int)p_line.size();
	int end = p_start;
	while (end < len) {
		const char c = p_line[(size_t)end];
		if (c == ' ' || c == '\t' || c == '\r' || c == '}' || c == ')' || c == ',') {
			break;
		}
		end += 1;
	}
	return end;
}

bool starts_line(const std::string &p_line, int p_start) {
	for (int i = 0; i < p_start; i++) {
		const char c = p_line[(size_t)i];
		if (c != ' ' && c != '\t' && c != '\r') {
			return false;
		}
	}
	return true;
}

// One past the bracket matching the one at p_pos, or -1. String literals are skipped so that a
// bracket inside one cannot unbalance the scan.
int matching_bracket_end(const std::string &p_line, int p_pos) {
	const char open = char_at(p_line, p_pos);
	const char close = open == '(' ? ')' : ']';
	int depth = 0;
	bool in_string = false;
	for (int i = p_pos; i < (int)p_line.size(); i++) {
		const char c = p_line[(size_t)i];
		if (in_string) {
			if (c == '\\') {
				i += 1;
			} else if (c == '"') {
				in_string = false;
			}
		} else if (c == '"') {
			in_string = true;
		} else if (c == open) {
			depth += 1;
		} else if (c == close) {
			depth -= 1;
			if (depth == 0) {
				return i + 1;
			}
		}
	}
	return -1;
}

// A definition binds the name it starts with: `Ready<override>():void =`. Verse has no `func`
// keyword, so the discriminator is the `=` after the parameter list -- a call never has one,
// and the `=` of a comparison or of an interpolated string sits inside the brackets, which this
// scan has already passed. Only `(` counts here, unlike is_call_position: a function is always
// declared with parentheses, and `[` is only ever a call to a <decides> function.
bool is_definition_position(const std::string &p_line, int p_pos) {
	int pos = p_pos;
	int end = 0;
	while (char_at(p_line, pos) == '<' && at_attribute(p_line, pos, end)) {
		pos = end;
	}
	if (char_at(p_line, pos) != '(') {
		return false;
	}
	pos = matching_bracket_end(p_line, pos);
	if (pos < 0) {
		return false;
	}
	for (int i = pos; i < (int)p_line.size(); i++) {
		if (p_line[(size_t)i] != '=') {
			continue;
		}
		// `<=`, `>=`, `!=` and `==` are comparisons, not the start of a body.
		const char prev = i > 0 ? p_line[(size_t)i - 1] : '\0';
		return prev != '<' && prev != '>' && prev != '!' && char_at(p_line, i + 1) != '=';
	}
	return false;
}

VerseTokenKind classify_word(const std::string &p_word) {
	for (size_t i = 0; i < std::size(verse_keywords::control_flow_words); i++) {
		if (p_word == verse_keywords::control_flow_words[i]) {
			return VerseTokenKind::ControlKeyword;
		}
	}
	for (size_t i = 0; i < std::size(verse_keywords::reserved_words); i++) {
		if (p_word == verse_keywords::reserved_words[i]) {
			return VerseTokenKind::Keyword;
		}
	}
	return VerseTokenKind::Text;
}

VerseTokenKind classify_identifier(const std::string &p_line, int p_start, int p_end, bool p_after_dot) {
	const VerseTokenKind word = classify_word(p_line.substr(p_start, p_end - p_start));
	if (word != VerseTokenKind::Text) {
		return word;
	}
	if (is_call_position(p_line, p_end)) {
		return starts_line(p_line, p_start) && is_definition_position(p_line, p_end)
				? VerseTokenKind::FunctionDefinition
				: VerseTokenKind::Function;
	}
	return p_after_dot ? VerseTokenKind::Member : VerseTokenKind::Identifier;
}

// The kind covering byte p_column, or false when nothing does. Tokens run in increasing column
// order and each one holds until the next starts, so the last that begins at or before the byte
// is the one it belongs to.
bool kind_at(const std::vector<VerseToken> &p_tokens, int p_column, VerseTokenKind &r_kind) {
	bool found = false;
	for (const VerseToken &token : p_tokens) {
		if (token.column > p_column) {
			break;
		}
		r_kind = token.kind;
		found = true;
	}
	return found;
}

} // namespace

bool VerseLexState::operator==(const VerseLexState &p_other) const {
	return block_comment_depth == p_other.block_comment_depth &&
			indent_comment_column == p_other.indent_comment_column &&
			in_string == p_other.in_string &&
			interpolation_depth == p_other.interpolation_depth;
}

void verse_lex_line(const std::string &p_line, VerseLexState &p_state, std::vector<VerseToken> &r_tokens) {
	const int len = (int)p_line.size();

	if (p_state.indent_comment_column >= 0) {
		if (is_blank_line(p_line)) {
			return;
		}
		if (leading_indent_columns(p_line) > p_state.indent_comment_column) {
			if (len > 0) {
				r_tokens.push_back({ 0, VerseTokenKind::Comment });
			}
			return;
		}
		p_state.indent_comment_column = -1;
	}

	// 'S' for raw string content, 'I' for code inside a {} interpolation. Nested {} within an
	// interpolation just push/pop another 'I'; a nested "..." pushes another 'S'. Only the
	// innermost run of 'I's on top of a single 'S' survives into p_state, since VerseLexState
	// has no room for a full stack -- a string left unterminated at end of line while nested
	// inside another string's interpolation won't resume correctly on the next line.
	std::vector<char> frames;
	if (p_state.in_string) {
		frames.push_back('S');
		for (int i = 0; i < p_state.interpolation_depth; i++) {
			frames.push_back('I');
		}
	}
	int block_depth = p_state.block_comment_depth;
	bool pending_member = false;

	VerseTokenKind last_kind = VerseTokenKind::Text;
	bool has_last = false;
	auto emit = [&](int p_col, VerseTokenKind p_kind) {
		if (!has_last || last_kind != p_kind) {
			r_tokens.push_back({ p_col, p_kind });
			last_kind = p_kind;
			has_last = true;
		}
	};

	int col = 0;
	while (col < len) {
		if (block_depth > 0) {
			if (at_indent_comment_open(p_line, col)) {
				// "<#>" inside a block comment is plain text: only Space, Content and IndCmt
				// places trigger an indented comment, and BlockCmt is none of those.
				emit(col, VerseTokenKind::Comment);
				col += 3;
			} else if (at_block_comment_open(p_line, col)) {
				emit(col, VerseTokenKind::Comment);
				block_depth += 1;
				col += 2;
			} else if (char_at(p_line, col) == '#' && char_at(p_line, col + 1) == '>') {
				emit(col, VerseTokenKind::Comment);
				block_depth -= 1;
				col += 2;
			} else {
				emit(col, VerseTokenKind::Comment);
				col += 1;
			}
			continue;
		}

		const char top = frames.empty() ? '\0' : frames.back();

		if (top == 'S') {
			const char c = p_line[col];
			if (c == '"') {
				frames.pop_back();
				emit(col, VerseTokenKind::String);
				col += 1;
			} else if (c == '\\') {
				emit(col, VerseTokenKind::Escape);
				col += (col + 1 < len) ? 2 : 1;
			} else if (c == '{') {
				frames.push_back('I');
				emit(col, VerseTokenKind::Interpolation);
				col += 1;
			} else if (at_block_comment_open(p_line, col)) {
				emit(col, VerseTokenKind::Comment);
				block_depth += 1;
				col += 2;
			} else if (at_line_comment_open(p_line, col)) {
				emit(col, VerseTokenKind::Comment);
				col = len;
			} else {
				// "<#>" is not tested here: Text<EPlace::String>'s '<' case only reaches IndCmt
				// for Space, Content and IndCmt places, and falls to plain text otherwise. Each of
				// the three characters lands in this branch in turn and coalesces into one String
				// token.
				emit(col, VerseTokenKind::String);
				col += 1;
			}
			continue;
		}

		// top == 'I' (interpolation code) and top == '\0' (top-level code) share the same
		// expression-position grammar; only {}-matching is specific to being inside a string.
		const char c = p_line[col];

		// Only an identifier directly behind a dot is a member access, so any other token clears
		// the flag -- whitespace included, which is why `A . B` does not colour B as a member.
		const bool after_dot = pending_member;
		pending_member = false;
		if (top == 'I' && c == '{') {
			frames.push_back('I');
			emit(col, VerseTokenKind::Interpolation);
			col += 1;
		} else if (top == 'I' && c == '}') {
			frames.pop_back();
			emit(col, VerseTokenKind::Interpolation);
			col += 1;
		} else if (c == '"') {
			frames.push_back('S');
			emit(col, VerseTokenKind::String);
			col += 1;
		} else if (c == '\'') {
			emit(col, VerseTokenKind::String);
			col += 1;
			while (col < len && p_line[col] != '\'') {
				if (p_line[col] == '\\') {
					emit(col, VerseTokenKind::Escape);
					col += (col + 1 < len) ? 2 : 1;
				} else {
					emit(col, VerseTokenKind::String);
					col += 1;
				}
			}
			if (col < len) {
				emit(col, VerseTokenKind::String);
				col += 1;
			}
		} else if (at_indent_comment_open(p_line, col)) {
			p_state.indent_comment_column = leading_indent_columns(p_line);
			emit(col, VerseTokenKind::Comment);
			col = len;
		} else if (at_block_comment_open(p_line, col)) {
			emit(col, VerseTokenKind::Comment);
			block_depth += 1;
			col += 2;
		} else if (at_line_comment_open(p_line, col)) {
			emit(col, VerseTokenKind::Comment);
			col = len;
		} else if (std::isdigit(static_cast<unsigned char>(c))) {
			const int end = scan_number_end(p_line, col);
			emit(col, VerseTokenKind::Number);
			col = end;
		} else if (is_ident_start(c)) {
			const int end = scan_identifier_end(p_line, col);
			emit(col, classify_identifier(p_line, col, end, after_dot));
			col = end;
		} else if (c == '@' && is_ident_start(char_at(p_line, col + 1))) {
			// The prefix attribute form: @editable, @clamp_min("0.0"). Its argument list, if any,
			// falls out of the attribute token and lexes as ordinary code.
			emit(col, VerseTokenKind::Attribute);
			col = scan_identifier_end(p_line, col + 1);
		} else if (c == '<' && is_ident_start(char_at(p_line, col + 1))) {
			int end = 0;
			if (at_attribute(p_line, col, end)) {
				emit(col, VerseTokenKind::Attribute);
				col = end;
			} else {
				emit(col, VerseTokenKind::Symbol);
				col += 1;
			}
		} else if (c == '/' && at_verse_path_start(p_line, col)) {
			// Lexed whole so its internal `.`s never set pending_member -- a path segment is not
			// a member access, and `using { /Godot.org/Godot }` is the first line of every script.
			const int end = scan_verse_path_end(p_line, col);
			emit(col, VerseTokenKind::Identifier);
			col = end;
		} else {
			// Whitespace carries no glyph to colour, and calling it a symbol would split every
			// run of plain text in two for nothing.
			const bool is_space = c == ' ' || c == '\t' || c == '\r';
			pending_member = c == '.';
			emit(col, is_space ? VerseTokenKind::Text : VerseTokenKind::Symbol);
			col += 1;
		}
	}

	p_state.block_comment_depth = block_depth;
	p_state.in_string = !frames.empty();
	p_state.interpolation_depth = 0;
	for (char f : frames) {
		if (f == 'I') {
			p_state.interpolation_depth += 1;
		}
	}
}

namespace {

// Lexes p_source line by line up to p_line and hands p_on_line that line's tokens, plus the
// state the lexer carried into it -- which is what a line with no tokens at all (a blank line
// inside a block comment, or inside an unterminated string) has to be classified by instead.
// Shared by verse_position_in_comment and verse_position_in_string, which differ only in which
// token kinds count as a match and what "inherited" means for their own kind.
template <typename OnLine>
void verse_walk_to_line(const std::string &p_source, int p_line, OnLine p_on_line) {
	VerseLexState state;
	std::vector<VerseToken> tokens;

	size_t start = 0;
	for (int row = 0; row <= p_line; row++) {
		const size_t newline = p_source.find('\n', start);
		const size_t end = newline == std::string::npos ? p_source.size() : newline;
		std::string line = p_source.substr(start, end - start);
		if (!line.empty() && line.back() == '\r') {
			line.pop_back();
		}

		const VerseLexState state_before = state;
		tokens.clear();
		verse_lex_line(line, state, tokens);

		if (row < p_line) {
			if (newline == std::string::npos) {
				return;
			}
			start = newline + 1;
			continue;
		}

		p_on_line(tokens, state_before);
		return;
	}
}

} // namespace

bool verse_position_in_comment(const std::string &p_source, int p_line, int p_column) {
	bool result = false;
	verse_walk_to_line(p_source, p_line, [&](const std::vector<VerseToken> &tokens, const VerseLexState &state_before) {
		// A comment the line opened above it covers the line whole, column zero included -- where
		// there is no character behind the cursor to classify, and where a blank line inside a
		// block comment has no token at all.
		const bool inherited = state_before.block_comment_depth > 0 || state_before.indent_comment_column >= 0;

		if (tokens.empty()) {
			result = inherited;
			return;
		}
		VerseTokenKind kind = VerseTokenKind::Text;
		if (p_column == 0) {
			result = inherited && kind_at(tokens, 0, kind) && kind == VerseTokenKind::Comment;
			return;
		}
		result = kind_at(tokens, p_column - 1, kind) && kind == VerseTokenKind::Comment;
	});
	return result;
}

bool verse_position_in_string(const std::string &p_source, int p_line, int p_column) {
	bool result = false;
	verse_walk_to_line(p_source, p_line, [&](const std::vector<VerseToken> &tokens, const VerseLexState &state_before) {
		// An unterminated string left open by the line above covers a blank line whole, the same
		// way an open block comment does.
		const bool inherited = state_before.in_string;

		if (tokens.empty()) {
			result = inherited;
			return;
		}
		VerseTokenKind kind = VerseTokenKind::Text;
		if (p_column == 0) {
			result = inherited && kind_at(tokens, 0, kind) && (kind == VerseTokenKind::String || kind == VerseTokenKind::Escape);
			return;
		}
		result = kind_at(tokens, p_column - 1, kind) && (kind == VerseTokenKind::String || kind == VerseTokenKind::Escape);
	});
	return result;
}
