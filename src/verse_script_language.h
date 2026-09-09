#pragma once

#include <godot_cpp/classes/script_language_extension.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/typed_array.hpp>

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

	// Pumps the Verse event loop once per frame for the whole language. Phase 2 did this from a
	// VerseTicker node; doing it here is what makes every scripted node driven instead of one.
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
	// when there is no host to ask.
	godot::TypedArray<godot::Dictionary> check_buffer(const godot::String &p_path, const godot::String &p_source) const;

	godot::TypedArray<godot::Dictionary> diagnostics_for(const godot::String &p_path) const;

private:
	static VerseScriptLanguage *singleton_instance;
	double frame_budget_ms = 4.0;
	bool project_built = false;
	godot::Dictionary diagnostics_by_path;

	static godot::PackedStringArray find_verse_sources(const godot::String &p_dir);
};
