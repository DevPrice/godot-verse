#pragma once

// _make_template returns a Ref<Script>, and Ref's destructor needs the complete type;
// script_language_extension.hpp only forward-declares it.
#include <godot_cpp/classes/script.hpp>
#include <godot_cpp/classes/script_language_extension.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/typed_array.hpp>

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

// The Godot class a mirrored Verse class name stands for, or nullptr for a name that is not part of
// the generated API -- a class the author wrote, most often. Shared rather than looked up twice:
// verse_script.cpp turns an export's class hint into the name an inspector slot filters by, and
// that has to be the same table the rest of the bridge resolves a Verse class name through.
const char *verse_godot_class_for(const godot::String &p_verse_class);

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

	// Verse debugs through uLangDAP against the host, not through Godot's debugger. These
	// exist because Godot treats them as required and errors at the call site — including from
	// inside its own error reporting — when a script language leaves one unbound.
	godot::String _debug_get_error() const override;
	int32_t _debug_get_stack_level_count() const override;
	int32_t _debug_get_stack_level_line(int32_t p_level) const override;
	godot::String _debug_get_stack_level_function(int32_t p_level) const override;
	godot::String _debug_get_stack_level_source(int32_t p_level) const override;
	godot::Dictionary _debug_get_stack_level_locals(int32_t p_level, int32_t p_max_subitems, int32_t p_max_depth) override;
	godot::Dictionary _debug_get_stack_level_members(int32_t p_level, int32_t p_max_subitems, int32_t p_max_depth) override;
	void *_debug_get_stack_level_instance(int32_t p_level) override;
	godot::Dictionary _debug_get_globals(int32_t p_max_subitems, int32_t p_max_depth) override;
	godot::String _debug_parse_stack_level_expression(int32_t p_level, const godot::String &p_expression, int32_t p_max_subitems, int32_t p_max_depth) override;
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

	// Budget handed to vh_tick each frame, so a runaway Verse task costs frame rate rather than
	// hanging the editor. Read from the verse/runtime/frame_budget_ms project setting at _init.
	double get_frame_budget_ms() const;

	// Builds every .verse file under res:// as one Verse program and publishes it as a new
	// generation. Verse's compilation unit is the package rather than the file, so one script
	// cannot be built alone and every build re-enumerates res:// -- which is what makes a file
	// added, renamed or deleted since the last build land without a restart (R-ITER-2).
	//
	// Called on Play and from the Build action, not on save: the whole project is ~200ms, and
	// GDScript only gets away with building on save because its unit is one file. What a save
	// does instead is refresh analysis, which keeps diagnostics, completion and the export
	// *shape* live per keystroke.
	//
	// A failed build publishes nothing, so the last generation that did keeps running (R-ITER-5).
	godot::Error build_project();

	// build_project the first time and the remembered result after, for the callers that need a
	// program to exist but have no business deciding when a new one is published.
	godot::Error ensure_project_built();

	// Errors the project build reported against one script, in the shape _validate returns.
	// The build is the only thing that ever produces them: asking the compiler again for a
	// single file would report every other script's definitions as duplicates.
	// Re-analyses the project with p_path's on-disk text replaced by the editor's buffer and
	// returns just that file's diagnostics. Falls back to the diagnostics recorded at startup
	// when there is no host to ask, and skips the re-analysis entirely when the host already
	// holds this exact text.
	godot::TypedArray<godot::Dictionary> check_buffer(const godot::String &p_path, const godot::String &p_source) const;

	// The classes a `.` at p_receiver_end reaches a member of, nearest first, or empty when the
	// buffer does not say which. Text and the analysis snapshot only: this is what completion has
	// to answer from before any analysis of the buffer in front of the author exists.
	godot::PackedStringArray receiver_classes_from_text(const godot::String &p_source, const godot::String &p_path, int64_t p_receiver_end) const;

	godot::TypedArray<godot::Dictionary> diagnostics_for(const godot::String &p_path) const;

	// The compiler's warnings against one script, in the same shape as its errors; _validate
	// reshapes them into the warnings array, which Godot reads with different keys.
	godot::TypedArray<godot::Dictionary> compiler_warnings_for(const godot::String &p_path) const;

	// Whether the host's last analysis answered for exactly p_source as p_path's text, so
	// diagnostics_for describes that text and not the one before it. True as well when there is
	// no host to ask: the build's diagnostics are then the only answer there will ever be, and a
	// caller that waits for a better one waits forever.
	bool analysis_is_current(const godot::String &p_path, const godot::String &p_source) const;

	// Queues p_source as p_path's text for analysis and returns. _frame is what hands it to the
	// host, and a later one is where the result lands and every script awaiting it is told.
	// Nothing starts on this thread: an analysis blocks the VM for its whole length, so one begun
	// in the middle of the editor's work stops the pump and every `@tool` instance until it lands.
	void queue_check(const godot::String &p_path, const godot::String &p_source) const;

	// Scripts that read their validity and export list out of the analysis, so a result landing
	// in _frame reaches them. Borrowed: a script adds itself on construction and removes itself
	// on destruction.
	void register_script(VerseScript *p_script);
	void unregister_script(VerseScript *p_script);

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
	double frame_budget_ms = 4.0;
	bool project_built = false;
	// What the last build came back with, so ensure_project_built can answer without publishing
	// a generation of its own.
	godot::Error project_build_status = godot::OK;
	mutable godot::Dictionary diagnostics_by_path;
	// The compiler's warnings, keyed and shaped the same way and recorded by the same analysis.
	// Kept apart from the errors because diagnostics_for is what decides a script's validity.
	mutable godot::Dictionary compiler_warnings_by_path;

	// The text the host currently holds for each script, keyed by res:// path. A validate whose
	// buffer already matches it needs no re-analysis: the host's last analysis answered for
	// exactly these sources. Godot validates on open, on every tab switch, on an idle timer and
	// on save, while a whole-project semantic analysis costs ~750 ms whether anything changed or
	// not. Nothing on the editor's thread waits for one any more, but it blocks the VM for its
	// whole length, so without this every tab switch costs a `@tool` script that long not running.
	mutable godot::Dictionary analyzed_source_by_path;

	// res:// path for each absolute path the host reports diagnostics against.
	godot::Dictionary path_by_globalized;

	// Warnings for the members a script declared and the host refused -- an `@export` the inspector
	// cannot draw, a `signal` Godot cannot register -- keyed by res:// path and shaped the
	// way _validate hands one over. Harvested when an analysis lands rather than asked for at
	// _validate: both lists describe the analysis the snapshot came from, so taking them in the
	// poll is what pairs a warning set with the diagnostics reported out of the same one.
	mutable godot::Dictionary script_warnings_by_path;

	// The buffer waiting for an analysis, and the one an analysis is running for. Only one runs
	// at a time, and a newer buffer replaces a waiting one rather than queueing behind it.
	mutable bool has_pending_check = false;
	mutable godot::String pending_check_path;
	mutable godot::String pending_check_source;
	mutable godot::String in_flight_path;
	mutable godot::String in_flight_source;

	// Whether that buffer is a completion buffer rather than the author's own text -- the half-typed
	// identifier replaced by the placeholder. Its diagnostics describe a line nobody has finished
	// writing, so poll_check drops them instead of drawing them; what it keeps is the record that
	// the host now holds this text, which is the whole reason the analysis was asked for.
	mutable bool pending_check_is_completion = false;
	mutable bool in_flight_is_completion = false;

	// The completion buffer whose analysis is worth re-asking completion for once it lands, and
	// the file it belongs to. Empty when nothing is waiting on one.
	mutable godot::String completion_refresh_path;
	mutable godot::String completion_refresh_source;
	mutable bool completion_refresh_pending = false;

	// The completion buffer the last vh_complete_symbol answered for, with its position, mode and
	// answer. Godot re-asks on every keystroke while the popup is open, and each ask costs a
	// whole-project analysis; normalizing the half-typed identifier out of the buffer is what
	// makes the whole of one prefix the same question, and this is what makes it free to repeat.
	mutable godot::String completion_cache_source;
	mutable int32_t completion_cache_line = -1;
	mutable int32_t completion_cache_column = -1;
	mutable int32_t completion_cache_mode = -1;
	mutable godot::TypedArray<godot::Dictionary> completion_cache_options;

	// The same, for the argument hint. Kept apart because the two are asked about different
	// positions in one buffer -- the callee for the hint, the cursor for the options -- and the
	// host answers both off a single analysis, so caching them together would throw one away.
	mutable godot::String signature_cache_source;
	mutable int32_t signature_cache_line = -1;
	mutable int32_t signature_cache_column = -1;
	mutable godot::Dictionary signature_cache;

	// Queues p_path's buffer for analysis. p_is_completion marks it as a completion buffer: see
	// pending_check_is_completion.
	void request_check(const godot::String &p_path, const godot::String &p_normalized_source, bool p_is_completion = false) const;
	void start_pending_check() const;

	// Asks the open script editor to complete again, if it is still showing the file the landed
	// completion analysis was for and the caret is still inside the same identifier. That second
	// _complete_code finds the host describing the buffer and replaces the partial list in place.
	void refresh_completion_if_current() const;

	// Reaps a finished analysis and starts whatever came in while it ran. Called once per frame.
	void poll_check() const;

	// Rebuilds script_warnings_by_path for one file, out of the export and signal lists the
	// analysis just landed for.
	void refresh_script_warnings(const godot::String &p_path) const;

	std::vector<VerseScript *> live_scripts;
	std::unordered_map<int64_t, VerseScriptInstance *> live_instances;

	// Replaces diagnostics_by_path and compiler_warnings_by_path with one analysis' results,
	// sorted by severity. Analysis covers the whole project, so a file absent from the result
	// has no errors and must lose any it had. Returns whether anything the editor draws
	// actually moved.
	bool record_diagnostics(const godot::Dictionary &p_diagnostics_by_globalized) const;

	// Adds "Godot has Node.foo, but ..." to any diagnostic that named a member the mirror
	// deliberately does not carry. R-SCN-2: the reason has to reach the author, not a report file.
	static void explain_skipped_members(const godot::TypedArray<godot::Dictionary> &p_errors);

	// Set by an analysis that moved something the editor has already drawn -- diagnostics that
	// differ from the last one's, or a script that settled its validity on this result -- and
	// cleared by the _frame that asks the script editor to draw it again. Godot has no reason of
	// its own to re-ask once the author stops typing, so without this an error survives its own
	// fix on screen and the documentation stays a save behind.
	mutable bool editor_refresh_pending = false;

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

	static godot::PackedStringArray find_verse_sources(const godot::String &p_dir);
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
	void report_name_collisions(const godot::PackedStringArray &p_sources,
			const std::vector<std::string> &p_texts) const;
};
