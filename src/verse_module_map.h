#pragma once

#include <map>
#include <string>
#include <vector>

// Which module each of a project's `.verse` files belongs to.
//
// A directory is a module only if it carries a `<name>.vmodule` marker, and the marker names the
// module -- not the directory. Everything else is organisational: a file joins its nearest marked
// ancestor, and a project with no markers has every file in the root module, which is why nothing
// on disk changes meaning when this lands.
//
// Epic's toolchain supports the other model too -- every directory is a module, from the tree,
// with no declaration (`CSourceFilePackage::ResolveModuleForRelativeVersePath`) -- and this is
// deliberately not it. `res://` is an asset tree whose directory names were chosen for sprites and
// scenes: under that rule `res://2d/` and `res://my-stuff/` are hard errors in a project that has
// done nothing wrong. The marker means the identifier rule applies only to names an author
// deliberately chose, which is the only place a diagnostic about one is actionable.
//
// No godot-cpp here, like verse_lexer and verse_class_decl: every rule above is a case in a test
// that needs neither Godot nor UE. Godot contributes only the two enumerations.

struct VerseModuleDiagnostic {
	// The `.vmodule` file the complaint is about, `res://`-relative.
	std::string marker_path;
	std::string message;
};

struct VerseModuleMap {
	// Module path per source file, keyed by the source path exactly as it was handed in.
	// "" is the root module; otherwise a '/'-separated path of Verse identifiers.
	std::map<std::string, std::string> module_by_source;

	// Every directory contributing to each module, keyed by module path. A module can be spread
	// across more than one directory, because two markers with the same stem name one module --
	// `FindOrAddSubmodule` is find-*or*-add, so that is what the toolchain already does. The
	// collision diagnostic for two same-named definitions needs to be able to name all of them.
	std::map<std::string, std::vector<std::string>> directories_by_module;

	// A marker whose stem is not a Verse identifier, and so names no module. Its directory falls
	// back to the nearest marked ancestor, which keeps the rest of the project building.
	std::vector<VerseModuleDiagnostic> diagnostics;
};

// p_source_paths and p_marker_paths are `res://`-relative, '/'-separated, and neither order nor
// the presence of a leading "res://" matters -- a leading "res://" is stripped if present.
VerseModuleMap verse_build_module_map(const std::vector<std::string> &p_source_paths,
		const std::vector<std::string> &p_marker_paths);

// Whether p_name is spellable as a Verse module name: ASCII `[A-Za-z_][A-Za-z0-9_]*`, which is
// `CSourceFilePackage::IsValidModuleName`'s rule.
bool verse_is_valid_module_name(const std::string &p_name);
