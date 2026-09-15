#pragma once

#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>

// Where an exported game keeps the Verse it ships: cooked packages, the class sidecar, and the
// Engine/ subtree the host boots against. Shared by the export plugin, which creates the
// directory, and VerseRuntime, which finds it again at runtime -- one rule, so the two cannot
// name different places.
//
// The shipped directory is plainly `verse_data`, beside the executable: it is already inside one
// game's own directory, so the app, platform and architecture that .NET's `data_<app>_<platform>_<arch>`
// carries (godotsharp_dirs.cpp:224-231) would say nothing there. Godot resolves it from the
// executable path at startup, which is why nothing has to be configured in an exported build (D8).
//
// The *cache* directory the cooker writes into still carries all three, because every project and
// platform on the machine lands beside every other one there. The shipped name is the cache
// directory's leaf for a reason that is not cosmetic: `add_shared_object` copies a directory under
// its own name and takes no rename (`editor_export_platform_pc.cpp:244`), so the only way to ship
// `verse_data` is for the qualified name to be its parent.
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

// What the shipped directory is called, and so also the leaf the cooker writes into.
extern const char *DATA_DIR_NAME;

// `verse_<app>_<platform>_<arch>` -- the per-project, per-platform directory under the user's
// cache that holds one cook. p_app is the project name; it is sanitised here the way
// OS::get_safe_dir_name would, since that is not bound for extensions.
godot::String cache_dir_name(const godot::String &p_app, const godot::String &p_platform, const godot::String &p_arch);

// Absolute path to that directory beside the running executable, or "" outside an exported
// build. Nothing here checks that it exists -- a missing one is a diagnostic vh_init owes.
godot::String data_dir_for_this_build();

} // namespace verse_paths
