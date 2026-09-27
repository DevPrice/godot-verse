#pragma once

#include <godot_cpp/classes/script_language_extension.hpp>

#include <cstdint>

// The profiler: boundary instrumentation plus `profile{}` blocks, not a sampler -- a sampler
// cannot produce a call count. Owned by VerseScriptLanguage as a mutable member, the way
// VerseProjectState and VerseDebugger are; every `_profiling_*` virtual stays declared on
// VerseScriptLanguage with a one-line body delegating here, for the same reason theirs do.
class VerseProfiler {
public:
	void start();
	void stop();
	// A no-op, and it stays one. Godot's "save native calls" asks a language to attribute time
	// spent inside engine calls to the script that made them; the bridge already does that and
	// cannot do otherwise -- a mirrored method call happens inside the Verse method's boundary
	// row, so its time is in that row's total whether anyone asks for it or not.
	void set_save_native_calls(bool p_enable) {}

	int32_t get_accumulated_data(godot::ScriptLanguageExtensionProfilingInfo *p_info_array, int32_t p_info_max);
	int32_t get_frame_data(godot::ScriptLanguageExtensionProfilingInfo *p_info_array, int32_t p_info_max);

private:
	// Whether Godot's profiler has asked for rows. Held here as well as in the host so that
	// start() on a session with no host loaded is still a no-op rather than a crash.
	bool profiling_active = false;

	// Copies the host's rows into the array Godot allocated. Not a loop over p_info_array[i]:
	// see the definition for why the stride is not sizeof.
	int32_t fill_profiling_info(godot::ScriptLanguageExtensionProfilingInfo *p_info_array, int32_t p_info_max, bool p_frame_only);
};
