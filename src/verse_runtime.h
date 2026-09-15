#pragma once

#include "verse_host.h"
#include "verse_host_abi.h"

#include <godot_cpp/classes/global_constants.hpp>
#include <godot_cpp/classes/object.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/templates/vector.hpp>
#include <godot_cpp/variant/typed_array.hpp>

#include <string>
#include <unordered_map>

// One method a Verse script declares, in Godot's vocabulary.
//
// A copy rather than a view: vh_class_method_list's descriptors live until the next call to it,
// and a script's method table outlives many of those.
struct VerseMethodInfo {
	// The Verse name, which is the name Godot calls it by -- a script method is not transformed on
	// its way out.
	godot::StringName name;
	// What call_instance takes. A CharString because the ABI wants utf8 bytes and a StringName
	// would have to be converted at every call.
	godot::CharString decorated;
	// Godot's own name for the virtual this overrides -- `_ready` -- or empty for a plain method.
	// A virtual answers to this name as well as to its Verse one, which is how Godot's own calls
	// reach it.
	godot::StringName godot_virtual;

	struct Param {
		godot::StringName name;
		godot::Variant::Type type = godot::Variant::NIL;
	};
	godot::Vector<Param> params;
	int32_t required_params = 0;

	godot::Variant::Type return_type = godot::Variant::NIL;
	bool returns_value = false;

	bool can_fail = false;
	bool suspends = false;
};

// One signal a Verse script declares, in Godot's vocabulary. A copy, for the reason
// VerseMethodInfo is one: the host's descriptors live only until the next call.
struct VerseSignalInfo {
	// The Verse spelling verbatim -- `Hit`, not `hit`. Godot's C# does the same, and every scene
	// connection in this repository already reaches a Verse method by its Verse name.
	godot::StringName name;

	struct Arg {
		godot::StringName name;
		godot::Variant::Type type = godot::Variant::NIL;
	};
	godot::Vector<Arg> args;

	// vh_signal_reject. Anything but VH_SIGNAL_OK and the signal is not registered with Godot at
	// all -- it is carried this far only so `_validate` can say why, at the line below.
	int32_t reject = 0;
	// The argument or field a payload rejection is about; empty for the rest.
	godot::String reject_detail;
	// Zero-based, as the host counts; -1 when the definition has no source location.
	int32_t line = -1;
	int32_t column = -1;
};

// The "VerseRuntime" engine singleton. Owns the verse_host.dll loader and the vh_init_desc handed
// to it. Every method degrades to ERR_UNAVAILABLE plus a warning when no host is loaded; nothing
// here may crash for that reason.
class VerseRuntime : public godot::Object {
	GDCLASS(VerseRuntime, godot::Object)

protected:
	static void _bind_methods();

public:
	VerseRuntime() = default;
	~VerseRuntime() override;

	// Loads the host from wherever verse_host_paths resolves it -- the environment, then the
	// user's EditorSettings, then the legacy project settings (R-DIST-12) -- with
	// verse/host/enable_debugger, which is the one of the four that is genuinely per project,
	// read from ProjectSettings here.
	//
	// In an exported game none of them is read and all three paths come from where the executable
	// is running (D8): they name one machine's Unreal checkout, which is not on the machine that
	// plays the game.
	godot::Error load_host();
	godot::Error load_host(const godot::String &p_dll_path);
	void unload_host();
	bool is_host_loaded() const;

	// False for the runtime host an exported game ships. Everything that compiles, analyses,
	// completes or looks a symbol up answers ERR_UNAVAILABLE there, so the caller's question
	// is whether to ask at all rather than what the answer was.
	bool host_has_compiler() const;

	// The runtime host's filename, which is both what the .gdextension declares as a
	// dependency and what an exported game looks for beside its own executable.
	static const char *RUNTIME_HOST_FILENAME;

	void tick(double p_budget_seconds);

	// R-ASYNC-6's observable half: what the last pump did, as Godot custom monitors in the
	// profiler's Monitors tab. Registered on the first tick with a host loaded.
	void register_monitors();
	double _monitor_queued_jobs() const;
	double _monitor_pump_ms() const;
	double _monitor_sleeping_tasks() const;
	double _monitor_analysis_wait_ms() const;
	double _monitor_instance_tasks() const;

	// Verse's compilation unit is the package, not the file, so every .verse file in the project
	// is built together -- a build is always of the whole project. Each successful call publishes
	// a new generation; instances made against an earlier one keep running against it.
	//
	// p_module_paths runs parallel to p_globalized_paths and says which module each file's
	// definitions go into, "" being the project's root module. Godot answers that question because
	// it is a question about res://, which the host knows nothing about.
	//
	// While r_diagnostics_by_path is non-null every diagnostic the host reports, whatever its
	// severity, is filed under its own source path as { severity, line, column, message, path,
	// code } and nothing reaches the output log.
	godot::Error compile_project(const godot::PackedStringArray &p_globalized_paths, const godot::PackedStringArray &p_module_paths, godot::Dictionary *r_diagnostics_by_path);

	// The two halves of a Godot Callable that calls a Verse function (R-INT-4, R-SIG-3). Called
	// by VerseCallable, which is what Godot actually holds; see src/verse_callable.h for why only
	// a function bound to a script instance is accepted.
	int32_t invoke_callback(int64_t p_callback_id, const godot::Variant **p_args, int32_t p_arg_count, godot::Variant &r_result);
	void release_callback(int64_t p_callback_id);

	// Which generation the last successful compile_project published, counting from 1; 0 before
	// the first. A failed build does not advance it.
	int32_t script_generation() const { return generation; }

	// Builds a new generation of every .verse under res://, the way the editor's Play button and
	// Build action do. Bound for GDScript, which is the only way anything outside the editor can
	// make an edit live.
	godot::Error build_project();

	// Which of the project's modules declare a top-level p_name, by module path. Empty when the
	// name is in the root module -- which needs no import -- or is not in the project at all.
	godot::PackedStringArray modules_declaring(const godot::String &p_name) const;
	// Re-runs semantic analysis with one file's text replaced, filing diagnostics the same way
	// compile_project does. Generates nothing, so it is safe to call as often as the editor asks.
	godot::Error check_project(const godot::String &p_globalized_path, const godot::String &p_source, godot::Dictionary *r_diagnostics_by_path);

	// The same analysis on the host's own thread, so the editor keeps drawing. ERR_BUSY when one
	// is already running -- only one at a time.
	godot::Error begin_check_project(const godot::String &p_globalized_path, const godot::String &p_source);

	// Reaps a begin_check_project. True only on the call that reaps one, which is when
	// r_diagnostics_by_path has been filled in. Cheap enough to call every frame.
	bool poll_check_project(godot::Dictionary *r_diagnostics_by_path);

	bool is_check_project_busy() const;
	// A script is a top-level class named after its own file, driven through one instance of
	// that class per node. False when the compiled project defines no such class, which is what
	// makes the .verse file unusable as a script.
	bool has_class(const godot::String &p_class_name) const;
	// The `@editable` data members of a top-level class, as { name, type, is_var } entries
	// where type is a vh_type. Read out of the last analysis pass rather than the running
	// program, so it follows the editor's buffer and refreshes without restarting -- unlike
	// anything routed through the compiled bytecode, which may only be generated once.
	//
	// r_found separates a class that exports nothing from one the analysis never saw; both come
	// back as an empty array, and only the second means the answer is not to be believed.
	godot::TypedArray<godot::Dictionary> class_exports(const godot::String &p_class_name, bool *r_found = nullptr) const;

	// The definition the identifier at p_line/p_column resolves to, as
	// { name, path, line, column, type, kind, is_var }, or an empty dictionary when nothing
	// there resolves. Positions in and out are the compiler's: zero-based rows, and columns
	// that are byte offsets into the line rather than character counts.
	//
	// Answered from the last analysis, which cannot tell that the buffer has moved since --
	// so the caller must have established that the text at p_globalized_path is the text that
	// analysis saw, or every locus below an edit is off by the rows it added.
	godot::Dictionary lookup_symbol(const godot::String &p_globalized_path, int32_t p_line, int32_t p_column) const;

	// What could be written at p_line/p_column of p_source, as an array of
	// { name, type, owner, kind, is_var } -- the members of the expression there when
	// p_mode is VH_COMPLETE_MEMBERS, everything the scope admits when it is VH_COMPLETE_SCOPE,
	// and the attributes among those when it is VH_COMPLETE_ATTRIBUTES. Positions are the
	// compiler's, as above, and for members they are the *receiver's* last byte rather than the
	// cursor.
	//
	// Takes the buffer to say which text it is asking about, not to have it analysed: since ABI
	// v7 the host neither analyses nor waits here. r_not_ready is set when it answered VH_ERR_STATE
	// -- an analysis is in flight, or the program was built from other text -- which is a different
	// thing from an empty answer and is the caller's cue to have this very buffer analysed and ask
	// again. Left alone otherwise, so a caller that does not care may pass nullptr.
	godot::TypedArray<godot::Dictionary> complete_symbol(const godot::String &p_globalized_path, const godot::String &p_source, int32_t p_line, int32_t p_column, int32_t p_mode, bool *r_not_ready = nullptr) const;

	// Every member p_class_name declares itself, as { name, type, owner, path, line, kind,
	// is_var } -- broader than class_exports, which answers only for the inspector. Read off the
	// last analysis, so it follows the editor's buffer.
	godot::TypedArray<godot::Dictionary> class_members(const godot::String &p_class_name) const;

	// What p_class_name inherits and has not declared itself, in the same shape -- the names an
	// `<override>` could still be written for. Read off the same snapshot, so it answers on the
	// keystroke rather than after the analysis behind it, and describes the last analysed text:
	// a method added since is neither in class_members nor missing from here.
	godot::TypedArray<godot::Dictionary> class_override_candidates(const godot::String &p_class_name) const;

	// The function called at p_line/p_column of p_source, as { name, result, params }. The
	// position names the callee's last byte rather than the cursor, for the same reason
	// complete_symbol's does, and r_not_ready means the same thing.
	godot::Dictionary signature_at(const godot::String &p_globalized_path, const godot::String &p_source, int32_t p_line, int32_t p_column, bool *r_not_ready = nullptr) const;

	vh_instance *instantiate(const godot::String &p_class_name, int64_t p_object_id);
	void release_instance(vh_instance *p_instance);
	bool instance_has_function(vh_instance *p_instance, const char *p_decorated_name) const;

	// Calls any method the script declares. Answers a vh_status rather than a godot::Error because
	// the caller has to tell the four outcomes apart: VH_ERR_NOT_FOUND is INVALID_METHOD to Godot,
	// VH_ERR_ARGUMENT is an argument-count or type error, VH_ERR_FAILED is a <decides> method that
	// declined -- which is a nil return, not an error -- and VH_ERR_RUNTIME has already been
	// reported with its stack.
	int32_t call_instance(vh_instance *p_instance,
			const char *p_decorated_name,
			const godot::Variant **p_args,
			int32_t p_arg_count,
			godot::Variant &r_result);

	// Every method a script's class declares, copied out of the host's storage -- which the ABI
	// only promises until the next call, so nothing here may hold a pointer into it.
	godot::Vector<VerseMethodInfo> class_methods(const godot::String &p_class_name) const;

	// A class's `@statics` module, as { name -> value } for its constants and a list of names for
	// its functions. R-NODE-4: Verse has no `static` keyword, and an inline module is what it has
	// instead -- what crosses is the *link*, which is the only part Godot needs telling about.
	godot::Dictionary class_static_constants(const godot::String &p_class_name) const;
	godot::PackedStringArray class_static_methods(const godot::String &p_class_name) const;

	// R-NODE-5: whether the class is `class<abstract>`, so Godot stops offering to instantiate a
	// base script that was never meant to be attached.
	bool class_is_abstract(const godot::String &p_class_name) const;

	// The signals a class declares, its base script classes' included. Read out of the last
	// analysis rather than the running program, so a signal added in the editor shows up without
	// a build -- the same bargain the export list makes.
	godot::Vector<VerseSignalInfo> class_signals(const godot::String &p_class_name) const;

	// One data member read off a live instance, and off the class default object respectively.
	// Unlike class_exports these go through the VM, because a value exists nowhere else. A nil
	// Variant means the field is absent or holds a Verse type with no Variant counterpart.
	godot::Variant instance_field(vh_instance *p_instance, const godot::String &p_name) const;
	godot::Variant class_default_field(const godot::String &p_class_name, const godot::String &p_name) const;
	// Writes one data member on a live instance. Covers bool, int, float, String and an Object --
	// the same types the reader covers -- and refuses anything else rather than truncating it.
	// An Object crosses as its instance id, and a null one clears the member.
	bool set_instance_field(vh_instance *p_instance, const godot::String &p_name, const godot::Variant &p_value);
	// Writes a reference member with another script's instance rather than with a handle, which is
	// what a member typed as one of the project's own classes holds. A null p_value clears it.
	bool set_instance_field_instance(vh_instance *p_instance, const godot::String &p_name, vh_instance *p_value);

	// R-DIAG-4. Installs or removes the Verse debugger; false when Epic's own socket debugger
	// holds the VM's single debugger slot, which is what verse/host/enable_debugger asks for.
	bool debug_set_enabled(bool p_enabled);

	// The three stopped-stack reads, all defined only while the host has a stop stashed -- which is
	// to say only from inside VerseScriptLanguage::debug_break. Outside one, the count is 0 and the
	// two dictionaries are empty.
	int32_t debug_stack_count() const;
	// { function, source, line } for one frame, innermost first. `source` is the path the host
	// named, unlocalized: turning it into a res:// path is the caller's, because only the caller
	// caches the answer.
	godot::Dictionary debug_stack_frame(int32_t p_level) const;
	// { names: PackedStringArray, values: Array } for one frame's locals or members. A value the
	// bridge carries arrives as itself; anything else -- a tuple, an option, a map, a class
	// instance -- arrives as the VM's own rendering, as a String.
	godot::Dictionary debug_stack_values(int32_t p_level, int32_t p_kind) const;

	// R-DIAG-5. Off until Godot's profiler asks.
	void profiling_set_enabled(bool p_enabled);
	// One row per boundary crossing the bridge timed and per `profile{}` block a script ran, as
	// { signature, call_count, total_time, self_time }. Times are microseconds, which is the unit
	// ScriptLanguage::ProfilingInfo carries. p_frame_only answers this frame's rows and resets
	// them; otherwise the run's.
	godot::TypedArray<godot::Dictionary> profiling_read(bool p_frame_only) const;

private:
	VerseHostLibrary host;

	// vh_init answered something other than VH_OK, and there is no second attempt: the host module
	// never unloads, so re-entering it runs FEngineLoop's PreInit twice and asserts.
	bool host_init_refused = false;

	// The last error the host reported through OnDiagnostic. An exported game shows it and stops
	// (D6): vh_init answers a status code, and the sentence an author can act on is this.
	godot::String last_error_message;

	vh_init_desc init_desc = {};
	vh_godot_api godot_api = {};
	godot::Dictionary *diagnostic_sink = nullptr;
	int32_t generation = 0;

	// The bytes api_signal_target last handed back. Held here rather than on the stack because the
	// host reads them after the call returns, which is the bargain every string this side lends
	// makes: valid until the next call.
	godot::CharString held_signal_name;

	vh_tick_stats last_tick_stats = {};
	bool monitors_registered = false;

	// How many times one raise site has printed its stack, and when its window opened (R-DIAG-3).
	//
	// Godot throttles the *error* itself -- network/limits/debugger/max_errors_per_second, and it
	// says once that it dropped some. What it does not throttle is the stack this prints
	// underneath, which is ordinary output: a script raising at 60 Hz with a six-frame stack emits
	// ~360 lines a second into a shared character budget and silences every other script, which is
	// the opposite of the intent. Keyed on the raise site plus the message, so a second script
	// raising somewhere else is not suppressed by the first.
	struct RaiseSite {
		double window_opened = 0.0;
		int64_t suppressed = 0;
	};
	std::unordered_map<std::string, RaiseSite> raise_sites;

	// Says how many stacks a closed window swallowed, and forgets the site. Called from tick, so
	// the summary lands within a frame of the window closing rather than only when the same error
	// happens again -- a script that raised sixty times and then stopped would otherwise never be
	// told what was dropped.
	void flush_suppressed_raises();
	// Frames the pump has been over budget in a row, so the warning can be said once and then
	// rarely rather than once per frame.
	int64_t overrun_frames = 0;

	godot::Error load_host_internal(const godot::String &p_dll_path, const godot::String &p_engine_dir, bool p_enable_debugger, const godot::String &p_cooked_dir);

	// Says why and closes the game, in an exported build only (D6). A no-op in the editor.
	void refuse_to_start(const godot::String &p_why);

	static void api_print(void *p_ctx, const char *p_utf8, int32_t p_len);
	static vh_bool api_is_valid(void *p_ctx, vh_handle p_handle);
	static int32_t api_get_property(void *p_ctx, vh_handle p_handle, const char *p_name_utf8, int32_t p_name_len, vh_arena *p_arena, vh_value *r_value);
	static int32_t api_set_property(void *p_ctx, vh_handle p_handle, const char *p_name_utf8, int32_t p_name_len, const vh_value *p_value);
	static int32_t api_call_method(void *p_ctx, vh_handle p_handle, const char *p_name_utf8, int32_t p_name_len, const vh_value *p_args, int32_t p_arg_count, vh_arena *p_arena, vh_value *r_value);
	static vh_handle api_get_singleton(void *p_ctx, const char *p_name_utf8, int32_t p_name_len);
	static int32_t api_get_class_of(void *p_ctx, vh_handle p_handle, vh_arena *p_arena, vh_value *r_class_name);
	static int64_t api_make_callable(void *p_ctx, int64_t p_callback_id, vh_handle p_owner_handle);
	static int32_t api_call_static(void *p_ctx, const char *p_class_utf8, int32_t p_class_len, const char *p_name_utf8, int32_t p_name_len, const vh_value *p_args, int32_t p_arg_count, vh_arena *p_arena, vh_value *r_value);
	static int32_t api_call_utility(void *p_ctx, const char *p_name_utf8, int32_t p_name_len, const vh_value *p_args, int32_t p_arg_count, vh_arena *p_arena, vh_value *r_value);

	// Signals (R-SIG-1..4). Declared in the v2 header and left unsupplied until now.
	static int32_t api_emit_signal(void *p_ctx, vh_handle p_handle, const char *p_name_utf8, int32_t p_name_len, const vh_value *p_args, int32_t p_arg_count);
	static int32_t api_connect_signal(void *p_ctx, vh_handle p_handle, const char *p_name_utf8, int32_t p_name_len, const vh_value *p_target, int32_t p_flags);
	static int32_t api_disconnect_signal(void *p_ctx, vh_handle p_handle, const char *p_name_utf8, int32_t p_name_len, const vh_value *p_target);
	static int32_t api_signal_target(void *p_ctx, int64_t p_ref, vh_handle *r_handle, const char **r_name_utf8);
	static int64_t api_make_signal_ref(void *p_ctx, vh_handle p_handle, const char *p_name_utf8, int32_t p_name_len);

	// R-DIAG-4. Both are called from inside the Verse interpreter's handshake, with an op in
	// flight, and both forward straight to VerseScriptLanguage -- the breakpoint list and the step
	// state are the script language's, and the runtime only owns the wire.
	static vh_bool api_debug_should_break(void *p_ctx, const char *p_path_utf8, int32_t p_path_len, int32_t p_line, int32_t p_relation);
	static void api_debug_break(void *p_ctx);

	// The reference table (R-TYPE-1). See src/verse_ref_table.h for what is in it and why, and the
	// ABI header's "reference values" for the ownership rule these implement.
	static void api_release_ref(void *p_ctx, int64_t p_ref);
	static int64_t api_retain_ref(void *p_ctx, int64_t p_ref);
	static int64_t api_new_ref(void *p_ctx, int32_t p_variant_tag);
	static int32_t api_ref_get(void *p_ctx, int64_t p_ref, const vh_value *p_key, vh_arena *p_arena, vh_value *r_value);
	static int32_t api_ref_set(void *p_ctx, int64_t p_ref, const vh_value *p_key, const vh_value *p_value);
	static int32_t api_ref_size(void *p_ctx, int64_t p_ref, int64_t *r_size);
	static int32_t api_ref_contents(void *p_ctx, int64_t p_ref, vh_arena *p_arena, vh_value *r_value);
	static int32_t api_invoke_callable(void *p_ctx, int64_t p_ref, const vh_value *p_args, int32_t p_arg_count, vh_arena *p_arena, vh_value *r_value);

	// R-NODE-3: an object a Verse script made, which is not a node and has no script attached. See
	// the ABI header's v8.3 block for the ownership split these two implement.
	static vh_handle api_instantiate_class(void *p_ctx, const char *p_class_utf8, int32_t p_class_len);
	static void api_release_object(void *p_ctx, vh_handle p_handle, vh_bool p_discard);

	// R-DIAG-2: a Verse runtime error, with the file, line and Verse call stack it was raised at.
	// Separate from on_diagnostic because the two go to different places -- a compile error
	// annotates the script editor's gutter, this goes to the output and errors panel.
	static void on_runtime_error(void *p_ctx, const vh_runtime_error *p_error);

	static void on_diagnostic(void *p_ctx, const vh_diagnostic *p_diagnostic);
};
