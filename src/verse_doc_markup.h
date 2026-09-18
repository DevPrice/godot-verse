#pragma once

#include <string>

// The prose above a Verse declaration, turned into the BBCode Godot's documentation renderer reads.
//
// A comment in this repository -- the mirror's, the bridge's own and a script's -- is written the
// way a Markdown reader would expect: backticks for code, a blank line between paragraphs, an
// indented or fenced block for a sample, `**bold**` and `*emphasis*`. Godot reads none of that.
// `_add_text_to_rt` (editor/doc/editor_help.cpp) understands `[code]`, `[codeblock]`, `[b]`,
// `[i]`, `[br]`, `[lb]`, `[rb]` and a handful of link tags, and hands everything else to
// RichTextLabel::add_text, where every `\n` begins a paragraph. So a description handed over as
// the reader produced it drew one paragraph per comment line, printed its backticks, and parsed
// `Floor[X]` as a tag.
//
// The join follows GDScript's own doc-comment parser (`_process_doc_line`, gdscript_parser.cpp):
// consecutive lines join with a space, a blank line is a paragraph, a code block keeps its lines
// and rejoins with a newline. A tag Godot's own documentation would accept -- `[b]`, `[method
// Node.add_child]`, `[param Delta]` -- passes through, so an author who knows GDScript's spelling
// can use it; any other `[` is escaped, because Verse spells a failable call and an array type
// with brackets and neither is a tag.
//
// No godot-cpp, like the lexer and the class-declaration scanner: every rule is a case in
// tests/verse_doc_markup, and both sides of the ABI produce the same input shape (a line per
// comment line, the delimiter and one space taken off, indentation kept, joined with `\n`).

// The comment block immediately above line p_line (0-based) of p_source, as prose: every
// delimiter taken off, joined with `\n`, empty when nothing documents that line.
//
// Verse has no doc-comment form of its own, so this is the whole convention, and it is the one
// Epic's tooling follows: the digest generator rewrites a `@doc("...")` attribute into `#` lines
// above the declaration, and VerseJsonInterfaceGen writes a definition's prefix comments as its
// documentation. Whatever comment precedes a definition documents it.
//
// Read with the lexer rather than by line prefix, because two of Verse's three comment forms
// span lines: a `<# ... #>` block's inner lines carry no delimiter, and a `<#>` comment's body is
// whatever is indented under it. Walking up by prefix alone read a multi-line block as `>` -- the
// closing `#>` stripped to that, and the line above it ending the walk.
//
// The rules, each a case in tests/verse_doc_markup:
//   - a `#` line contributes what follows the `#` and one space, indentation kept, so an indented
//     sample is still a code block to verse_doc_to_bbcode;
//   - a `<# ... #>` block contributes the text between its delimiters, its lines dedented by what
//     they share, leading and trailing blank lines dropped;
//   - a `<#>` comment contributes what follows the marker on its line and the body indented under
//     it, dedented the same way;
//   - an attribute line between the comment and the declaration is stepped over: the prose for
//     `@global_class mover` or an `@export` member sits above the attribute clause;
//   - a blank line or a line of code ends the walk. A comment trailing code on the line above
//     documents nothing, and neither does one separated by a blank line.
//
// The host's DocOf reads the parser's own comment nodes by the same rules, so a consumer cannot
// tell which side produced a description.
std::string verse_doc_comment_above(const std::string &p_source, int p_line);

// The BBCode for p_doc. Empty in, empty out.
std::string verse_doc_to_bbcode(const std::string &p_doc);

// The first paragraph of a converted description, which is what Godot draws as a class's
// `brief_description`. A code block never begins a brief.
std::string verse_doc_brief(const std::string &p_bbcode);
