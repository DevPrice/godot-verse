#include "verse_debugger.h"

#include "verse_diagnostic_prose.h"
#include "verse_runtime.h"
#include "verse_script_language.h"

#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/engine_debugger.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

using namespace godot;

namespace {

VerseRuntime *get_runtime() {
	return Object::cast_to<VerseRuntime>(Engine::get_singleton()->get_singleton("VerseRuntime"));
}

} // namespace

String VerseDebugger::get_error() const {
	return break_reason;
}

int32_t VerseDebugger::get_stack_level_count() const {
	VerseRuntime *runtime = get_runtime();
	return runtime != nullptr ? runtime->debug_stack_count() : 0;
}

int32_t VerseDebugger::get_stack_level_line(int32_t p_level) const {
	VerseRuntime *runtime = get_runtime();
	if (runtime == nullptr) {
		return 0;
	}
	const Dictionary frame = runtime->debug_stack_frame(p_level);
	return frame.has("line") ? (int32_t)(int64_t)frame["line"] : 0;
}

String VerseDebugger::get_stack_level_function(int32_t p_level) const {
	VerseRuntime *runtime = get_runtime();
	if (runtime == nullptr) {
		return String();
	}
	const Dictionary frame = runtime->debug_stack_frame(p_level);
	return frame.has("function") ? String(frame["function"]) : String();
}

String VerseDebugger::get_stack_level_source(int32_t p_level) const {
	VerseRuntime *runtime = get_runtime();
	if (runtime == nullptr) {
		return String();
	}
	const Dictionary frame = runtime->debug_stack_frame(p_level);
	const String source = frame.has("source") ? String(frame["source"]) : String();
	if (source.is_empty()) {
		// A native frame. Epic's stack walk emits one with a name and no location, which is what
		// makes a stop inside a mirrored method legible rather than a hole in the stack.
		return String();
	}
	return res_path_for_source(source);
}

// The extension wrapper splits the dictionary on these two keys and no others
// (script_language_extension.h), so the key is the contract rather than a convention.
Dictionary VerseDebugger::get_stack_level_locals(int32_t p_level, int32_t p_max_subitems, int32_t p_max_depth) {
	return debug_values_at(p_level, VH_DEBUG_LOCALS, "locals");
}

Dictionary VerseDebugger::get_stack_level_members(int32_t p_level, int32_t p_max_subitems, int32_t p_max_depth) {
	// Self and its fields. This is the only place a script instance's state appears -- see
	// get_stack_level_instance.
	return debug_values_at(p_level, VH_DEBUG_MEMBERS, "members");
}

Dictionary VerseDebugger::debug_values_at(int32_t p_level, int32_t p_kind, const char *p_key) const {
	VerseRuntime *runtime = get_runtime();
	if (runtime == nullptr) {
		return Dictionary();
	}
	const Dictionary values = runtime->debug_stack_values(p_level, p_kind);
	if (!values.has("names")) {
		return Dictionary();
	}
	Dictionary result;
	result[String(p_key)] = values["names"];
	result["values"] = values["values"];
	return result;
}

// Null, permanently, and this is a landmine rather than a stub.
//
// Godot calls `inst->get_owner()` on whatever comes back (remote_debugger.cpp) -- a C++ virtual on
// a ScriptInstance*. This extension's instances are raw GDExtensionScriptInstanceInfo3 vtables; the
// ScriptInstanceExtension wrapper that *is* a ScriptInstance is built by Godot core and its address
// never reaches a GDExtension. Returning a vh_instance* or a VerseScriptInstance* here is a
// type-confused virtual call and a crash.
//
// Two things follow. `self` is delivered through get_stack_level_members instead, and
// expression evaluation is unreachable: Godot's `evaluate` command bails before it would ask us.
void *VerseDebugger::get_stack_level_instance(int32_t p_level) {
	return nullptr;
}

// Honestly empty, not a stub. A Verse module-level definition is a constant, not a mutable global
// a debugger would watch change, so there is nothing for this to answer with. It is bound REQUIRED,
// so it cannot be omitted the way an unsupported EXBIND virtual can.
Dictionary VerseDebugger::get_globals(int32_t p_max_subitems, int32_t p_max_depth) {
	return Dictionary();
}

TypedArray<Dictionary> VerseDebugger::get_current_stack_info() {
	return TypedArray<Dictionary>();
}

// gdscript_vm.cpp's order, and diverging from it is what makes stepping and breakpoints disagree
// about which one fires: a pending step wins, then a breakpoint, then the poll runs whatever the
// answer was.
//
// The one substitution is depth. GDScript keeps EngineDebugger's counter honest by pushing and
// popping it around every call; this bridge sees no Verse call, only an op, so the counter would
// never move and step-over would behave as step-in. p_relation is the same question asked of the
// stack instead -- Epic's own frame-ancestry test, which is the mechanism Notify is given the
// arguments for -- and Godot's depth stays exactly what Godot set it to.
//
// is_skipping_breakpoints needs no handling: RemoteDebugger::debug checks it itself and returns
// without stopping, so asking here would only duplicate the check.
bool VerseDebugger::should_break(const String &p_path, int32_t p_line, int32_t p_relation) {
	EngineDebugger *debugger = EngineDebugger::get_singleton();
	if (debugger == nullptr || !debugger->is_active()) {
		return false;
	}

	const String source = res_path_for_source(p_path);

	bool do_break = false;
	const int32_t lines_left = debugger->get_lines_left();
	if (lines_left > 0) {
		// A step that lands where it started has not stepped. See stopped_line's declaration.
		const bool moved = p_relation != VH_DEBUG_FRAME_SAME || p_line != stopped_line || source != stopped_source;
		const int32_t depth = debugger->get_depth();
		const bool eligible = moved &&
				(depth < 0 // step in: anywhere
						|| (depth == 0 && p_relation != VH_DEBUG_FRAME_DEEPER) // next: not inside a call from here
						|| (depth > 0 && p_relation == VH_DEBUG_FRAME_OTHER)); // out: neither here nor deeper
		if (eligible) {
			debugger->set_lines_left(lines_left - 1);
			if (lines_left - 1 <= 0) {
				do_break = true;
				break_reason = "Step";
			}
		}
	}
	if (debugger->is_breakpoint(p_line, source)) {
		do_break = true;
		break_reason = "Breakpoint";
	}
	debugger->line_poll();

	if (do_break) {
		stopped_source = source;
		stopped_line = p_line;
	}
	return do_break;
}

// Blocks on the calling thread, which is the Verse interpreter's, and does not return until the
// user continues. Godot's debug loop runs here and calls back through the _debug_* virtuals below
// while it does -- which is the shape the whole design is forced into: RemoteDebugger::debug loops
// on the thread that called it, so there is no arrangement where the host parks on a primitive of
// its own and Godot still runs.
void VerseDebugger::break_here() {
	EngineDebugger *debugger = EngineDebugger::get_singleton();
	if (debugger == nullptr) {
		return;
	}
	debugger->script_debug(&language, true, false);
}

// Attach whenever Godot's debugger is active, which is unconditionally correct and is what D6 asks
// for until a measurement says otherwise. The alternative -- a polled mirror that sweeps
// is_breakpoint over the lines Verse reports and attaches only when one exists -- buys back the
// per-op Notify at the cost of a breakpoint that arms on a delay, and S-2's number did not justify
// it (phase-6-design.md 13.1).
void VerseDebugger::sync_attachment() {
	VerseRuntime *runtime = get_runtime();
	EngineDebugger *debugger = EngineDebugger::get_singleton();
	if (runtime == nullptr || debugger == nullptr) {
		return;
	}
	const bool wanted = debugger->is_active();
	if (wanted == debugger_attached) {
		return;
	}
	// A refusal means Epic's socket debugger already owns the VM's one debugger slot, which is
	// what verse/host/enable_debugger asked for. Said once rather than every frame.
	if (!runtime->debug_set_enabled(wanted)) {
		if (wanted) {
			UtilityFunctions::push_warning(String("Verse: ") + verse_diagnostic(verse_diag::VG6001));
		}
		debugger_attached = wanted; // do not ask again every frame
		return;
	}
	debugger_attached = wanted;
}

// The host names a script by the absolute path vh_compile_project was given, verbatim -- which on
// Windows is a mix of separators, because that is what the consumer built the list out of. Godot's
// breakpoint list is keyed by res:// path, so this is the whole of the translation, and it is
// cached because the question is asked once per distinct location per frame.
String VerseDebugger::res_path_for_source(const String &p_path) const {
	const std::string key(p_path.utf8().get_data());
	const auto found = res_path_by_source.find(key);
	if (found != res_path_by_source.end()) {
		return found->second;
	}
	const String localized = ProjectSettings::get_singleton()->localize_path(p_path.replace("\\", "/"));
	res_path_by_source[key] = localized;
	return localized;
}
