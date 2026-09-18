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

// The BBCode for p_doc. Empty in, empty out.
std::string verse_doc_to_bbcode(const std::string &p_doc);

// The first paragraph of a converted description, which is what Godot draws as a class's
// `brief_description`. A code block never begins a brief.
std::string verse_doc_brief(const std::string &p_bbcode);
