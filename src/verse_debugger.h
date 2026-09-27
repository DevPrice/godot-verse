#pragma once

#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/typed_array.hpp>

#include <cstdint>
#include <string>
#include <unordered_map>

class VerseScriptLanguage;

// R-DIAG-4. A breakpoint in Godot's script editor stops a Verse script and shows its locals.
//
// The division with the host is: the host owns *which frame*, because only the Verse
// interpreter's Notify can see one; this owns *which line*, because the breakpoint list and the
// step state are Godot's. Everything below answers from what the host stashed at the stop, and
// is defined only while one is on the stack.
//
// Owned by VerseScriptLanguage as a mutable member, the way VerseProjectState is: every one of
// Godot's `_debug_*` virtuals stays declared and bound on VerseScriptLanguage (godot-cpp binds a
// virtual with Godot only when the subclass declares it, so this class must never gain one of its
// own), and each virtual's body is a one-line delegation to the method here of the same shape.
class VerseDebugger {
public:
	explicit VerseDebugger(VerseScriptLanguage &p_language) :
			language(p_language) {}

	godot::String get_error() const;
	int32_t get_stack_level_count() const;
	int32_t get_stack_level_line(int32_t p_level) const;
	godot::String get_stack_level_function(int32_t p_level) const;
	godot::String get_stack_level_source(int32_t p_level) const;
	godot::Dictionary get_stack_level_locals(int32_t p_level, int32_t p_max_subitems, int32_t p_max_depth);
	godot::Dictionary get_stack_level_members(int32_t p_level, int32_t p_max_subitems, int32_t p_max_depth);
	// Two of these stay empty on purpose and the audit in phase-6-design.md §7 says why:
	// get_stack_level_instance can never be anything but null (Godot calls a C++ virtual on
	// what it returns, and a GDExtension script instance is not a ScriptInstance), and Verse has no
	// mutable globals for get_globals to answer with.
	void *get_stack_level_instance(int32_t p_level);
	godot::Dictionary get_globals(int32_t p_max_subitems, int32_t p_max_depth);
	godot::TypedArray<godot::Dictionary> get_current_stack_info();

	// The consumer half of R-DIAG-4's break decision, reached from VerseRuntime's ABI callbacks.
	//
	// Called from inside the Verse interpreter's handshake with an op in flight, so nothing here
	// may enter the host except the vh_debug_* reads -- and those only from inside break_here,
	// which is where Godot's own debug loop runs.
	//
	// p_relation is a vh_debug_frame_relation: how the frame about to execute relates to the frame
	// the last stop was in. It stands in for Godot's depth counter, which this bridge cannot keep
	// because it never sees a Verse call, only an op.
	bool should_break(const godot::String &p_path, int32_t p_line, int32_t p_relation);
	void break_here();

	// Attaches the Verse debugger when Godot's is active and detaches it when it stops being.
	// Called once per frame from _frame and before each instance is made; cheap when nothing moved.
	void sync_attachment();

private:
	VerseScriptLanguage &language;

	// R-DIAG-4's state, all of it. Whether the Verse debugger is installed in the host, why the
	// last stop happened, and where it happened.
	//
	// The last stop's position is what keeps a step a step: `Total := Helper()` reports its line
	// twice, once before the call and once when the result lands, so a step-over with no memory of
	// where it started stops again on the line it started on. GDScript never meets this because
	// its line opcode is per source line; a Verse location is per op.
	bool debugger_attached = false;
	godot::String break_reason;
	godot::String stopped_source;
	int32_t stopped_line = 0;
	// Whether every location reported since the last stop has been that stop's own line again or
	// inside a call made from it. The breakpoint's half of the rule above: Continue from a line
	// holding a call must not stop on it again when the result lands.
	bool repeat_of_stop = false;

	// res:// path per absolute host path. The host asks once per distinct location per frame and
	// hands back the path it was given at build time, separators and all; localizing it is a
	// string walk that has no business happening inside the interpreter's handshake.
	//
	// The debugger's own: nothing in the profiler resolves a source path today.
	mutable std::unordered_map<std::string, godot::String> res_path_by_source;

	// One stopped frame's locals or members, in the { <p_key>: PackedStringArray, values: Array }
	// shape the extension wrapper splits on.
	godot::Dictionary debug_values_at(int32_t p_level, int32_t p_kind, const char *p_key) const;

	// The res:// path for a path the host named. Case-insensitive and separator-insensitive,
	// because the host hands back exactly what vh_compile_project was given -- which on Windows
	// is a mix: `C:/project\scripts\player.verse`.
	godot::String res_path_for_source(const godot::String &p_path) const;
};
