// Standalone driver for the doc-markup converter. No Godot and no godot-cpp, like the lexer and
// class-declaration tests beside it: what a comment becomes is decidable from its text alone.
// Each case is one rule, and the expected string is what Godot's `_add_text_to_rt` will read.
#include "verse_doc_markup.h"

#include <cstdio>
#include <string>

namespace {

bool Step(const char* Name, bool Result)
{
	printf("[verse_doc_markup_test] %s: %s\n", Name, Result ? "ok" : "FAIL");
	return Result;
}

bool Expect(const char* Name, const std::string& Doc, const std::string& Want)
{
	const std::string Got = verse_doc_to_bbcode(Doc);
	const bool Ok = Got == Want;
	if (!Ok)
	{
		printf("    want: %s\n    got:  %s\n", Want.c_str(), Got.c_str());
	}
	return Step(Name, Ok);
}

bool TestEmpty()
{
	return Expect("empty in, empty out", "", "")
		&& Expect("blank lines alone are nothing", "\n\n", "");
}

bool TestLinesJoinIntoParagraphs()
{
	// GDScript's rule: consecutive lines are one paragraph, a blank line separates two.
	return Expect("consecutive lines join with a space",
				"The first line\nand the second.", "The first line and the second.")
		&& Expect("a blank line is a paragraph",
				"First paragraph.\n\nSecond paragraph.", "First paragraph.\nSecond paragraph.")
		&& Expect("several blank lines are one paragraph break",
				"First.\n\n\n\nSecond.", "First.\nSecond.")
		&& Expect("leading and trailing blank lines are dropped",
				"\nOnly.\n", "Only.")
		&& Expect("a continuation line's indentation is not text",
				"A bullet that wraps\n  onto a second line.", "A bullet that wraps onto a second line.");
}

bool TestInlineCode()
{
	return Expect("a backtick span is [code]",
				"Call `Floor[X]` first.", "Call [code]Floor[X][/code] first.")
		&& Expect("inside a span nothing is escaped",
				"`[]char` is a string", "[code][]char[/code] is a string")
		&& Expect("an unclosed backtick is literal",
				"a ` alone", "a ` alone")
		&& Expect("two spans on one line",
				"`A` and `B`.", "[code]A[/code] and [code]B[/code].");
}

bool TestBracketsAreEscaped()
{
	// Verse spells a failable call and an array type with brackets, and Godot reads `[` as a tag.
	return Expect("a bracket in prose is escaped",
				"Floor[X] answers an int", "Floor[lb]X[rb] answers an int")
		&& Expect("an array type in prose is escaped",
				"[]int is an array", "[lb][rb]int is an array")
		&& Expect("a Verse index is not an italic tag",
				"Items[i] reads one", "Items[lb]i[rb] reads one");
}

bool TestGodotTagsPassThrough()
{
	return Expect("a Godot tag passes through",
				"This is [b]bold[/b] text", "This is [b]bold[/b] text")
		&& Expect("a link tag passes through",
				"See [method Node.add_child] and [param Delta].",
				"See [method Node.add_child] and [param Delta].")
		&& Expect("Godot's own escapes pass through",
				"a [lb]bracket[rb]", "a [lb]bracket[rb]")
		&& Expect("a [code] span written Godot's way is read raw",
				"[code]Floor[X][/code] here", "[code]Floor[X][/code] here")
		&& Expect("an unknown tag is escaped",
				"[node] is a class", "[lb]node[rb] is a class")
		&& Expect("a codeblock written Godot's way keeps its lines",
				"Sample:\n[codeblock]\nX := Floor[Y]\n[/codeblock]\nAfter.",
				"Sample:\n[codeblock]\nX := Floor[Y]\n[/codeblock]\nAfter.");
}

bool TestEmphasis()
{
	return Expect("double asterisks are bold",
				"It is **not** total.", "It is [b]not[/b] total.")
		&& Expect("single asterisks are italic",
				"the *active* scope", "the [i]active[/i] scope")
		&& Expect("a spaced asterisk is arithmetic",
				"0.5 * (A * 2.0)", "0.5 * (A * 2.0)")
		&& Expect("an asterisk inside a word is arithmetic",
				"2*x and x*2", "2*x and x*2")
		&& Expect("code inside bold is still code",
				"**`X` wins**", "[b][code]X[/code] wins[/b]")
		&& Expect("an unclosed asterisk is literal",
				"*starts and never ends", "*starts and never ends");
}

bool TestIndentedBlock()
{
	// The shape the mirror's own comments use: prose, a blank line, four-space-indented sample.
	return Expect("an indented block after a blank line is a code block",
				"So spelled:\n\n    player := class(area2d):\n        Hit<public>:signal() = signal(){}\n\nAfter.",
				"So spelled:\n[codeblock lang=verse]\nplayer := class(area2d):\n    Hit<public>:signal() = signal(){}\n[/codeblock]\nAfter.")
		&& Expect("a block's brackets are not escaped",
				"Sample:\n\n    X := Floor[Y]",
				"Sample:\n[codeblock lang=verse]\nX := Floor[Y]\n[/codeblock]")
		&& Expect("a blank line inside a block stays inside it",
				"Sample:\n\n    A := 1\n\n    B := 2\n\nAfter.",
				"Sample:\n[codeblock lang=verse]\nA := 1\n\nB := 2\n[/codeblock]\nAfter.")
		&& Expect("a tab indents a block too",
				"Sample:\n\n\tA := 1\n\t\tB := 2",
				"Sample:\n[codeblock lang=verse]\nA := 1\n\tB := 2\n[/codeblock]")
		&& Expect("an indented line with no blank line before it is a continuation",
				"A sentence that\n    wraps indented.", "A sentence that wraps indented.")
		&& Expect("a block may begin the text",
				"    X := 1\n\nProse.", "[codeblock lang=verse]\nX := 1\n[/codeblock]\nProse.");
}

bool TestFencedBlock()
{
	return Expect("a fenced block is a code block",
				"Sample:\n```\nX := Floor[Y]\n```\nAfter.",
				"Sample:\n[codeblock lang=verse]\nX := Floor[Y]\n[/codeblock]\nAfter.")
		&& Expect("a fence may name its language",
				"```gdscript\nvar x = 1\n```", "[codeblock lang=gdscript]\nvar x = 1\n[/codeblock]")
		&& Expect("an unclosed fence runs to the end",
				"Sample:\n```\nX := 1", "Sample:\n[codeblock lang=verse]\nX := 1\n[/codeblock]")
		&& Expect("a fenced block is dedented by what its lines share",
				"```\n  A := 1\n    B := 2\n```", "[codeblock lang=verse]\nA := 1\n  B := 2\n[/codeblock]");
}

bool TestLists()
{
	return Expect("a bullet breaks the line rather than joining",
				"Two things:\n- the first\n- the second",
				"Two things:[br]- the first[br]- the second")
		&& Expect("a numbered item breaks the line too",
				"Steps:\n1. build\n2. run", "Steps:[br]1. build[br]2. run");
}

bool TestBrief()
{
	return Step("the brief is the first paragraph",
				verse_doc_brief("First.\nSecond.") == "First.")
		&& Step("a one-paragraph description is its own brief",
				verse_doc_brief("Only.") == "Only.")
		&& Step("a code block never begins a brief",
				verse_doc_brief("[codeblock lang=verse]\nX := 1\n[/codeblock]\nProse.").empty())
		&& Step("a newline inside a code block does not end the brief",
				verse_doc_brief("See [codeblock]\nX := 1\n[/codeblock] here.\nSecond.")
					== "See [codeblock]\nX := 1\n[/codeblock] here.");
}

bool TestCarriageReturns()
{
	return Expect("CRLF input reads as LF", "First\r\n\r\nSecond", "First\nSecond");
}

} // namespace

int main()
{
	const bool Ok = TestEmpty()
		&& TestLinesJoinIntoParagraphs()
		&& TestInlineCode()
		&& TestBracketsAreEscaped()
		&& TestGodotTagsPassThrough()
		&& TestEmphasis()
		&& TestIndentedBlock()
		&& TestFencedBlock()
		&& TestLists()
		&& TestBrief()
		&& TestCarriageReturns();

	printf("[verse_doc_markup_test] %s\n", Ok ? "all checks passed" : "FAILURES");
	return Ok ? 0 : 1;
}
