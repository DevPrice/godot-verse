#pragma once

#include <string>
#include <vector>

enum class VerseTokenKind {
	Text,
	Identifier,
	Comment,
	String,
	Escape,
	Interpolation,
	Number,
	Keyword,
	ControlKeyword,
	Attribute,
	Symbol,
	Function,
	Member,
};

// column is a 0-based byte offset into the line.
struct VerseToken {
	int column;
	VerseTokenKind kind;
};

// Everything the lexer needs to resume on the next line. Value-comparable so a cache can
// detect when re-lexing a line changed nothing downstream.
struct VerseLexState {
	int block_comment_depth = 0;
	int indent_comment_column = -1;
	bool in_string = false;
	int interpolation_depth = 0;

	bool operator==(const VerseLexState &p_other) const;
};

// Lexes one line, appending tokens in strictly increasing column order, and advances p_state
// to the state the next line starts in.
void verse_lex_line(const std::string &p_line, VerseLexState &p_state, std::vector<VerseToken> &r_tokens);
