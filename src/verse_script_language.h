#pragma once

// _make_template returns a Ref<Script>, and Ref's destructor needs the complete type;
// script_language_extension.hpp only forward-declares it.
#include "verse_api_lookup.h"
#include "verse_bindings.h"
#include "verse_debugger.h"
#include "verse_profiler.h"
#include "verse_project_state.h"
#include "verse_script.h"

#include <godot_cpp/templates/hash_map.hpp>
#include <godot_cpp/templates/hash_set.hpp>
#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/script.hpp>
#include <godot_cpp/classes/script_language_extension.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/typed_array.hpp>

#include <atomic>
#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

struct VerseClassDecl;
struct VerseScriptInstance;
class VerseScript;

#ifdef TOOLS_ENABLED
namespace godot {
class EditorInterface;
}
#endif

// Line endings normalised away. A buffer arrives through TextEdit, which need not hand back the
// endings the file was written with, and Verse analyses identically either way.
godot::String verse_newline_normalized(const godot::String &p_source);

// The run of comment lines immediately above p_line, delimiters removed. Verse has no doc-comment
// form of its own, so this convention -- whatever precedes a definition documents it -- is the
// whole of where a script's documentation comes from.
godot::String verse_doc_comment_above(const godot::String &p_source, int64_t p_line);

// verse_doc_to_bbcode over a godot::String: what verse_doc_comment_above produced, in the BBCode
// Godot's documentation renderer reads. Every description handed to Godot goes through it.
godot::String verse_doc_bbcode(const godot::String &p_doc);

// verse_godot_class_for and verse_godot_class_name, which verse_script.cpp and verse_runtime.cpp
// also use, are declared in verse_api_lookup.h, included above.

#ifdef TOOLS_ENABLED
// The EditorInterface singleton, or nullptr when this process is not an editor.
//
// Not the same as EditorInterface::get_singleton(), which is two errors in the log before the null
// comes back: Engine::get_singleton_object refuses an editor-only singleton unless is_editor_hint()
// (engine: core/config/engine.cpp:347-351), and godot-cpp then reports the null it was handed. Null
// checking the result is therefore too late -- the question has to be asked first.
//
// TOOLS_ENABLED does not answer it. A game launched from the editor loads this same editor build of
// the library, with is_editor_hint() false, which is where the errors actually showed up.
godot::EditorInterface *verse_editor_interface();
#endif

// The "Verse" ScriptLanguage. One instance, created and handed to
// Engine::register_script_language by register_types.cpp, so singleton() is valid for the life
// of the extension.
//
// Only the virtuals this phase actually answers are overridden. godot-cpp gives every other
// ScriptLanguageExtension virtual a default body, and its register_virtuals() binds a virtual
// with Godot only when the subclass declares it, so leaving one out is the way to say
// "unsupported" rather than an omission.
class VerseScriptLanguage : public godot::ScriptLanguageExtension {
	GDCLASS(VerseScriptLanguage, godot::ScriptLanguageExtension)

protected:
	static void _bind_methods();

public:
	VerseScriptLanguage();
	~VerseScriptLanguage() override;

	static VerseScriptLanguage *singleton();

	godot::String _get_name() const override;
	godot::String _get_type() const override;
	godot::String _get_extension() const override;
	godot::PackedStringArray _get_recognized_extensions() const override;
	void _init() override;
	void _finish() override;

	// Godot's EditorStandardSyntaxHighlighter builds its CodeHighlighter from exactly these
	// five, so answering them is the whole of tier-one syntax highlighting. Verse's nested
	// <# #> comments and comments-inside-strings are beyond what a delimiter list can express;
	// that is the Phase 5 lexer, not this.
	godot::PackedStringArray _get_reserved_words() const override;
	bool _is_control_flow_keyword(const godot::String &p_keyword) const override;
	godot::PackedStringArray _get_comment_delimiters() const override;
	godot::PackedStringArray _get_doc_comment_delimiters() const override;
	godot::PackedStringArray _get_string_delimiters() const override;

	godot::Ref<godot::Script> _make_template(const godot::String &p_template, const godot::String &p_class_name, const godot::String &p_base_class_name) const override;
	bool _is_using_templates() override;
	godot::Object *_create_script() const override;
	godot::Dictionary _validate(const godot::String &p_script, const godot::String &p_path, bool p_validate_functions, bool p_validate_errors, bool p_validate_warnings, bool p_validate_safe_lines) const override;

	bool _has_named_classes() const override;
	bool _supports_builtin_mode() const override;
	bool _supports_documentation() const override;
	bool _can_inherit_from_file() const override;
	bool _can_make_function() const override;
	int32_t _find_function(const godot::String &p_function, const godot::String &p_code) const override;
	godot::String _auto_indent_code(const godot::String &p_code, int32_t p_from_line, int32_t p_to_line) const override;
	godot::ScriptLanguage::ScriptNameCasing _preferred_file_name_casing() const override;

	// Verse has no host-injected globals; these exist because Godot calls them unconditionally.
	void _add_global_constant(const godot::StringName &p_name, const godot::Variant &p_value) override;
	void _add_named_global_constant(const godot::StringName &p_name, const godot::Variant &p_value) override;
	void _remove_named_global_constant(const godot::StringName &p_name) override;

	void _thread_enter() override;
	void _thread_exit() override;

	void _reload_all_scripts() override;
	void _reload_scripts(const godot::Array &p_scripts, bool p_soft_reload) override;
	void _reload_tool_script(const godot::Ref<godot::Script> &p_script, bool p_soft_reload) override;

	godot::String _validate_path(const godot::String &p_path) const override;
	godot::TypedArray<godot::Dictionary> _get_built_in_templates(const godot::StringName &p_object) const override;
	godot::String _make_function(const godot::String &p_class_name, const godot::String &p_function_name, const godot::PackedStringArray &p_function_args) const override;
	godot::Error _open_in_external_editor(const godot::Ref<godot::Script> &p_script, int32_t p_line, int32_t p_column) override;
	bool _overrides_external_editor() override;
	godot::Dictionary _complete_code(const godot::String &p_code, const godot::String &p_path, godot::Object *p_owner) const override;
	godot::Dictionary _lookup_code(const godot::String &p_code, const godot::String &p_symbol, const godot::String &p_path, godot::Object *p_owner) const override;

	// R-DIAG-4. A breakpoint in Godot's script editor stops a Verse script and shows its locals.
	//
	// The division with the host is: the host owns *which frame*, because only the Verse
	// interpreter's Notify can see one; this owns *which line*, because the breakpoint list and the
	// step state are Godot's. Everything below answers from what the host stashed at the stop, and
	// is defined only while one is on the stack.
	//
	// Two of these stay empty on purpose and the audit in phase-6-design.md §7 says why:
	// _debug_get_stack_level_instance can never be anything but null (Godot calls a C++ virtual on
	// what it returns, and a GDExtension script instance is not a ScriptInstance), and Verse has no
	// mutable globals for _debug_get_globals to answer with.
	godot::String _debug_get_error() const override;
	int32_t _debug_get_stack_level_count() const override;
	int32_t _debug_get_stack_level_line(int32_t p_level) const override;
	godot::String _debug_get_stack_level_function(int32_t p_level) const override;
	godot::String _debug_get_stack_level_source(int32_t p_level) const override;
	godot::Dictionary _debug_get_stack_level_locals(int32_t p_level, int32_t p_max_subitems, int32_t p_max_depth) override;
	godot::Dictionary _debug_get_stack_level_members(int32_t p_level, int32_t p_max_subitems, int32_t p_max_depth) override;
	void *_debug_get_stack_level_instance(int32_t p_level) override;
	godot::Dictionary _debug_get_globals(int32_t p_max_subitems, int32_t p_max_depth) override;
	// _debug_parse_stack_level_expression is deliberately NOT declared. It is EXBIND, so leaving it
	// unbound is how a ScriptLanguage says "unsupported" and is silent; and the remote debugger
	// never reaches it anyway, because its guard is _debug_get_stack_level_instance, which is null
	// forever. Evaluating would mean compiling an expression against a stopped frame's scope and
	// running it in a VM paused mid-op, and Epic's own Verse DAP client has no `evaluate` to follow.
	godot::TypedArray<godot::Dictionary> _debug_get_current_stack_info() override;

	void _profiling_start() override;
	void _profiling_stop() override;
	void _profiling_set_save_native_calls(bool p_enable) override;
	int32_t _profiling_get_accumulated_data(godot::ScriptLanguageExtensionProfilingInfo *p_info_array, int32_t p_info_max) override;
	int32_t _profiling_get_frame_data(godot::ScriptLanguageExtensionProfilingInfo *p_info_array, int32_t p_info_max) override;

	bool _handles_global_class_type(const godot::String &p_type) const override;
	godot::Dictionary _get_global_class_name(const godot::String &p_path) const override;

	godot::TypedArray<godot::Dictionary> _get_public_functions() const override;
	godot::Dictionary _get_public_constants() const override;
	godot::TypedArray<godot::Dictionary> _get_public_annotations() const override;

	// Pumps the Verse event loop once per frame for the whole language, which is what makes
	// every scripted node driven rather than one hand-placed one.
	void _frame() override;

	// Bound so EditorFileSystem's filesystem_changed can reach it. Nothing else calls it.
	void on_filesystem_changed();
	void on_script_classes_updated();

	// Regenerates the bindings package and hands it to the host (R-INT-7, R-INT-8). Cheap to
	// call when nothing changed: the host compares the source and only a *changed* one costs a
	// package name.
	// False when the host is not up yet, which leaves the request armed for the next frame.
	bool refresh_bindings();
	// What a withheld build says instead of the diagnostics it is withholding.
	void warn_incomplete_roster() const;
	bool bindings_hook_connected = false;
	bool bindings_refresh_pending = true;
	// What a generated binding stands for, which is the only thing the editor can say about
	// one. The Verse declaration says nothing: the package is a synthetic snippet the host
	// reads back from a digest in the engine tree, so a binding's `path` is a file no editor
	// can open and there is no comment above it that anybody wrote.
	//
	// Filled by refresh_bindings, and read by _lookup_code, the syntax highlighter and
	// R-INT-10's refusal.
	struct BindingInfo {
		/// The ClassDB class, or empty for a script class.
		godot::String godot_class;
		/// The global `class_name`, or empty for a ClassDB class.
		godot::String script_class;
		/// The script's res:// path, or empty for a ClassDB class.
		godot::String script_path;
		/// Verse member name -> the Godot name it calls, which is what names a doc page.
		godot::HashMap<godot::String, godot::String> methods;
		godot::HashMap<godot::String, godot::String> signals;
	};
	const BindingInfo *binding_for(const godot::String &p_verse_class) const;
	// A binding that stands for a *script* class, which is the one base a Verse class may not
	// extend (R-INT-10).
	bool is_script_binding(const godot::String &p_verse_class) const;
	const godot::HashMap<godot::String, BindingInfo> &bindings() const { return bindings_by_verse_class; }

	godot::HashMap<godot::String, BindingInfo> bindings_by_verse_class;
	// The last generation's classes, whole, which BindingInfo above is not: it keeps names and
	// drops the types a declaration needs. Handed to the next generation so a script held back to
	// avoid a cyclic load keeps the members the last one read off it (B36).
	std::vector<VerseBindingClass> last_binding_classes;

	// The consumer half of R-DIAG-4's break decision, reached from VerseRuntime's ABI callbacks.
	// VerseDebugger documents the rest; these two are the public face, the way build_project is
	// VerseProjectState's.
	bool debug_should_break(const godot::String &p_path, int32_t p_line, int32_t p_relation) {
		return debugger.should_break(p_path, p_line, p_relation);
	}
	void debug_break() { debugger.break_here(); }

	// Budget handed to vh_tick each frame, so a runaway Verse task costs frame rate rather than
	// hanging the editor. Read from the verse/runtime/frame_budget_ms project setting at _init.
	double get_frame_budget_ms() const;

	// Attaches the Verse debugger when Godot's is active and detaches it when it stops being.
	// Called once per frame from _frame, which is also where every other per-frame decision is.
	void sync_debugger_attachment() { debugger.sync_attachment(); }

	// The build-and-analysis pump's public face; VerseProjectState documents each.
	godot::Error build_project() { return project_state.build_project(); }
	godot::Error ensure_project_built() { return project_state.ensure_project_built(); }
	godot::TypedArray<godot::Dictionary> check_buffer(const godot::String &p_path, const godot::String &p_source) const {
		return project_state.check_buffer(p_path, p_source);
	}
	godot::TypedArray<godot::Dictionary> diagnostics_for(const godot::String &p_path) const {
		return project_state.diagnostics_for(p_path);
	}
	godot::TypedArray<godot::Dictionary> compiler_warnings_for(const godot::String &p_path) const {
		return project_state.compiler_warnings_for(p_path);
	}
	bool analysis_is_current(const godot::String &p_path, const godot::String &p_source) const {
		return project_state.analysis_is_current(p_path, p_source);
	}
	uint64_t description_epoch_value() const { return project_state.description_epoch_value(); }
	void queue_check(const godot::String &p_path, const godot::String &p_source) const {
		project_state.queue_check(p_path, p_source);
	}

	// Every hover the editor could produce over one script, as one row per word and run of
	// columns that answer alike. The editor's own hover path is unreachable from a test --
	// CodeEdit decides the word and the column from the mouse, and ScriptTextEditor turns the
	// result into a tooltip in the editor's own C++ -- so this reproduces both halves over a
	// file on disk and reports what _lookup_code answered beside what the host resolved, which
	// is the pair a mislabelled tooltip has to be read out of. tools/probe_hover.py consumes it.
	godot::TypedArray<godot::Dictionary> probe_hover(const godot::String &p_path);

	// The same seam for completion. p_positions is flat (line, column) pairs, zero-based,
	// with the column a byte offset into the line the way probe_hover reports one -- chosen
	// by the caller rather than walked here, because a completion costs an analysis where a
	// hover costs none: _complete_code substitutes a placeholder for the identifier being
	// typed, so the text the host is asked about differs per caret and the one analysis
	// probe_hover gets away with does not exist here.
	//
	// Each position is asked twice, which is what an author gets: the first answer comes from
	// whatever the last analysis left and queues the buffer this caret needs, and the second
	// comes from that. In the editor _frame reaps the queue; this has no frames, so it
	// flushes the check itself. tools/probe_complete.py consumes it.
	godot::TypedArray<godot::Dictionary> probe_complete(
		const godot::String &p_path, const godot::PackedInt32Array &p_positions);

	// The classes a `.` at p_receiver_end reaches a member of, nearest first, or empty when the
	// buffer does not say which. Text and the analysis snapshot only: this is what completion has
	// to answer from before any analysis of the buffer in front of the author exists.
	godot::PackedStringArray receiver_classes_from_text(const godot::String &p_source, const godot::String &p_path, int64_t p_receiver_end) const;

	// Fills r_result with what a string literal at p_marker can be completed to -- a node path, a
	// res:// path, an input action or a signal name -- decided by the call the literal is an
	// argument to. Leaves it untouched when the literal is not one of those.
	void complete_in_string(const godot::String &p_code, const godot::String &p_path, int64_t p_marker, godot::Object *p_owner, godot::Dictionary &r_result) const;

	// The signals reachable on the receiver ending at p_receiver_end: the class's own Verse-spelled
	// ones, or Godot's for a mirrored class.
	void collect_signal_names(const godot::String &p_source, const godot::String &p_path, int64_t p_receiver_end, godot::Array &r_options) const;

	// Scripts that read their validity and export list out of the analysis, so a result landing
	// in _frame reaches them. Borrowed: a script adds itself on construction and removes itself
	// on destruction.
	void register_script(VerseScript *p_script);
	void unregister_script(VerseScript *p_script);

	// Said by a script Godot asked to describe itself before the analysis that describes it had
	// landed, so the next one that does can re-answer the question. Godot asks once, on a thread
	// of its own, and never asks again on its own account -- see republish_script_docs.
	//
	// Atomic because that thread is not the editor's: the flag is set off it and read in _frame.
	void note_script_docs_deferred() const;

	// Live script instances, by the instance id of the object each is attached to. Borrowed, the
	// way live_scripts is: an instance adds itself in create() and removes itself in free_func,
	// which are the only two places one is born and dies.
	//
	// The table exists because an exported reference to another script's class has to hold *that*
	// node's Verse object, and what the inspector hands over is a Godot Object -- so something has
	// to map one to the other, and Godot's own script-instance pointer is not reachable from here.
	void register_instance(int64_t p_object_id, VerseScriptInstance *p_instance);
	void unregister_instance(int64_t p_object_id);
	VerseScriptInstance *instance_for(int64_t p_object_id) const;

	// The class name every .verse file under res:// defines, which is its own stem: a script's
	// class is named after its file, and one flat scope for the whole project is what forces
	// that. It needs no compiled program -- the syntax highlighter has to colour a script that has
	// never been built.
	//
	// Cached, because the walk underneath is a recursive DirAccess enumeration of the whole
	// project and completion asked for it on every keystroke. invalidate_script_class_names is
	// what puts a new, renamed or deleted file into it.
	//
	// Module-qualified -- `left/widget` for a file under a `.vmodule` -- which is the name the
	// host reports as a member's owner and the name the script registers its documentation
	// under. A caller offering a name for an author to type takes the leaf.
	const godot::PackedStringArray &script_class_names() const;

	// Every class name the generated Godot mirror carries, as Strings built once for the process.
	// Shared by completion, which matches a typed prefix against all ~1026 of them per keystroke,
	// and by the syntax highlighter, which colours them -- both used to pay to turn the same
	// static table of `const char *` into Strings again.
	static const godot::PackedStringArray &mirrored_class_names();

	// The module a script's definitions go into: "" for the root module, otherwise a
	// '/'-separated path. A directory is a module only if it carries a `<name>.vmodule` marker,
	// and the marker names the module -- see src/verse_module_map.h for the whole rule.
	godot::String module_for_script(const godot::String &p_res_path) const;

	// The module-qualified class name for a script: `player` at the root, `gameplay/player` in a
	// module. This is what every ClassNameUtf8 in the host ABI carries.
	godot::String qualified_class_name(const godot::String &p_res_path) const;

	// Every `.verse` under p_dir, recursively. Public because the export plugin enumerates the
	// same set a build does, and a project that means one thing to the build and another to the
	// cook is the one way an export can be wrong and look right.
	static godot::PackedStringArray find_verse_sources(const godot::String &p_dir);

	// The Godot class a script attaches at, and the base_type its global-class registration
	// records. One walk up the Verse superclass chain answers both, which is why they come back
	// together: they differ only in where they stop.
	struct BaseTypes {
		// The nearest mirrored Godot class. "Node" when the chain reaches none, which is the
		// floor for any scripted node.
		godot::StringName instance_base;
		// The nearest ancestor that is itself a global class, so the class picker nests them the
		// way C# does; instance_base when there is no such ancestor.
		godot::String registry_base;
	};
	BaseTypes base_types_for(const VerseClassDecl &p_decl) const;

	// Drops the cached list of script class names, so the next ask re-walks res://. Called by a
	// build and, in the editor, by EditorFileSystem's filesystem_changed.
	void invalidate_script_class_names() const;

private:
	static VerseScriptLanguage *singleton_instance;
	friend class VerseProjectState;
	// Mutable because Godot's virtuals are const and nearly every one of them feeds the pump.
	mutable VerseProjectState project_state{ *this };
	double frame_budget_ms = 4.0;
	// Warnings for the members a script declared and the host refused -- an `@export` the inspector
	// cannot draw, a `signal` Godot cannot register -- keyed by res:// path and shaped the
	// way _validate hands one over. Harvested when an analysis lands rather than asked for at
	// _validate: both lists describe the analysis the snapshot came from, so taking them in the
	// poll is what pairs a warning set with the diagnostics reported out of the same one.
	mutable godot::Dictionary script_warnings_by_path;

	// The completion buffer the last vh_complete_symbol answered for, with its position, mode, the
	// analysis_epoch it was answered at, and the answer. Godot re-asks on every keystroke while the
	// popup is open, and each ask costs a whole-project analysis; normalizing the half-typed
	// identifier out of the buffer is what makes the whole of one prefix the same question, and the
	// epoch is what makes it safe to answer from cache rather than merely cheap.
	mutable godot::String completion_cache_source;
	mutable int32_t completion_cache_line = -1;
	mutable int32_t completion_cache_column = -1;
	mutable int32_t completion_cache_mode = -1;
	mutable uint64_t completion_cache_epoch = 0;
	mutable godot::TypedArray<godot::Dictionary> completion_cache_options;

	// The same, for the argument hint. Kept apart because the two are asked about different
	// positions in one buffer -- the callee for the hint, the cursor for the options -- and the
	// host answers both off a single analysis, so caching them together would throw one away.
	mutable godot::String signature_cache_source;
	mutable int32_t signature_cache_line = -1;
	mutable int32_t signature_cache_column = -1;
	mutable uint64_t signature_cache_epoch = 0;
	mutable godot::Dictionary signature_cache;

	// Asks the open script editor to complete again, if it is still showing the file the landed
	// completion analysis was for and the caret is still inside the same identifier. That second
	// _complete_code finds the host describing the buffer and replaces the partial list in place.
	void refresh_completion_if_current() const;

	// Re-registers every loaded script's documentation, which is the only way a class described
	// too early gets described again: Godot builds its script docs once per session, on a loader
	// thread of its own, and otherwise republishes a script's only when it is saved.
	void republish_script_docs() const;

	// Registers one script class's documentation, if what Godot holds for it is not what the
	// current analysis would say. Called from a lookup that is about to name the class, because
	// the tooltip for a member of a script class is drawn from the registered doc and from
	// nothing else -- so a hover is the moment the doc has to exist, and the one place that
	// knows it is wanted (B38).
	void ensure_script_doc_published(const godot::String &p_class_name) const;

	// Registers a documentation page for a Godot-package function that no Godot class documents --
	// an extension method on a Verse type like `event(t)`, or a free function of GodotApi -- so a
	// hover draws a method tooltip rather than a constant whose type is the whole function type. No
	// Godot page exists for one, and `EditorHelp` is not exposed to a GDExtension, so the page is
	// carried by `api_doc_carrier`: a script with no file whose only job is to feed
	// `ScriptEditor::update_docs_from_script`, the one door onto the doc store. Returns the class
	// name to put in the lookup result, or empty when there is no script editor to register with
	// (a headless run), so the caller falls back to the local result (B40).
	godot::String publish_api_method(const godot::String &p_receiver_type, const godot::String &p_member,
			const godot::String &p_function_type, const godot::String &p_description) const;

	// Rebuilds script_warnings_by_path for one file, out of the export and signal lists the
	// analysis just landed for.
	void refresh_script_warnings(const godot::String &p_path) const;

	std::vector<VerseScript *> live_scripts;
	std::unordered_map<int64_t, VerseScriptInstance *> live_instances;

	// The doc carrier and the pages it holds, for publish_api_method. The carrier is a VerseScript
	// with no file, kept out of live_scripts so the build and analysis walks never reach it; its
	// documentation is whatever api_doc_pages currently holds, one ClassDoc per Godot-package
	// receiver a hover has asked about. Mutable because a hover is const and is where they fill.
	mutable godot::Ref<VerseScript> api_doc_carrier;
	mutable godot::HashMap<godot::String, godot::Dictionary> api_doc_pages;

	// Adds "Godot has Node.foo, but ..." to any diagnostic that named a member the mirror
	// deliberately does not carry. R-SCN-2: the reason has to reach the author, not a report file.
	void explain_skipped_members(const godot::String &p_path, const godot::TypedArray<godot::Dictionary> &p_errors) const;

	// Set by an analysis that moved something the editor has already drawn -- diagnostics that
	// differ from the last one's, or a script that settled its validity on this result -- and
	// cleared by the _frame that asks the script editor to draw it again. Godot has no reason of
	// its own to re-ask once the author stops typing, so without this an error survives its own
	// fix on screen and the documentation stays a save behind.
	mutable bool editor_refresh_pending = false;

	// The same arrangement for the *documentation*, which is a separate question because Godot
	// asks for it once per session and off the editor's thread. Set by note_script_docs_deferred,
	// taken by the _frame that re-registers every script's documentation.
	mutable std::atomic<bool> script_docs_deferred{ false };
	mutable bool docs_refresh_pending = false;

	// Whether the republish has already been tried against the program as it now stands. Godot's
	// one pass can be the last thing that ever asks -- an editor opened and left alone starts no
	// analysis of its own -- so waiting for one to land is not enough, and retrying every frame
	// until a class can be described is too much. Cleared by a landed analysis and by a published
	// generation, which are the two things that change the answer.
	mutable bool docs_refresh_attempted = false;

	// Module path per res:// script path, and whether it has been derived at all this session.
	// Mutable because verse_class_name() is const and every lookup goes through it.
	mutable std::map<std::string, std::string> module_by_script;
	mutable bool module_map_built = false;

	// script_class_names' answer, and whether it still describes res://.
	mutable godot::PackedStringArray script_class_names_cache;
	mutable bool script_class_names_built = false;

	// Whether EditorFileSystem's filesystem_changed has been hooked up yet. Not at _init: the
	// editor's singletons do not exist when a ScriptLanguage is registered.
	bool filesystem_hook_connected = false;

	// R-DIAG-4's state and behaviour, all of it: see verse_debugger.h. Mutable for the same reason
	// project_state is -- Godot's virtuals are const and nearly every one of them touches it.
	mutable VerseDebugger debugger{ *this };

	// The profiler's state and behaviour: see verse_profiler.h. Not mutable -- none of the
	// `_profiling_*` virtuals it backs is const.
	VerseProfiler profiler;

	// The import _frame is about to write, as the res:// path of the file and the module path to
	// import. One at a time: the next analysis reports whatever is still unresolved.
	mutable godot::String pending_import_path;
	mutable godot::String pending_import_module;
	// Every (file, module) this session has already offered, so an import the author deletes is
	// not put straight back and a stale diagnostic does not insert a second copy.
	mutable std::unordered_map<std::string, bool> offered_imports;

	// Writes one file's build diagnostics to the output log by severity. The build is the only
	// caller: an analysis's results reach the author through the script editor alone.
	void log_build_diagnostics(const godot::TypedArray<godot::Dictionary> &p_diagnostics) const;

	// R-TOOL-12. Looks through a file's fresh diagnostics for an unknown identifier that one of
	// the project's modules declares, and queues the `using` that would fix it -- goimports'
	// shape, reacting to the diagnostic rather than to the keystroke, because Godot's completion
	// API carries no edit-on-accept hook to hang it on.
	//
	// Queued rather than written: this runs inside a validate, and writing into the buffer the
	// editor is mid-validate on is not somewhere to do it. _frame performs the insertion.
	//
	// Only when exactly one module declares the name. Two modules declaring one name is legal --
	// it is the point of modules -- and only the author knows which was meant.
	void note_missing_imports(const godot::String &p_path, const godot::TypedArray<godot::Dictionary> &p_errors) const;

	// Performs the queued insertion, if the script editor is still showing the file it is for.
	// Insert only: nothing is ever removed, because removing a line the author may have written
	// by hand is a different and worse promise.
	void insert_pending_import() const;

	// Every file under p_dir whose extension is p_extension, lowercased. The `.verse` walk and the
	// `.vmodule` walk are the same walk with a different answer.
	static godot::PackedStringArray find_project_files(const godot::String &p_dir, const godot::String &p_extension);

	// Every res:// path of a script whose *file stem* is p_class_name. More than one is possible
	// once modules exist -- gameplay/player.verse and ui/player.verse both answer to `player` --
	// and telling the callers apart is the caller's problem, because only one of them can do
	// anything about it. A linear walk rather than a cached map: it is only reached for a script
	// whose superclass is another script, which the generated Godot API never is.
	godot::PackedStringArray script_paths_for_class(const godot::String &p_class_name) const;

	// Re-reads the .vmodule markers and re-derives which module every script is in, reporting any
	// marker whose stem is not a Verse identifier. Called by every build, and once more by a
	// lookup for a path the map has never seen -- which is a script added since the last build.
	void refresh_module_map() const;

	// Complains about two files in one module claiming one class name, and about two
	// @global_class classes claiming one Godot name. Both are project-wide questions that only a
	// pass over every source can answer, so they ride along with the build, which reads them all
	// anyway. p_sources and p_texts run parallel.
	// Every script warning once per build, in the log. `_validate`'s copy reaches the gutter and
	// nothing else, so this is the half a test can read.
	void log_script_warnings(const godot::PackedStringArray &p_sources) const;
	void report_name_collisions(const godot::PackedStringArray &p_sources,
			const std::vector<std::string> &p_texts) const;
};
