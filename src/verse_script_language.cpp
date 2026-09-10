#include "verse_script_language.h"

#include "verse_api_classes.h"
#include "verse_keywords.h"
#include "verse_runtime.h"
#include "verse_script.h"

#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/memory.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <iterator>

using namespace godot;

namespace {

VerseRuntime *get_runtime() {
	return Object::cast_to<VerseRuntime>(Engine::get_singleton()->get_singleton("VerseRuntime"));
}

// A validate's buffer arrives via TextEdit, which need not hand back the line endings the file
// was written with, so a CRLF file compared raw would miss the cache on every keystroke and
// every tab switch. Verse is newline-agnostic, so text differing only in line endings analyses
// identically and is safe to treat as unchanged.
String newline_normalized(const String &p_source) {
	return p_source.replace("\r\n", "\n").replace("\r", "\n");
}

// The run of comment lines immediately above p_line, with the delimiters taken off so a hover
// shows prose rather than syntax. Verse has no doc-comment form of its own, so this is the whole
// convention: whatever precedes a definition documents it.
//
// Read out of the source rather than asked of the compiler. The parser does keep comments, but it
// hangs one off whichever node begins the construct, and for a member behind four lines of
// `@editable` and friends that is the attribute clause rather than the member -- so recovering
// the association costs more Vst archaeology than re-reading four lines of text.
//
// A `<# #>` block contributes only the lines that open with its delimiter; a continuation line
// reads as ordinary text and stops the walk, which is the conservative direction to be wrong in.
String doc_comment_above(const String &p_source, int64_t p_line) {
	const PackedStringArray lines = p_source.split("\n");
	PackedStringArray collected;

	for (int64_t i = p_line - 1; i >= 0 && i < lines.size(); i--) {
		String line = lines[i].strip_edges();
		if (line.begins_with("<#>")) {
			line = line.substr(3);
		} else if (line.begins_with("<#")) {
			line = line.substr(2).trim_suffix("#>");
		} else if (line.begins_with("#")) {
			line = line.substr(1);
		} else {
			break;
		}
		collected.push_back(line.strip_edges());
	}

	collected.reverse();
	return String("\n").join(collected).strip_edges();
}

const char *mirrored_class(const String &p_godot_class) {
	for (size_t i = 0; i < std::size(verse_api::classes); i++) {
		if (p_godot_class == verse_api::classes[i].godot_name) {
			return verse_api::classes[i].verse_name;
		}
	}
	return nullptr;
}

// Only a subset of Godot's classes is mirrored, so a node whose own class was not generated
// inherits from the nearest ancestor that was. `node` is the floor: every scripted node has
// one, and a template that names a class the project does not define would not compile.
String verse_base_class_for(const String &p_godot_class) {
	for (String name = p_godot_class; !name.is_empty(); name = ClassDB::get_parent_class(name)) {
		if (const char *mirrored = mirrored_class(name)) {
			return String(mirrored);
		}
	}
	return String("node");
}

} // namespace

VerseScriptLanguage *VerseScriptLanguage::singleton_instance = nullptr;

VerseScriptLanguage::VerseScriptLanguage() {
	singleton_instance = this;
}

VerseScriptLanguage::~VerseScriptLanguage() {
	singleton_instance = nullptr;
}

VerseScriptLanguage *VerseScriptLanguage::singleton() {
	return singleton_instance;
}

void VerseScriptLanguage::_bind_methods() {
}

String VerseScriptLanguage::_get_name() const {
	return "Verse";
}

String VerseScriptLanguage::_get_type() const {
	return "VerseScript";
}

String VerseScriptLanguage::_get_extension() const {
	return "verse";
}

PackedStringArray VerseScriptLanguage::_get_recognized_extensions() const {
	PackedStringArray extensions;
	extensions.push_back("verse");
	return extensions;
}

void VerseScriptLanguage::_init() {
	ProjectSettings *settings = ProjectSettings::get_singleton();

	const String setting_name = "verse/runtime/frame_budget_ms";
	const double default_budget_ms = 4.0;
	if (!settings->has_setting(setting_name)) {
		settings->set_setting(setting_name, default_budget_ms);
	}
	settings->set_initial_value(setting_name, default_budget_ms);
	Dictionary property_info;
	property_info["name"] = setting_name;
	property_info["type"] = (int64_t)Variant::FLOAT;
	property_info["hint"] = (int64_t)PROPERTY_HINT_NONE;
	property_info["hint_string"] = String();
	settings->add_property_info(property_info);

	frame_budget_ms = settings->get_setting(setting_name);
}

void VerseScriptLanguage::_finish() {
}

PackedStringArray VerseScriptLanguage::_get_reserved_words() const {
	PackedStringArray words;
	for (size_t i = 0; i < std::size(verse_keywords::reserved_words); i++) {
		words.push_back(verse_keywords::reserved_words[i]);
	}
	return words;
}

bool VerseScriptLanguage::_is_control_flow_keyword(const String &p_keyword) const {
	for (size_t i = 0; i < std::size(verse_keywords::control_flow_words); i++) {
		if (p_keyword == verse_keywords::control_flow_words[i]) {
			return true;
		}
	}
	return false;
}

PackedStringArray VerseScriptLanguage::_get_comment_delimiters() const {
	PackedStringArray delimiters;
	delimiters.push_back("#");
	delimiters.push_back("<# #>");
	// Verse's <#> indented comment ends at a dedent rather than a closing token, so it has no
	// delimiter pair a start/end entry can express and is left out.
	return delimiters;
}

PackedStringArray VerseScriptLanguage::_get_doc_comment_delimiters() const {
	return PackedStringArray();
}

PackedStringArray VerseScriptLanguage::_get_string_delimiters() const {
	PackedStringArray delimiters;
	delimiters.push_back("\" \"");
	delimiters.push_back("' '");
	return delimiters;
}

Ref<Script> VerseScriptLanguage::_make_template(const String &p_template, const String &p_class_name, const String &p_base_class_name) const {
	const String class_name = p_class_name.is_empty() ? String("script") : p_class_name;

	String source =
			"using { /Godot.org/Godot }\n"
			"\n"
			"# The class is named after this file, which is how the node it is attached to finds it.\n"
			"_CLASS_ := class(_BASE_):\n"
			"\n"
			"    Ready<override>():void =\n"
			"        Print(\"_CLASS_ is ready\")\n"
			"\n"
			"    Update<override>(Delta:float):void =\n";
	source = source.replace("_CLASS_", class_name.to_snake_case());
	source = source.replace("_BASE_", verse_base_class_for(p_base_class_name));

	VerseScript *script = memnew(VerseScript);
	script->set_source_code(source);
	return Ref<Script>(script);
}

bool VerseScriptLanguage::_is_using_templates() {
	return true;
}

Object *VerseScriptLanguage::_create_script() const {
	return memnew(VerseScript);
}

Dictionary VerseScriptLanguage::_validate(const String &p_script, const String &p_path, bool p_validate_functions, bool p_validate_errors, bool p_validate_warnings, bool p_validate_safe_lines) const {
	const TypedArray<Dictionary> errors = check_buffer(p_path, p_script);

	Dictionary result;
	result["valid"] = errors.is_empty();
	result["errors"] = errors;
	return result;
}

bool VerseScriptLanguage::_has_named_classes() const {
	return false;
}

bool VerseScriptLanguage::_supports_builtin_mode() const {
	return false;
}

bool VerseScriptLanguage::_supports_documentation() const {
	return false;
}

bool VerseScriptLanguage::_can_inherit_from_file() const {
	return false;
}

bool VerseScriptLanguage::_can_make_function() const {
	return false;
}

int32_t VerseScriptLanguage::_find_function(const String &p_function, const String &p_code) const {
	const PackedStringArray lines = p_code.split("\n");
	const int64_t name_length = p_function.length();

	for (int64_t i = 0; i < lines.size(); i++) {
		const String stripped = lines[i].strip_edges();
		if (!stripped.begins_with(p_function) || stripped.length() <= name_length) {
			continue;
		}

		const char32_t next = stripped[name_length];
		if (next == '(' || next == '<') {
			return (int32_t)(i + 1);
		}
	}

	return -1;
}

String VerseScriptLanguage::_auto_indent_code(const String &p_code, int32_t p_from_line, int32_t p_to_line) const {
	return p_code;
}

ScriptLanguage::ScriptNameCasing VerseScriptLanguage::_preferred_file_name_casing() const {
	return ScriptLanguage::SCRIPT_NAME_CASING_SNAKE_CASE;
}

void VerseScriptLanguage::_add_global_constant(const StringName &p_name, const Variant &p_value) {
}

void VerseScriptLanguage::_add_named_global_constant(const StringName &p_name, const Variant &p_value) {
}

void VerseScriptLanguage::_remove_named_global_constant(const StringName &p_name) {
}

// The host ABI requires every vh_* call to come from the thread that called vh_init, so there is
// nothing per-thread to set up or tear down here.
void VerseScriptLanguage::_thread_enter() {
}

void VerseScriptLanguage::_thread_exit() {
}

void VerseScriptLanguage::_reload_all_scripts() {
}

void VerseScriptLanguage::_reload_tool_script(const Ref<Script> &p_script, bool p_soft_reload) {
	VerseScript *script = Object::cast_to<VerseScript>(p_script.ptr());
	if (script != nullptr) {
		script->compile();
	}
}

void VerseScriptLanguage::_reload_scripts(const Array &p_scripts, bool p_soft_reload) {
	for (int64_t i = 0; i < p_scripts.size(); i++) {
		VerseScript *script = Object::cast_to<VerseScript>(p_scripts[i]);
		if (script != nullptr) {
			script->compile();
		}
	}
}

String VerseScriptLanguage::_validate_path(const String &p_path) const {
	return String();
}

TypedArray<Dictionary> VerseScriptLanguage::_get_built_in_templates(const StringName &p_object) const {
	return TypedArray<Dictionary>();
}

String VerseScriptLanguage::_make_function(const String &p_class_name, const String &p_function_name, const PackedStringArray &p_function_args) const {
	return String();
}

Error VerseScriptLanguage::_open_in_external_editor(const Ref<Script> &p_script, int32_t p_line, int32_t p_column) {
	return ERR_UNAVAILABLE;
}

bool VerseScriptLanguage::_overrides_external_editor() {
	return false;
}

Dictionary VerseScriptLanguage::_complete_code(const String &p_code, const String &p_path, Object *p_owner) const {
	return Dictionary();
}

// Ctrl+click, the ctrl-hover underline and the documentation tooltip are all this one call.
//
// Godot reads "result" and "type" back out unconditionally and logs ERR_UNAVAILABLE when either
// is missing, so an answer is mandatory even when there is nothing to say. The refusal is spelled
// with a real type rather than LOOKUP_RESULT_MAX because newer Godot bounds-checks the value and
// would turn every hover into the error spam this used to be written to avoid.
//
// The two live types are the only ones that serve both features: SCRIPT_LOCATION jumps but shows
// no tooltip at all, and the CLASS_* types route into Godot's own class documentation, which has
// nothing to say about a Verse definition. LOCAL_VARIABLE and LOCAL_CONSTANT build a tooltip out
// of doc_type/description, and the click path ignores `type` entirely -- it jumps on `location`
// alone, provided `class_name` is empty. Leaving class_name unset is therefore load-bearing.
Dictionary VerseScriptLanguage::_lookup_code(const String &p_code, const String &p_symbol, const String &p_path, Object *p_owner) const {
	Dictionary result;
	result["result"] = (int64_t)ERR_UNAVAILABLE;
	result["type"] = (int64_t)ScriptLanguageExtension::LOOKUP_RESULT_LOCAL_VARIABLE;

	VerseRuntime *runtime = get_runtime();
	if (!project_built || runtime == nullptr || !runtime->is_host_loaded()) {
		return result;
	}

	// The editor marks the cursor by splicing U+FFFF into the buffer it hands over, and that is
	// the only place the position arrives: p_symbol is just the word under the pointer, which
	// cannot tell two same-named locals in different functions apart. The underline path asks
	// about the mouse rather than the caret and hands over an empty string when the pointer is
	// off the end of the text, so a missing marker is ordinary rather than a fault.
	const int64_t marker = p_code.find(String::chr(0xFFFF));
	if (marker < 0) {
		return result;
	}

	const String before = p_code.substr(0, marker);
	const int64_t line = before.count("\n");
	const int64_t line_start = before.rfind("\n") + 1;
	// Godot counts the column in characters and the compiler counts it in utf8 bytes; one
	// non-ASCII character earlier on the line is enough to make them disagree.
	const int64_t column = before.substr(line_start).utf8().length();

	// Answering from an analysis that predates the edit would be worse than not answering: the
	// loci below an inserted row are all shifted, so the jump lands confidently on the wrong
	// line. This is the same predicate check_buffer uses to decide a re-analysis is unnecessary.
	const String normalized = newline_normalized(before + p_code.substr(marker + 1));
	if (!analyzed_source_by_path.has(p_path) || String(analyzed_source_by_path[p_path]) != normalized) {
		return result;
	}

	// The host blocks on an in-flight analysis before touching the semantic program, and this
	// runs on the editor's thread. An analysis of some other file is the one case where the
	// buffer can be current and the host still busy; declining costs an underline for a frame.
	if (runtime->is_check_project_busy()) {
		return result;
	}

	const String globalized = ProjectSettings::get_singleton()->globalize_path(p_path);
	const Dictionary found = runtime->lookup_symbol(globalized, (int32_t)line, (int32_t)column);
	if (found.is_empty()) {
		return result;
	}

	result["result"] = (int64_t)OK;
	result["type"] = (int64_t)(bool(found["is_var"])
					? ScriptLanguageExtension::LOOKUP_RESULT_LOCAL_VARIABLE
					: ScriptLanguageExtension::LOOKUP_RESULT_LOCAL_CONSTANT);
	result["doc_type"] = found["type"];

	const int64_t definition_line = found["line"];
	const String definition_path = found["path"];
	if (definition_line < 0 || definition_path.is_empty()) {
		// A definition from the generated Godot API or Verse's own library: it describes, but
		// there is no file in the project to open or to read a comment out of.
		return result;
	}

	// A location with no script beside it is read as a line in the file being edited, so a
	// cross-file definition we cannot name gets no location at all rather than a jump to that
	// line of the wrong file.
	const bool same_file = definition_path == globalized;
	const String definition_res_path = same_file ? p_path : String(path_by_globalized.get(definition_path, String()));
	if (definition_res_path.is_empty()) {
		return result;
	}

	result["location"] = definition_line + 1;
	if (!same_file) {
		result["script"] = ResourceLoader::get_singleton()->load(definition_res_path);
		result["script_path"] = definition_res_path;
	}

	// The buffer for the file being edited, which may be ahead of what is on disk; anything else
	// has to come off disk, and is at worst as stale as the analysis that pointed here.
	const String definition_source = same_file
			? normalized
			: newline_normalized(FileAccess::get_file_as_string(definition_res_path));
	const String description = doc_comment_above(definition_source, definition_line);
	if (!description.is_empty()) {
		result["description"] = description;
	}
	return result;
}

String VerseScriptLanguage::_debug_get_error() const {
	return String();
}

int32_t VerseScriptLanguage::_debug_get_stack_level_count() const {
	return 0;
}

int32_t VerseScriptLanguage::_debug_get_stack_level_line(int32_t p_level) const {
	return 0;
}

String VerseScriptLanguage::_debug_get_stack_level_function(int32_t p_level) const {
	return String();
}

String VerseScriptLanguage::_debug_get_stack_level_source(int32_t p_level) const {
	return String();
}

Dictionary VerseScriptLanguage::_debug_get_stack_level_locals(int32_t p_level, int32_t p_max_subitems, int32_t p_max_depth) {
	return Dictionary();
}

Dictionary VerseScriptLanguage::_debug_get_stack_level_members(int32_t p_level, int32_t p_max_subitems, int32_t p_max_depth) {
	return Dictionary();
}

void *VerseScriptLanguage::_debug_get_stack_level_instance(int32_t p_level) {
	return nullptr;
}

Dictionary VerseScriptLanguage::_debug_get_globals(int32_t p_max_subitems, int32_t p_max_depth) {
	return Dictionary();
}

String VerseScriptLanguage::_debug_parse_stack_level_expression(int32_t p_level, const String &p_expression, int32_t p_max_subitems, int32_t p_max_depth) {
	return String();
}

TypedArray<Dictionary> VerseScriptLanguage::_debug_get_current_stack_info() {
	return TypedArray<Dictionary>();
}

void VerseScriptLanguage::_profiling_start() {
}

void VerseScriptLanguage::_profiling_stop() {
}

void VerseScriptLanguage::_profiling_set_save_native_calls(bool p_enable) {
}

int32_t VerseScriptLanguage::_profiling_get_accumulated_data(ScriptLanguageExtensionProfilingInfo *p_info_array, int32_t p_info_max) {
	return 0;
}

int32_t VerseScriptLanguage::_profiling_get_frame_data(ScriptLanguageExtensionProfilingInfo *p_info_array, int32_t p_info_max) {
	return 0;
}

bool VerseScriptLanguage::_handles_global_class_type(const String &p_type) const {
	return false;
}

Dictionary VerseScriptLanguage::_get_global_class_name(const String &p_path) const {
	return Dictionary();
}

TypedArray<Dictionary> VerseScriptLanguage::_get_public_functions() const {
	return TypedArray<Dictionary>();
}

Dictionary VerseScriptLanguage::_get_public_constants() const {
	return Dictionary();
}

TypedArray<Dictionary> VerseScriptLanguage::_get_public_annotations() const {
	return TypedArray<Dictionary>();
}

void VerseScriptLanguage::_frame() {
	VerseRuntime *runtime = get_runtime();
	if (runtime != nullptr && runtime->is_host_loaded()) {
		poll_check();
		runtime->tick(frame_budget_ms / 1000.0);
	}
}

double VerseScriptLanguage::get_frame_budget_ms() const {
	return frame_budget_ms;
}

PackedStringArray VerseScriptLanguage::find_verse_sources(const String &p_dir) {
	PackedStringArray found;

	Ref<DirAccess> dir = DirAccess::open(p_dir);
	if (dir.is_null()) {
		return found;
	}

	const PackedStringArray files = dir->get_files();
	for (int64_t i = 0; i < files.size(); i++) {
		if (files[i].get_extension().to_lower() == "verse") {
			found.push_back(p_dir.path_join(files[i]));
		}
	}

	const PackedStringArray subdirs = dir->get_directories();
	for (int64_t i = 0; i < subdirs.size(); i++) {
		// .godot holds the import cache and addons/ holds the extension's own binaries; neither
		// can contain project source, and both are large.
		if (subdirs[i].begins_with(".") || (p_dir == "res://" && subdirs[i] == "addons")) {
			continue;
		}
		found.append_array(find_verse_sources(p_dir.path_join(subdirs[i])));
	}

	return found;
}

Error VerseScriptLanguage::ensure_project_built() {
	if (project_built) {
		return OK;
	}

	VerseRuntime *runtime = get_runtime();
	if (runtime == nullptr) {
		return ERR_UNAVAILABLE;
	}
	if (!runtime->is_host_loaded()) {
		const Error host_status = runtime->load_host();
		if (host_status != OK) {
			return host_status;
		}
	}

	const PackedStringArray sources = find_verse_sources("res://");
	PackedStringArray globalized;
	ProjectSettings *settings = ProjectSettings::get_singleton();
	for (int64_t i = 0; i < sources.size(); i++) {
		globalized.push_back(settings->globalize_path(sources[i]));
	}

	// The host reports against the absolute path it was handed; scripts are keyed by res:// path.
	path_by_globalized.clear();
	for (int64_t i = 0; i < sources.size(); i++) {
		path_by_globalized[globalized[i]] = sources[i];
	}

	Dictionary errors_by_globalized;
	const Error status = runtime->compile_project(globalized, &errors_by_globalized);

	record_diagnostics(errors_by_globalized);

	// The host loaded each of these from disk just now, so this is the text it holds. Seeding it
	// here is what makes the *first* validate of a file free rather than only the repeats.
	analyzed_source_by_path.clear();
	for (int64_t i = 0; i < sources.size(); i++) {
		const String text = FileAccess::get_file_as_string(sources[i]);
		if (FileAccess::get_open_error() == OK) {
			analyzed_source_by_path[sources[i]] = newline_normalized(text);
		}
	}

	const Array reported = errors_by_globalized.keys();
	for (int64_t i = 0; i < reported.size(); i++) {
		log_new_diagnostics(reported[i], TypedArray<Dictionary>(errors_by_globalized[reported[i]]));
	}

	project_built = true;


	return status;
}

TypedArray<Dictionary> VerseScriptLanguage::check_buffer(const String &p_path, const String &p_source) const {
	VerseRuntime *runtime = get_runtime();
	if (!project_built || runtime == nullptr || !runtime->is_host_loaded()) {
		return diagnostics_for(p_path);
	}

	const String globalized = ProjectSettings::get_singleton()->globalize_path(p_path);
	const String normalized = newline_normalized(p_source);

	// The host still holds exactly this text, so its last analysis already answered for it. This
	// is the common case by far: opening a file, switching to its tab and saving it all validate
	// a buffer nothing has touched since the last analysis.
	const bool analysis_is_current = analyzed_source_by_path.has(p_path)
			&& String(analyzed_source_by_path[p_path]) == normalized;

	// Anything else needs a fresh analysis, which takes about as long as three frames. Start it
	// on the host's thread and answer from the last one: returning stale diagnostics for a moment
	// is a far smaller cost than freezing the editor on every keystroke. _frame picks the result
	// up, and Godot re-validates often enough that the fresh answer lands on its own.
	//
	// That answer is deliberately not logged. It describes whatever the file said before this
	// edit, which may be a mistake the author has already undone, and the output log has no way
	// to retract a line. The script editor's own error list is free to show it because Godot
	// replaces it wholesale on the next validate; the log is not.
	if (!analysis_is_current) {
		request_check(p_path, normalized);
		return diagnostics_for(p_path);
	}

	// Analysis covers the whole project, so a broken file elsewhere reports against its own path;
	// the editor asked about this one.
	const TypedArray<Dictionary> errors = diagnostics_for(p_path);
	log_new_diagnostics(globalized, errors);
	return errors;
}

void VerseScriptLanguage::settle_checks() const {
	VerseRuntime *runtime = get_runtime();
	if (runtime == nullptr || !runtime->is_host_loaded()) {
		return;
	}

	// Two passes is the whole outstanding set: one analysis in flight, and at most one queued
	// buffer behind it, since a newer buffer replaces a waiting one rather than queueing.
	for (int pass = 0; pass < 2; pass++) {
		start_pending_check();
		if (in_flight_path.is_empty()) {
			return;
		}

		// An analysis of this project takes ~100ms. The cap is not a real duration so much as a
		// promise that a wedged host costs a stale error list rather than an editor that never
		// comes back.
		const uint64_t deadline_ms = Time::get_singleton()->get_ticks_msec() + 5000;
		while (runtime->is_check_project_busy()) {
			if (Time::get_singleton()->get_ticks_msec() > deadline_ms) {
				return;
			}
			OS::get_singleton()->delay_msec(1);
		}
		poll_check();
	}
}

void VerseScriptLanguage::request_check(const String &p_path, const String &p_normalized_source) const {
	// The analysis in flight is already for this exact text. Godot validates the same unchanged
	// buffer several times over while one runs, and queueing behind it would buy the same answer
	// a second time -- which a save then has to wait through.
	if (p_path == in_flight_path && p_normalized_source == in_flight_source) {
		return;
	}

	// Newest buffer wins: while an analysis runs the editor keeps typing, and every intermediate
	// state is worth less than the one the author is looking at now.
	pending_check_path = p_path;
	pending_check_source = p_normalized_source;
	has_pending_check = true;
	start_pending_check();
}

void VerseScriptLanguage::start_pending_check() const {
	if (!has_pending_check) {
		return;
	}

	VerseRuntime *runtime = get_runtime();
	if (runtime == nullptr || !runtime->is_host_loaded() || runtime->is_check_project_busy()) {
		return;
	}

	const String globalized = ProjectSettings::get_singleton()->globalize_path(pending_check_path);
	if (runtime->begin_check_project(globalized, pending_check_source) != OK) {
		return;
	}

	// The host has taken this text, so it is what the next result answers for.
	in_flight_path = pending_check_path;
	in_flight_source = pending_check_source;
	has_pending_check = false;
}

void VerseScriptLanguage::poll_check() const {
	VerseRuntime *runtime = get_runtime();
	if (runtime == nullptr || !runtime->is_host_loaded()) {
		return;
	}

	Dictionary errors_by_globalized;
	if (runtime->poll_check_project(&errors_by_globalized)) {
		// Only now does the host hold this text, so only now may a validate answer from cache.
		analyzed_source_by_path[in_flight_path] = in_flight_source;
		record_diagnostics(errors_by_globalized);

		// This is the authoritative moment for the file that was analysed, and the only one a
		// validate is not guaranteed to follow, so the log is written from here.
		const String globalized = ProjectSettings::get_singleton()->globalize_path(in_flight_path);
		log_new_diagnostics(globalized, diagnostics_for(in_flight_path));

		in_flight_path = String();
		in_flight_source = String();
	}

	// A buffer that changed while that ran is still waiting.
	start_pending_check();
}

void VerseScriptLanguage::record_diagnostics(const Dictionary &p_errors_by_globalized) const {
	diagnostics_by_path.clear();

	const Array reported = p_errors_by_globalized.keys();
	for (int64_t i = 0; i < reported.size(); i++) {
		const String globalized = reported[i];
		const String path = path_by_globalized.has(globalized) ? String(path_by_globalized[globalized]) : globalized;
		diagnostics_by_path[path] = p_errors_by_globalized[globalized];
	}
}

// Godot re-validates the edited buffer on an idle timer and again on save, so one compile error
// reaches this several times over. The script editor shows every result itself; the output log
// only wants a file's diagnostics when they change.
void VerseScriptLanguage::log_new_diagnostics(const String &p_globalized_path, const TypedArray<Dictionary> &p_errors) const {
	PackedStringArray formatted;
	for (int64_t i = 0; i < p_errors.size(); i++) {
		Dictionary error = p_errors[i];
		formatted.push_back(String(error["path"]) + String(":") + String::num_int64((int64_t)error["line"]) + String(":") + String::num_int64((int64_t)error["column"]) + String(": ") + String(error["message"]));
	}

	if (logged_diagnostics.has(p_globalized_path) && PackedStringArray(logged_diagnostics[p_globalized_path]) == formatted) {
		return;
	}
	logged_diagnostics[p_globalized_path] = formatted;

	for (int64_t i = 0; i < formatted.size(); i++) {
		UtilityFunctions::push_error(formatted[i]);
	}
}

TypedArray<Dictionary> VerseScriptLanguage::diagnostics_for(const String &p_path) const {
	if (!diagnostics_by_path.has(p_path)) {
		return TypedArray<Dictionary>();
	}
	return TypedArray<Dictionary>(diagnostics_by_path[p_path]);
}
