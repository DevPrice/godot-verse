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
	FunctionDefinition,
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

// Whether a cursor at p_line/p_column of p_source stands inside a comment. p_line is 0-based and
// p_column a byte offset into it, counted the way the lexer counts columns.
//
// A cursor sits between two characters rather than on one, so what it is inside is decided by the
// character behind it: `Print #` with the caret on the `#` is still in code. Lexing runs from the
// start of the source, since a `<#` several lines above is what makes the line a comment at all.
bool verse_position_in_comment(const std::string &p_source, int p_line, int p_column);

// Whether a cursor at p_line/p_column of p_source stands inside a string literal. Same contract
// and the same "the character behind the cursor decides" rule as verse_position_in_comment.
//
// True for String and Escape tokens. False for Interpolation and for anything lexed inside a
// `{...}` interpolation -- that is code, and completion there is wanted the same as anywhere else.
bool verse_position_in_string(const std::string &p_source, int p_line, int p_column);
