#include "verse_profiler.h"

#include "verse_runtime.h"

#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/variant/typed_array.hpp>

using namespace godot;

namespace {

VerseRuntime *get_runtime() {
	return Object::cast_to<VerseRuntime>(Engine::get_singleton()->get_singleton("VerseRuntime"));
}

} // namespace

void VerseProfiler::start() {
	profiling_active = true;
	VerseRuntime *runtime = get_runtime();
	if (runtime != nullptr) {
		runtime->profiling_set_enabled(true);
	}
}

void VerseProfiler::stop() {
	profiling_active = false;
	VerseRuntime *runtime = get_runtime();
	if (runtime != nullptr) {
		runtime->profiling_set_enabled(false);
	}
}

// **The array is not laid out the way godot-cpp thinks it is.**
//
// Godot's ScriptLanguage::ProfilingInfo carries a fifth field -- `internal_time`, added in 4.3 --
// that its GDREGISTER_NATIVE_STRUCT registration string does not mention
// (core/register_core_types.cpp against core/object/script_language.h). godot-cpp generates its
// struct from that string, so it is 32 bytes for an array whose real elements are 40, and indexing
// past element zero writes into the wrong offsets and eventually past the end.
//
// So the stride comes from the engine's version rather than from sizeof. Element zero is at the
// same address under either layout, which is the only reason getting this wrong would ever have
// been survivable; every element after it would not have been.
static size_t profiling_info_stride() {
	const Dictionary version = Engine::get_singleton()->get_version_info();
	const int64_t major = version.get("major", 4);
	const int64_t minor = version.get("minor", 0);
	const bool has_internal_time = major > 4 || (major == 4 && minor >= 3);
	return sizeof(ScriptLanguageExtensionProfilingInfo) + (has_internal_time ? sizeof(uint64_t) : 0);
}

int32_t VerseProfiler::fill_profiling_info(ScriptLanguageExtensionProfilingInfo *p_info_array, int32_t p_info_max, bool p_frame_only) {
	VerseRuntime *runtime = get_runtime();
	if (p_info_array == nullptr || p_info_max <= 0 || runtime == nullptr || !profiling_active) {
		return 0;
	}

	const TypedArray<Dictionary> rows = runtime->profiling_read(p_frame_only);
	const size_t stride = profiling_info_stride();
	int32_t written = 0;
	for (int64_t i = 0; i < rows.size() && written < p_info_max; i++) {
		const Dictionary row = rows[i];
		ScriptLanguageExtensionProfilingInfo *slot = reinterpret_cast<ScriptLanguageExtensionProfilingInfo *>(
				reinterpret_cast<uint8_t *>(p_info_array) + stride * (size_t)written);
		// The signature is already constructed -- Godot resized the array before handing it over --
		// so this is assignment, not placement. internal_time is left as Godot zeroed it: the
		// bridge has no separate "time inside engine calls" to report, because a mirrored call
		// happens inside the Verse method's own row and is already in that row's total.
		slot->signature = StringName(String(row["signature"]));
		slot->call_count = (uint64_t)(int64_t)row["call_count"];
		slot->total_time = (uint64_t)(int64_t)row["total_time"];
		slot->self_time = (uint64_t)(int64_t)row["self_time"];
		written++;
	}
	return written;
}

int32_t VerseProfiler::get_accumulated_data(ScriptLanguageExtensionProfilingInfo *p_info_array, int32_t p_info_max) {
	return fill_profiling_info(p_info_array, p_info_max, false);
}

int32_t VerseProfiler::get_frame_data(ScriptLanguageExtensionProfilingInfo *p_info_array, int32_t p_info_max) {
	return fill_profiling_info(p_info_array, p_info_max, true);
}
