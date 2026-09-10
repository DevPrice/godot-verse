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

struct VerseClassDecl;

// Line endings normalised away. A buffer arrives through TextEdit, which need not hand back the
// endings the file was written with, and Verse analyses identically either way.
godot::String verse_newline_normalized(const godot::String &p_source);

// The run of comment lines immediately above p_line, delimiters removed. Verse has no doc-comment
// form of its own, so this convention -- whatever precedes a definition documents it -- is the
// whole of where a script's documentation comes from.
godot::String verse_doc_comment_above(const godot::String &p_source, int64_t p_line);

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

	// Budget handed to vh_tick each frame, so a runaway Verse task costs frame rate rather than
	// hanging the editor. Read from the verse/runtime/frame_budget_ms project setting at _init.
	double get_frame_budget_ms() const;

	// Compiles every .verse file under res:// as one Verse program, once. Verse's compilation
	// unit is the package rather than the file, and a second build in the same process aborts
	// the engine, so the first script that needs compiling pays for all of them.
	godot::Error ensure_project_built();

	// Errors the project build reported against one script, in the shape _validate returns.
	// The build is the only thing that ever produces them: asking the compiler again for a
	// single file would report every other script's definitions as duplicates.
	// Re-analyses the project with p_path's on-disk text replaced by the editor's buffer and
	// returns just that file's diagnostics. Falls back to the diagnostics recorded at startup
	// when there is no host to ask, and skips the re-analysis entirely when the host already
	// holds this exact text.
	godot::TypedArray<godot::Dictionary> check_buffer(const godot::String &p_path, const godot::String &p_source) const;

	godot::TypedArray<godot::Dictionary> diagnostics_for(const godot::String &p_path) const;

	// The class name every .verse file under res:// defines, which is its own stem: a script's
	// class is named after its file, and one flat scope for the whole project is what forces
	// that. Cheap enough to answer from a directory walk, and it needs no compiled program --
	// the syntax highlighter has to colour a script that has never been built.
	godot::PackedStringArray script_class_names() const;

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

	// Blocks until every outstanding analysis has landed, so diagnostics_for answers for the
	// current text rather than the text before the last edit. Saving and reloading are worth a
	// wait: they are explicit, they are where a script's validity is decided, and answering them
	// from a superseded analysis is how an already-undone error reaches the output log.
	void settle_checks() const;

private:
	static VerseScriptLanguage *singleton_instance;
	double frame_budget_ms = 4.0;
	bool project_built = false;
	// What the one build this process gets came back with. Remembered because there is no second
	// attempt to ask again: every later ensure_project_built answers from here.
	godot::Error project_build_status = godot::OK;
	mutable godot::Dictionary diagnostics_by_path;

	// The text the host currently holds for each script, keyed by res:// path. A validate whose
	// buffer already matches it needs no re-analysis: the host's last analysis answered for
	// exactly these sources. Godot validates on open, on every tab switch, on an idle timer and
	// on save, while a whole-project semantic analysis costs ~100ms whether anything changed or
	// not, so without this the editor stalls on each of them.
	mutable godot::Dictionary analyzed_source_by_path;

	// res:// path for each absolute path the host reports diagnostics against.
	godot::Dictionary path_by_globalized;

	// The buffer waiting for an analysis, and the one an analysis is running for. Only one runs
	// at a time, and a newer buffer replaces a waiting one rather than queueing behind it.
	mutable bool has_pending_check = false;
	mutable godot::String pending_check_path;
	mutable godot::String pending_check_source;
	mutable godot::String in_flight_path;
	mutable godot::String in_flight_source;

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

	// Queues p_path's buffer for analysis and starts it if the host is free.
	void request_check(const godot::String &p_path, const godot::String &p_normalized_source) const;
	void start_pending_check() const;

	// Reaps a finished analysis and starts whatever came in while it ran. Called once per frame.
	void poll_check() const;

	// Formatted diagnostics last written to the output log, keyed by globalized path.
	mutable godot::Dictionary logged_diagnostics;

	// Replaces diagnostics_by_path with one analysis' results. Analysis covers the whole project,
	// so a file absent from the result has no errors and must lose any it had.
	void record_diagnostics(const godot::Dictionary &p_errors_by_globalized) const;

	void log_new_diagnostics(const godot::String &p_globalized_path, const godot::TypedArray<godot::Dictionary> &p_errors) const;

	static godot::PackedStringArray find_verse_sources(const godot::String &p_dir);

	// The res:// path of the script defining p_class_name, or empty. A linear walk of the project
	// rather than a cached map: it is only reached for a script whose superclass is another
	// script, which the generated Godot API never is.
	godot::String script_path_for_class(const godot::String &p_class_name) const;
};
