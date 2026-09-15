#pragma once

#include <godot_cpp/variant/string.hpp>

// Where this machine's Unreal checkout is, and the two binaries under it the editor needs: the
// host it loads and the cooker an export runs.
//
// Three stores, one order, and the order is the whole point: an environment variable, then the
// user's EditorSettings, then the project's own settings. Only the last of those travels in a
// committed file, and it is read for projects that predate this and never written -- these paths
// name one machine, so a project.godot carrying them is somebody's local state in everybody
// else's repository (R-DIST-12).
//
// An exported game reads none of it: it derives everything from its own executable (D8 of
// phase-7-design.md), which is why all of this is editor-side.

namespace verse_host_paths {

// The Unreal checkout root -- the directory holding `Engine/`, which is what UE_ROOT names
// everywhere else in this repository. "" when nothing says.
godot::String engine_dir();

// The editor host to load. VERSE_HOST_DLL, then the two `verse/host/dll_path` settings, then
// derived from engine_dir(). "" when nothing names one and engine_dir() is empty.
godot::String host_dll();

// The cooker an export runs, on the same terms: VERSE_COOKER, the two `verse/host/cooker_path`
// settings, then beside host_dll().
godot::String cooker_exe();

// One sentence naming what to set and where, for the caller that found nothing.
godot::String unconfigured_message();

// Declares the three entries so they appear in Editor Settings with the right pickers. Call once
// from the editor plugin; reading works whether or not this has run, because an unregistered
// name simply fails has_setting and falls through to the next store.
void register_editor_settings();

} // namespace verse_host_paths
