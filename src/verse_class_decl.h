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
	// Zero-based row the declaration is on, or -1 when there is none. The comment block above it
	// is the class' documentation, and finding that block needs the row.
	int line = -1;
};

// Scans for the first top-level `name := class(base):` and the attributes directly above it.
// Comments and string literals are skipped by the real lexer, so `@global_class` inside either
// is not mistaken for the attribute.
VerseClassDecl verse_scan_class_decl(const std::string &p_source);

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
