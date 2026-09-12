#pragma once

#include <string>

// The top-level class a .verse file declares, read out of the source text.
//
// Text rather than the semantic program because the one consumer, Godot's global-class registry,
// asks from EditorFileSystem's scan thread and asks before anything has been built -- and neither
// is something the host ABI can answer. See VerseScriptLanguage::_get_global_class_name.
struct VerseClassDecl {
	// Empty when the file declares no top-level class. The Verse name verbatim: it is what a
	// sibling script would `using`, and what Godot registers.
	std::string name;
	// The first superclass, e.g. "node2d". Empty for `class:` with no supers.
	std::string base;
	bool is_global = false;
	bool is_abstract = false;
	// `@tool`: Ready and Process run in the editor too. C#'s [Tool], and Godot asks the same
	// question of every language through Script::is_tool.
	bool is_tool = false;
	// Zero-based row the declaration is on, or -1 when there is none. The comment block above it
	// is the class' documentation, and finding that block needs the row.
	int line = -1;
};

// Scans for a top-level `name := class(base):` and the attributes directly above it. Comments and
// string literals are skipped by the real lexer, so `@global_class` inside either is not mistaken
// for the attribute.
//
// p_file_stem is the file's own name without its extension, and it is what picks the class out: a
// file may declare any number of top-level names and only the one named after the file is the
// script. Pass it empty to take the first top-level class instead, which is what a caller with no
// file in hand -- a test, a scratch buffer -- wants.
VerseClassDecl verse_scan_class_decl(const std::string &p_source, const std::string &p_file_stem = std::string());

// The Godot type name for a Verse class name: `player_controller` becomes `PlayerController`.
//
// Godot's global class names share one namespace with the engine's own PascalCase types, so the
// Verse spelling both reads wrong beside them in the class picker and is what collides there.
// The Verse source keeps its own name -- vh_instantiate is still handed the file stem.
//
// Words split on underscores, plus one narrow rule for Godot's dimensional suffix: a `d` closing
// a word with a digit before it uppercases, so `enemy2d` becomes `Enemy2D` the way `Node2D` and
// `Camera3D` are spelled. Nothing else about digits is special -- `add` and `vector2i` are left
// exactly as they are.
std::string verse_pascal_case(const std::string &p_verse_name);

// The inverse split: `MyTestMethod` becomes `my_test_method`, the way Godot's own methods are
// spelled and the way tools/gen_verse_api.py's verse_class_name() already lowers Godot's PascalCase
// class names for the Verse side (the one other place this repo does this transform -- kept as the
// reference for the word-boundary rule: a word starts at a lower-to-upper transition, or an
// upper-to-upper transition immediately followed by a lowercase letter).
//
// Not exactly the inverse of verse_pascal_case: a run of capitals from an acronym (`HP2`) collapses
// its case information going this way and verse_pascal_case cannot recover it, so a name is never
// round-tripped through both -- a lookup goes through an explicit name map built by applying this to
// every declared name once, not by guessing a PascalCase name back from what this returns.
std::string verse_snake_case(const std::string &p_verse_name);
