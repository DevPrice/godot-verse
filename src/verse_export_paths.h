#pragma once

#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>

// Where an exported game keeps the Verse it ships: cooked packages, the class sidecar, and the
// Engine/ subtree the host boots against. Shared by the export plugin, which creates the
// directory, and VerseRuntime, which finds it again at runtime -- one rule, so the two cannot
// name different places.
//
// The layout is .NET's, exactly: `data_<app>_<platform>_<arch>` beside the executable
// (ExportPlugin.cs:250-262, godotsharp_dirs.cpp:224-231), with `verse_` in place of `data_`.
// Godot resolves it from the executable path at startup, which is why nothing has to be
// configured in an exported build (D8).
//
// Both halves read *feature tags*, not OS calls, because the editor knows an export's platform
// only as a tag and godot-cpp binds neither `OS::get_identifier` nor `OS::get_safe_dir_name`.

namespace verse_paths {

// "windows", "linuxbsd", "macos", ... -- Godot's own platform identifiers, which are also its
// platform feature tags. Empty when the tags name none of them.
godot::String platform_tag(const godot::PackedStringArray &p_features);

// "x86_64", "arm64", ... Empty when the tags name no architecture, which is what a macOS
// universal build looks like.
godot::String arch_tag(const godot::PackedStringArray &p_features);

// `verse_<app>_<platform>_<arch>`. p_app is the project name; it is sanitised here the way
// OS::get_safe_dir_name would, since that is not bound for extensions.
godot::String data_dir_name(const godot::String &p_app, const godot::String &p_platform, const godot::String &p_arch);

// The same name, for the running game: the feature tags are this build's own. Empty outside an
// exported build (`template` is the tag every export template carries and no editor does).
godot::String data_dir_name_for_this_build();

// Absolute path to that directory beside the running executable, or "" outside an exported
// build. Nothing here checks that it exists -- a missing one is a diagnostic vh_init owes.
godot::String data_dir_for_this_build();

} // namespace verse_paths
