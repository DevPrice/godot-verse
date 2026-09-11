// Standalone driver that lexes hand-written Verse snippets and checks the resulting token
// stream against the grammar's comment/string/interpolation rules, with no Godot and no
// godot-cpp dependency.
#include "verse_lexer.h"

#include <cstdio>
#include <string>
#include <vector>

namespace {

bool Step(const char* Name, bool Result)
{
	printf("[verse_lexer_test] %s: %s\n", Name, Result ? "ok" : "FAIL");
	return Result;
}

struct LineResult
{
	std::vector<VerseToken> Tokens;
	VerseLexState StateAfter;
};

std::vector<LineResult> LexAll(const std::vector<std::string>& Lines)
{
	std::vector<LineResult> Results;
	VerseLexState State;
	for (const std::string& Line : Lines)
	{
		LineResult R;
		verse_lex_line(Line, State, R.Tokens);
		R.StateAfter = State;
		Results.push_back(R);
	}
	return Results;
}

VerseTokenKind KindAt(const std::vector<VerseToken>& Tokens, int Col)
{
	VerseTokenKind Kind = VerseTokenKind::Text;
	for (const VerseToken& T : Tokens)
	{
		if (T.column > Col)
		{
			break;
		}
		Kind = T.kind;
	}
	return Kind;
}

bool HasKind(const std::vector<VerseToken>& Tokens, VerseTokenKind Kind)
{
	for (const VerseToken& T : Tokens)
	{
		if (T.kind == Kind)
		{
			return true;
		}
	}
	return false;
}

bool TestLineComment()
{
	auto Results = LexAll({ "X := 1 # a comment" });
	const auto& Tokens = Results[0].Tokens;
	bool Ok = KindAt(Tokens, 7) == VerseTokenKind::Comment;
	Ok = Ok && KindAt(Tokens, 5) == VerseTokenKind::Number;

	auto NotComment = LexAll({ "A #> B" });
	Ok = Ok && !HasKind(NotComment[0].Tokens, VerseTokenKind::Comment);

	return Step("line comment, and #> not starting one", Ok);
}

bool TestNestedBlockComment()
{
	auto Results = LexAll({
			"before <# outer",
			"<# nested #>",
			"end #> after",
	});

	bool Ok = Results[0].StateAfter.block_comment_depth == 1;
	Ok = Ok && Results[1].Tokens.size() == 1 && Results[1].Tokens[0].column == 0 && Results[1].Tokens[0].kind == VerseTokenKind::Comment;
	Ok = Ok && Results[1].StateAfter.block_comment_depth == 1;
	Ok = Ok && Results[2].StateAfter.block_comment_depth == 0;
	Ok = Ok && KindAt(Results[2].Tokens, 6) == VerseTokenKind::Text;

	return Step("nested block comment across three lines", Ok);
}

bool TestIndentCommentNotBlockComment()
{
	auto Results = LexAll({ "<#> indented" });
	bool Ok = Results[0].StateAfter.indent_comment_column == 0;
	Ok = Ok && Results[0].StateAfter.block_comment_depth == 0;
	Ok = Ok && Results[0].Tokens.size() == 1 && Results[0].Tokens[0].kind == VerseTokenKind::Comment;

	return Step("<#> is an indented comment, not a block comment", Ok);
}

bool TestIndentCommentDedent()
{
	auto Results = LexAll({
			"    <#> start",
			"        deeper one",
			"",
			"        deeper two",
			"    dedent now",
	});

	bool Ok = Results[0].StateAfter.indent_comment_column == 4;
	Ok = Ok && Results[1].Tokens.size() == 1 && Results[1].Tokens[0].kind == VerseTokenKind::Comment;
	Ok = Ok && Results[1].StateAfter.indent_comment_column == 4;
	Ok = Ok && Results[2].Tokens.empty();
	Ok = Ok && Results[2].StateAfter.indent_comment_column == 4;
	Ok = Ok && Results[3].Tokens.size() == 1 && Results[3].Tokens[0].kind == VerseTokenKind::Comment;
	Ok = Ok && Results[4].StateAfter.indent_comment_column == -1;
	Ok = Ok && KindAt(Results[4].Tokens, 4) == VerseTokenKind::Identifier;

	return Step("<#> comment ends at a dedent; a blank line does not end it", Ok);
}

bool TestHashInsideString()
{
	auto Results = LexAll({ "\"before # after\"" });
	const auto& Tokens = Results[0].Tokens;
	bool Ok = KindAt(Tokens, 1) == VerseTokenKind::String;
	Ok = Ok && KindAt(Tokens, 8) == VerseTokenKind::Comment;
	Ok = Ok && Results[0].StateAfter.in_string;

	return Step("# inside string content starts a comment, per the grammar", Ok);
}

bool TestBlockCommentInsideString()
{
	auto Results = LexAll({ "\"a <# cmt #> b\"" });
	const auto& Tokens = Results[0].Tokens;
	bool Ok = KindAt(Tokens, 1) == VerseTokenKind::String;
	Ok = Ok && KindAt(Tokens, 3) == VerseTokenKind::Comment;
	Ok = Ok && KindAt(Tokens, 13) == VerseTokenKind::String;
	Ok = Ok && !Results[0].StateAfter.in_string;
	Ok = Ok && Results[0].StateAfter.block_comment_depth == 0;

	return Step("<# #> closes inside a string and the string still closes", Ok);
}

bool TestInterpolation()
{
	auto Results = LexAll({ "\"sum is {A + {B}}\"" });
	const auto& Tokens = Results[0].Tokens;
	bool Ok = KindAt(Tokens, 8) == VerseTokenKind::Interpolation; // {
	Ok = Ok && KindAt(Tokens, 9) == VerseTokenKind::Identifier; // A
	Ok = Ok && KindAt(Tokens, 13) == VerseTokenKind::Interpolation; // nested {
	Ok = Ok && KindAt(Tokens, 14) == VerseTokenKind::Identifier; // B
	Ok = Ok && KindAt(Tokens, 15) == VerseTokenKind::Interpolation; // }}
	Ok = Ok && KindAt(Tokens, 17) == VerseTokenKind::String; // closing "
	Ok = Ok && !Results[0].StateAfter.in_string;
	Ok = Ok && Results[0].StateAfter.interpolation_depth == 0;

	return Step("{} interpolation highlights as code, including nested {}", Ok);
}

bool TestKeywordClassification()
{
	auto Results = LexAll({ "if class Foo" });
	const auto& Tokens = Results[0].Tokens;
	bool Ok = KindAt(Tokens, 0) == VerseTokenKind::ControlKeyword;
	Ok = Ok && KindAt(Tokens, 3) == VerseTokenKind::Keyword;
	Ok = Ok && KindAt(Tokens, 9) == VerseTokenKind::Identifier;

	return Step("keyword vs. control-flow keyword vs. plain identifier", Ok);
}

bool TestAttributes()
{
	auto Results = LexAll({ "Ready<public>():void =", "Foo<decides>" });
	bool Ok = KindAt(Results[0].Tokens, 5) == VerseTokenKind::Attribute;
	Ok = Ok && KindAt(Results[1].Tokens, 3) == VerseTokenKind::Attribute;

	return Step("<public> and <decides> are Attribute tokens", Ok);
}

bool TestSymbols()
{
	auto Results = LexAll({ "X := A + 1" });
	auto& Tokens = Results[0].Tokens;

	bool Ok = KindAt(Tokens, 0) == VerseTokenKind::Identifier; // X
	Ok = Ok && KindAt(Tokens, 2) == VerseTokenKind::Symbol; // :=
	Ok = Ok && KindAt(Tokens, 5) == VerseTokenKind::Identifier; // A
	Ok = Ok && KindAt(Tokens, 7) == VerseTokenKind::Symbol; // +
	Ok = Ok && KindAt(Tokens, 9) == VerseTokenKind::Number;

	return Step("operators and punctuation are symbols, not plain text", Ok);
}

bool TestCallsAndMembers()
{
	auto Results = LexAll({ "Print(P.X)", "if (Q := GetPosition[]):", "Ready<override>():void =" });

	bool Ok = KindAt(Results[0].Tokens, 0) == VerseTokenKind::Function; // Print
	Ok = Ok && KindAt(Results[0].Tokens, 6) == VerseTokenKind::Identifier; // P, not a call
	Ok = Ok && KindAt(Results[0].Tokens, 8) == VerseTokenKind::Member; // .X

	// A <decides> call is spelled with brackets, so those count as a call position too.
	Ok = Ok && KindAt(Results[1].Tokens, 9) == VerseTokenKind::Function; // GetPosition

	// A definition binds its name rather than calling it, and GDScript colours the two apart.
	Ok = Ok && KindAt(Results[2].Tokens, 0) == VerseTokenKind::FunctionDefinition; // Ready
	Ok = Ok && KindAt(Results[2].Tokens, 5) == VerseTokenKind::Attribute; // <override>

	return Step("call positions and member accesses colour apart from plain identifiers", Ok);
}

bool TestDefinitionsColourApartFromCalls()
{
	auto Results = LexAll({
			"\tBump():void =",
			"\t\tPrint(\"x = {P}\")",
			"\t\tSetPosition(GetPosition())",
			"\tGetter<epic_internal>(A:accessor)<transacts>:float = VhToFloat(V)",
			"\t\tif (Field = \"X\"):",
	});

	// No specifiers, so the `=` after the parameter list is the only thing marking a definition.
	bool Ok = KindAt(Results[0].Tokens, 1) == VerseTokenKind::FunctionDefinition; // Bump

	// A call whose argument holds an `=`, inside a string at that: the scan is already past the
	// closing bracket before it looks, so neither reaches it.
	Ok = Ok && KindAt(Results[1].Tokens, 2) == VerseTokenKind::Function; // Print
	Ok = Ok && KindAt(Results[2].Tokens, 2) == VerseTokenKind::Function; // SetPosition
	Ok = Ok && KindAt(Results[2].Tokens, 14) == VerseTokenKind::Function; // GetPosition, nested

	Ok = Ok && KindAt(Results[3].Tokens, 1) == VerseTokenKind::FunctionDefinition; // Getter
	Ok = Ok && KindAt(Results[3].Tokens, 54) == VerseTokenKind::Function; // VhToFloat, the body

	// `if` is the first token here, so the comparison cannot be read as a definition.
	Ok = Ok && KindAt(Results[4].Tokens, 2) == VerseTokenKind::ControlKeyword; // if

	return Step("a definition's name colours apart from a call to it", Ok);
}

bool TestPrefixAttributes()
{
	auto Results = LexAll({ "@editable", "@clamp_min(\"0.0\")" });

	bool Ok = KindAt(Results[0].Tokens, 0) == VerseTokenKind::Attribute;
	Ok = Ok && KindAt(Results[1].Tokens, 0) == VerseTokenKind::Attribute;
	// The argument list is ordinary code, not part of the attribute token.
	Ok = Ok && KindAt(Results[1].Tokens, 10) == VerseTokenKind::Symbol; // (
	Ok = Ok && KindAt(Results[1].Tokens, 11) == VerseTokenKind::String;

	// An email-ish `@` with no identifier after it stays a plain symbol.
	auto Bare = LexAll({ "A @ B" });
	Ok = Ok && KindAt(Bare[0].Tokens, 2) == VerseTokenKind::Symbol;

	return Step("@name is an attribute; a bare @ is not", Ok);
}

bool TestBareIdentifier()
{
	auto Results = LexAll({ "Foo" });
	bool Ok = KindAt(Results[0].Tokens, 0) == VerseTokenKind::Identifier;

	return Step("a bare identifier is an Identifier token", Ok);
}

bool TestIdentifierTokensDoNotMerge()
{
	// Regression test: emit() coalesces adjacent same-kind tokens, so before Identifier existed
	// this whole line was one Text run and "Bar"'s column was unrecoverable.
	auto Results = LexAll({ "Foo Bar" });
	const auto& Tokens = Results[0].Tokens;

	bool Ok = Tokens.size() == 3;
	Ok = Ok && Tokens[0].column == 0 && Tokens[0].kind == VerseTokenKind::Identifier;
	Ok = Ok && Tokens[1].column == 3 && Tokens[1].kind == VerseTokenKind::Text;
	Ok = Ok && Tokens[2].column == 4 && Tokens[2].kind == VerseTokenKind::Identifier;

	return Step("two space-separated identifiers stay two separate Identifier tokens", Ok);
}

bool TestIdentifierInsideCommentOrString()
{
	auto Results = LexAll({ "# foo bar", "\"foo bar\"" });

	bool Ok = HasKind(Results[0].Tokens, VerseTokenKind::Comment);
	Ok = Ok && !HasKind(Results[0].Tokens, VerseTokenKind::Identifier);
	Ok = Ok && HasKind(Results[1].Tokens, VerseTokenKind::String);
	Ok = Ok && !HasKind(Results[1].Tokens, VerseTokenKind::Identifier);

	return Step("identifier-shaped text inside a comment or string stays Comment/String", Ok);
}

bool TestPositionInComment()
{
	// Rows, in order: a trailing comment, an indented one, a string, a block comment over three
	// lines, and an indented comment with a blank line in its body.
	const std::string Source =
			"Foo := 1 # trailing\n"
			"\t# indented\n"
			"Print(\"text\")\n"
			"<# block\n"
			"still inside\n"
			"#> Bar\n"
			"<#>\n"
			"\tindented body\n"
			"\n"
			"Baz\n";

	struct Case
	{
		const char* What;
		int Row;
		int Column;
		bool Expect;
	};
	const Case Cases[] = {
		{ "code ahead of a trailing comment is not in one", 0, 4, false },
		{ "nor is the column its `#` sits at", 0, 9, false },
		{ "one column further is", 0, 10, true },
		{ "and so is the prose past it", 0, 11, true },
		{ "an indented comment's own column is code", 1, 1, false },
		{ "its body is not", 1, 5, true },
		{ "a string literal is not a comment", 2, 8, false },
		{ "the column a block comment opens at is code", 3, 0, false },
		{ "a line inside one is comment from column zero", 4, 0, true },
		{ "and past the `#>` that closed it is code again", 5, 4, false },
		{ "the body of an indented comment is comment", 7, 6, true },
		{ "a blank line inside one still is", 8, 0, true },
		{ "and a dedent ends it", 9, 2, false },
	};

	bool Ok = true;
	for (const Case& C : Cases)
	{
		Ok = Step(C.What, verse_position_in_comment(Source, C.Row, C.Column) == C.Expect) && Ok;
	}
	return Ok;
}

} // namespace

int main()
{
	bool Ok = true;
	Ok = TestLineComment() && Ok;
	Ok = TestNestedBlockComment() && Ok;
	Ok = TestIndentCommentNotBlockComment() && Ok;
	Ok = TestIndentCommentDedent() && Ok;
	Ok = TestHashInsideString() && Ok;
	Ok = TestBlockCommentInsideString() && Ok;
	Ok = TestInterpolation() && Ok;
	Ok = TestKeywordClassification() && Ok;
	Ok = TestAttributes() && Ok;
	Ok = TestSymbols() && Ok;
	Ok = TestCallsAndMembers() && Ok;
	Ok = TestDefinitionsColourApartFromCalls() && Ok;
	Ok = TestPrefixAttributes() && Ok;
	Ok = TestBareIdentifier() && Ok;
	Ok = TestIdentifierTokensDoNotMerge() && Ok;
	Ok = TestIdentifierInsideCommentOrString() && Ok;
	Ok = TestPositionInComment() && Ok;

	printf("[verse_lexer_test] %s\n", Ok ? "ALL PASS" : "FAILURES");
	return Ok ? 0 : 1;
}
