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
			} else {
				emit(col, VerseTokenKind::String);
				col += 1;
			}
			continue;
		}

		// top == 'I' (interpolation code) and top == '\0' (top-level code) share the same
		// expression-position grammar; only {}-matching is specific to being inside a string.
		const char c = p_line[col];
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
			emit(col, classify_word(p_line.substr(col, end - col)));
			col = end;
		} else if (c == '<' && is_ident_start(char_at(p_line, col + 1))) {
			const int end = scan_identifier_end(p_line, col + 1);
			if (char_at(p_line, end) == '>') {
				emit(col, VerseTokenKind::Attribute);
				col = end + 1;
			} else {
				emit(col, VerseTokenKind::Text);
				col += 1;
			}
		} else {
			emit(col, VerseTokenKind::Text);
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
