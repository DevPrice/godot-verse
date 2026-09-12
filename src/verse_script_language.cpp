#include "verse_script_language.h"

#include "verse_api_classes.h"
#include "verse_class_decl.h"
#include "verse_keywords.h"
#include "verse_lexer.h"
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

#ifdef TOOLS_ENABLED
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/script_editor.hpp>
#include <godot_cpp/classes/script_editor_base.hpp>
#endif

#include <algorithm>
#include <iterator>

using namespace godot;

// A validate's buffer arrives via TextEdit, which need not hand back the line endings the file
// was written with, so a CRLF file compared raw would miss the cache on every keystroke and
// every tab switch. Verse is newline-agnostic, so text differing only in line endings analyses
// identically and is safe to treat as unchanged.
String verse_newline_normalized(const String &p_source) {
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
String verse_doc_comment_above(const String &p_source, int64_t p_line) {
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

#ifdef TOOLS_ENABLED
EditorInterface *verse_editor_interface() {
	return Engine::get_singleton()->is_editor_hint() ? EditorInterface::get_singleton() : nullptr;
}
#endif

const char *verse_godot_class_for(const String &p_verse_class) {
	for (size_t i = 0; i < std::size(verse_api::classes); i++) {
		if (p_verse_class == verse_api::classes[i].verse_name) {
			return verse_api::classes[i].godot_name;
		}
	}
	return nullptr;
}

namespace {

VerseRuntime *get_runtime() {
	return Object::cast_to<VerseRuntime>(Engine::get_singleton()->get_singleton("VerseRuntime"));
}

// The Godot class whose documentation describes a Verse class. That is the mirrored table plus the
// one name missing from it: `vh_object`. That is Godot.native.verse's hand-written native root, the
// base Godot's own mirrored `object` derives from -- so it is not part of the generated API and not
// in the table, and absent from it a hover on one of its three lifecycle methods would report a
// local constant, with a tooltip that says nothing and nowhere for a click to go. Godot's Object is
// what those methods belong to as far as the documentation is concerned.
//
// Deliberately not folded into verse_godot_class_for: that one answers "is this name part of the
// generated API", which `vh_object` is not, and the completion path relies on the distinction.
const char *godot_doc_class_for(const String &p_verse_class) {
	if (p_verse_class == String("vh_object")) {
		return "Object";
	}
	return verse_godot_class_for(p_verse_class);
}

// The Godot method a mirrored Verse method stands for, keyed by the class that declares it. The
// Verse name cannot be inverted on its own: the transform to PascalCase drops the underscores
// that separated the words, so `SetPosition` could have come from any of several spellings.
const verse_api::method_mapping *godot_method_for(const String &p_verse_class, const String &p_verse_method) {
	for (size_t i = 0; i < std::size(verse_api::methods); i++) {
		if (p_verse_method == verse_api::methods[i].verse_method && p_verse_class == verse_api::methods[i].verse_class) {
			return &verse_api::methods[i];
		}
	}
	return nullptr;
}

// The Godot function one of the bridge's globals stands for. Godot documents its global functions
// on @GlobalScope -- a class its documentation has and ClassDB does not, and the one GDScript
// sends a click on `print(` to -- so naming it is what gives a global the tooltip and the jump a
// mirrored method already gets. Hand-written because the globals are: gen_verse_api.py mirrors
// classes, and a global belongs to none.
struct global_mapping {
	const char *verse_name;
	const char *godot_function;
};

constexpr global_mapping globals[] = {
	{ "IsInstanceValid", "is_instance_valid" },
	{ "Print", "print" },
};

const char *godot_global_for(const String &p_verse_name) {
	for (size_t i = 0; i < std::size(globals); i++) {
		if (p_verse_name == globals[i].verse_name) {
			return globals[i].godot_function;
		}
	}
	return nullptr;
}

// The Godot class a generated singleton accessor hands out -- GetEngine's Engine, which is the
// only thing that accessor can be said to be. gen_verse_api.py spells one as `Get` and the Godot
// class' own name, so inverting it is a lookup in the class table rather than a table of its own.
String godot_singleton_class_for(const String &p_verse_name) {
	if (!p_verse_name.begins_with("Get")) {
		return String();
	}
	const String godot_class = p_verse_name.substr(3);
	for (size_t i = 0; i < std::size(verse_api::classes); i++) {
		if (godot_class == verse_api::classes[i].godot_name) {
			return godot_class;
		}
	}
	return String();
}

// Whether a definition is one of the Godot package's globals, rather than a member of one of its
// classes or anything the project declares.
//
// A definition at the top level has no class to be owned by and reports the file it was written in
// instead -- a snippet scope carries its path as its name -- so the owner agreeing with the path is
// what says "top level". Which file it is then separates the package from the project, whose one
// flat scope would otherwise let a script's own Print be documented as Godot's.
bool is_godot_package_global(const String &p_owner, const String &p_path) {
	if (p_owner != p_path) {
		return false;
	}
	const String file = p_path.get_file();
	return file == String("Godot.native.verse")
			|| file == String("GodotApi.native.verse")
			|| file == String("GodotClasses.native.verse");
}

const char *mirrored_class(const String &p_godot_class) {
	for (size_t i = 0; i < std::size(verse_api::classes); i++) {
		if (p_godot_class == verse_api::classes[i].godot_name) {
			return verse_api::classes[i].verse_name;
		}
	}
	return nullptr;
}

// The Godot class a mirrored Verse class stands for, or empty when the name is not one of them --
// which is how a superclass is told to be another script's class rather than a piece of the
// generated API.
String godot_class_for(const std::string &p_verse_class) {
	const char *godot_name = verse_godot_class_for(String(p_verse_class.c_str()));
	return godot_name != nullptr ? String(godot_name) : String();
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

String formatted_diagnostic(const Dictionary &p_error) {
	return String(p_error["path"]) + ":" + String::num_int64((int64_t)p_error["line"]) + ":"
			+ String::num_int64((int64_t)p_error["column"]) + ": " + String(p_error["message"]);
}

// Every diagnostic in one comparable list. Dictionary's own == is reference equality, so telling
// one analysis' results from the next means flattening them.
PackedStringArray flattened_diagnostics(const Dictionary &p_errors_by_path) {
	PackedStringArray flattened;
	const Array paths = p_errors_by_path.keys();
	for (int64_t i = 0; i < paths.size(); i++) {
		const TypedArray<Dictionary> errors = p_errors_by_path[paths[i]];
		for (int64_t e = 0; e < errors.size(); e++) {
			flattened.push_back(formatted_diagnostic(errors[e]));
		}
	}
	return flattened;
}

#ifdef TOOLS_ENABLED

// Everything the editor drew from an analysis older than the one that just landed.
//
// Godot validates a script when its text changes and not once more after the author stops typing,
// and it republishes a script's documentation only when the script is saved. A result that arrives
// from a background analysis therefore has no way onto the screen: the error list, the error bar
// and the marked line keep describing the buffer as it was one analysis ago -- an error the author
// has already fixed stays underlined until they type again -- and the class documentation keeps
// describing the program as it was one save ago.
//
// `validate_script` is the signal CodeTextEditor's own idle timer emits to ask for the first, and
// CodeTextEditor is reachable because the CodeEdit ScriptEditorBase hands out is its child. It is
// not in the extension API, so it is checked rather than assumed: a build that moves it costs the
// stale underline back, not a crash. update_docs_from_script is the ask for the second, and it is
// the same pair of calls ScriptEditor::save_current_script makes around a save.
//
// Only the visible editor is refreshed. Godot validates a script when its tab is opened and
// republishes its documentation when it is saved, so the rest come back current on their own.
void refresh_current_script_editor() {
	// Reached at game runtime as well: analysis runs there too, and a result landing sets the flag
	// that asks for this. See verse_editor_interface.
	EditorInterface *editor_interface = verse_editor_interface();
	if (editor_interface == nullptr) {
		return;
	}

	ScriptEditor *script_editor = editor_interface->get_script_editor();
	if (script_editor == nullptr) {
		return;
	}

	const Ref<Script> script = script_editor->get_current_script();
	if (Object::cast_to<VerseScript>(script.ptr()) == nullptr) {
		return;
	}

	script_editor->clear_docs_from_script(script);
	script_editor->update_docs_from_script(script);

	ScriptEditorBase *current = script_editor->get_current_editor();
	Control *code_edit = current != nullptr ? current->get_base_editor() : nullptr;
	Node *code_text_editor = code_edit != nullptr ? code_edit->get_parent() : nullptr;
	if (code_text_editor != nullptr && code_text_editor->has_signal("validate_script")) {
		code_text_editor->emit_signal("validate_script");
	}
}

#endif

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
			"    Process<override>(Delta:float):void =\n";
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

// Every error moved onto a position that exists in p_source.
//
// Godot does not bounds-check a diagnostic against the buffer it is about to show it on:
// CodeTextEditor walks `line_text[i]` for every i below the error's column to work out where the
// caret goes once tabs are expanded, so a column past the end of that line is not a marker in the
// wrong place, it is `CRASH_BAD_INDEX` in CowData::get and the editor is gone -- no dialog, no log
// line, the whole process traps. (Godot 4.7, editor/code_editor.cpp; the crash reads
// "Index p_index = N is out of bounds (size() = M)".)
//
// The compiler's answer can miss the buffer three ways, and none of them is worth that. It
// describes the text the last analysis saw rather than the keystroke Godot is asking about --
// check_buffer answers from the previous analysis by design. It counts columns in utf8 bytes
// where Godot counts characters. And it may point one past the end of a line on purpose, which is
// exactly the off-by-one the loop above cannot survive.
//
// So the line is pinned inside the buffer and the column inside that line, with one past the last
// character allowed, which is what an "expected something here" error wants to say. Only the
// editor's copy is moved: the log keeps the compiler's own numbers, which describe the file it
// actually read.
static TypedArray<Dictionary> errors_fitted_to(const TypedArray<Dictionary> &p_errors, const String &p_source) {
	const PackedStringArray lines = verse_newline_normalized(p_source).split("\n");
	if (lines.is_empty()) {
		return p_errors;
	}

	TypedArray<Dictionary> fitted;
	for (int64_t i = 0; i < p_errors.size(); i++) {
		Dictionary error = Dictionary(p_errors[i]).duplicate();

		// A diagnostic with no location at all reports 0, which belongs on the first line rather
		// than one above it.
		int64_t line = error["line"];
		line = line < 1 ? 1 : (line > lines.size() ? lines.size() : line);

		const CharString utf8 = lines[line - 1].utf8();
		int64_t byte_column = (int64_t)error["column"] - 1;
		byte_column = byte_column < 0 ? 0 : (byte_column > utf8.length() ? utf8.length() : byte_column);

		error["line"] = line;
		error["column"] = String::utf8(utf8.get_data(), (int)byte_column).length() + 1;
		fitted.push_back(error);
	}
	return fitted;
}

Dictionary VerseScriptLanguage::_validate(const String &p_script, const String &p_path, bool p_validate_functions, bool p_validate_errors, bool p_validate_warnings, bool p_validate_safe_lines) const {
	const TypedArray<Dictionary> errors = errors_fitted_to(check_buffer(p_path, p_script), p_script);

	Dictionary result;
	result["valid"] = errors.is_empty();
	result["errors"] = errors;

	// An export the host refused is not an error -- the script compiles and runs, it just has one
	// fewer property than the author asked for -- and it would be invisible without this: Godot
	// draws what the property list holds, and a member that never reaches it leaves nothing behind
	// to explain its absence. The list is whatever the last analysis of this file left behind,
	// which is the same staleness every diagnostic here has.
	if (p_validate_warnings && export_warnings_by_path.has(p_path)) {
		result["warnings"] = export_warnings_by_path[p_path];
	}

	// ScriptTextEditor::get_functions() reads this key alone to build the script editor's method
	// outline -- none of Script's own method-list virtuals are involved. Left unset when there is
	// nothing to answer from, the same as every other optional key here.
	VerseRuntime *runtime = get_runtime();
	if (p_validate_functions && runtime != nullptr) {
		PackedStringArray functions;
		const TypedArray<Dictionary> members = runtime->class_members(p_path.get_file().get_basename());
		for (int64_t i = 0; i < members.size(); i++) {
			const Dictionary member = members[i];
			const int64_t line = member["line"];
			// No source location -- a member the host synthesised rather than one this file wrote --
			// has no line for the outline to jump to.
			if ((int64_t)member["kind"] == VH_LOOKUP_FUNCTION && line >= 0) {
				functions.push_back(String(member["name"]) + ":" + String::num_int64(line + 1));
			}
		}
		result["functions"] = functions;
	}

	return result;
}

bool VerseScriptLanguage::_has_named_classes() const {
	return false;
}

bool VerseScriptLanguage::_supports_builtin_mode() const {
	return false;
}

// Turning this on is what registers a script's own documentation, and with it the difference
// between the editor calling a member a property and calling it a local variable -- Godot reaches
// for documentation for everything except its two local lookup results.
//
// It has a startup cost. Godot loads every file of a language that supports documentation during
// the editor's filesystem scan, and loading a .verse resource builds the project, so the host now
// boots when the project is opened rather than when the first script is opened. For a project
// whose scene already runs Verse that is the same work moved earlier; for one where nothing does,
// it is new.
bool VerseScriptLanguage::_supports_documentation() const {
	return true;
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

// Godot drops an option that is missing any one of these keys and prints an error for it, so
// every option is built here rather than assembled piecemeal by each caller.
static Dictionary completion_option(const String &p_text, int64_t p_kind, int64_t p_location) {
	Dictionary option;
	option["kind"] = p_kind;
	option["display"] = p_text;
	option["insert_text"] = p_text;
	option["font_color"] = Color(1, 1, 1);
	option["icon"] = Variant();
	option["default_value"] = Variant();
	option["location"] = p_location;
	return option;
}

static bool is_identifier_char(char32_t p_c) {
	return (p_c >= 'a' && p_c <= 'z') || (p_c >= 'A' && p_c <= 'Z') || (p_c >= '0' && p_c <= '9') || p_c == '_';
}

// Whether a name is a candidate for what has been typed: the typed characters appear in it in
// order, ignoring case. `Prcs` reaches `Process`.
//
// This has to be at least as permissive as Godot's own filter or it decides the answer by itself.
// CodeEdit fuzzy-matches and ranks the options it is handed, so a name trimmed here is one the
// editor would have offered and never sees -- and a prefix test trims most of what fuzzy matching
// exists to find. A subsequence admits everything a fuzzy matcher would and leaves the ranking,
// which is the half worth having, where it already lives. ASCII folding is the whole of the case
// rule because a Verse identifier is ASCII.
//
// Deliberately narrower than Godot in one direction: this reads the name, while CodeEdit reads
// the rendered display text. An option that displays a whole declaration would otherwise match on
// its parameter types, and `flt` is not what someone typing `float` is looking for.
static bool matches_typed_prefix(const String &p_name, const String &p_prefix) {
	auto folded = [](char32_t p_c) { return p_c >= 'A' && p_c <= 'Z' ? p_c - 'A' + 'a' : p_c; };

	int64_t at = 0;
	for (int64_t i = 0; i < p_prefix.length(); i++) {
		while (at < p_name.length() && folded(p_name[at]) != folded(p_prefix[i])) {
			at++;
		}
		if (at >= p_name.length()) {
			return false;
		}
		at++;
	}
	return true;
}

// Stands in for the identifier being typed while the completion buffer is analysed. A legal Verse
// identifier, so the line parses; one no project would write, so it resolves to nothing and the
// answer is about the position rather than about whatever it collided with.
static const char *completion_placeholder = "VhCompletionCursor";

// Whether the token ending at p_end is a number rather than a name, which is what tells the `.` of
// `1.5` from the `.` of `Position.X`. An identifier may well end in a digit -- `node2d` does -- so
// it is the whole token that has to be digits, not just the character before the dot.
static bool ends_a_number_literal(const String &p_text, int64_t p_end) {
	if (p_end < 0 || p_end >= p_text.length()) {
		return false;
	}
	int64_t start = p_end + 1;
	while (start > 0 && is_identifier_char(p_text[start - 1])) {
		start--;
	}
	for (int64_t i = start; i <= p_end; i++) {
		if (p_text[i] < '0' || p_text[i] > '9') {
			return false;
		}
	}
	return start <= p_end;
}

// The offset of the last character of the callee of the innermost call the cursor is inside, or
// -1 when it is inside none.
//
// Scans back over the text before the cursor, closing every bracket it meets, so a nested call's
// arguments do not count as the outer call's. Verse opens a call with `(` and a failable one with
// `[`; `{` opens an archetype, which is a different construct and stops the scan rather than
// answering for it.
//
// Strings and comments are not skipped: a bracket inside either would throw the balance off. The
// cost is a wrong hint for a line with an unbalanced bracket inside a literal, which the host then
// declines to answer for anyway -- the callee that comes out is not a function.
static int64_t enclosing_call_callee_end(const String &p_before) {
	int64_t depth = 0;
	for (int64_t i = p_before.length() - 1; i >= 0; i--) {
		const char32_t c = p_before[i];
		if (c == ')' || c == ']' || c == '}') {
			depth++;
		} else if (c == '(' || c == '[') {
			if (depth > 0) {
				depth--;
				continue;
			}
			// An opening bracket with nothing to close it is the call the cursor is inside. Its
			// callee is whatever identifier ends immediately before it; a bracket that opens a
			// group rather than a call has no name there and answers -1.
			int64_t end = i - 1;
			while (end >= 0 && (p_before[end] == ' ' || p_before[end] == '\t')) {
				end--;
			}
			return end >= 0 && is_identifier_char(p_before[end]) ? end : -1;
		} else if (c == '{') {
			return -1;
		} else if (c == '\n' && depth == 0) {
			// Verse continues an argument list across lines, but a cursor on a line that opened no
			// bracket of its own is not inside a call this scan can trust.
			return -1;
		}
	}
	return -1;
}

// Which argument the cursor sits in: the commas between the call's opening bracket and the cursor,
// counted at bracket depth zero so a nested call's own commas do not advance the outer one.
static int64_t argument_index_in_call(const String &p_before, int64_t p_callee_end) {
	int64_t depth = 0;
	int64_t index = 0;
	for (int64_t i = p_callee_end + 1; i < p_before.length(); i++) {
		const char32_t c = p_before[i];
		if (c == '(' || c == '[' || c == '{') {
			depth++;
		} else if (c == ')' || c == ']' || c == '}') {
			depth--;
		} else if (c == ',' && depth == 1) {
			index++;
		}
	}
	return index;
}

// Which bracket the call the cursor is inside was opened with. vh_signature_desc reports a
// function's parameters but not its effects, so the hint takes the author's own answer: a call
// already written with `[` is the fallible one, and spelling its hint with parentheses contradicts
// the line it sits above.
static bool call_opened_with_bracket(const String &p_before, int64_t p_callee_end) {
	for (int64_t i = p_callee_end + 1; i < p_before.length(); i++) {
		if (p_before[i] != ' ' && p_before[i] != '\t') {
			return p_before[i] == '[';
		}
	}
	return false;
}

// The signature as Verse spells it, with the argument the cursor is in wrapped in the markers
// Godot highlights between. Verse's own order -- name, parameters, then `:type` -- rather than
// GDScript's leading return type, because that is how the declaration reads in the file.
static String call_hint_for(const Dictionary &p_signature, int64_t p_argument, bool p_fallible) {
	const String name = p_signature["name"];
	const String result_type = p_signature["result"];
	const TypedArray<Dictionary> params = p_signature["params"];

	String hint = name + (p_fallible ? String("[") : String("("));
	for (int64_t i = 0; i < params.size(); i++) {
		if (i > 0) {
			hint += ", ";
		}
		if (i == p_argument) {
			hint += String::chr(0xFFFF);
		}
		const Dictionary param = params[i];
		hint += String(param["name"]) + String(":") + String(param["type"]);
		if (i == p_argument) {
			hint += String::chr(0xFFFF);
		}
	}
	hint += p_fallible ? "]" : ")";
	if (!result_type.is_empty()) {
		hint += String(":") + result_type;
	}
	return hint;
}

// Godot's kind for a definition, so the completion box draws the right icon beside it.
static int64_t completion_kind_for(int64_t p_lookup_kind) {
	switch (p_lookup_kind) {
		case VH_LOOKUP_FUNCTION:
			return ScriptLanguageExtension::CODE_COMPLETION_KIND_FUNCTION;
		case VH_LOOKUP_CLASS:
		case VH_LOOKUP_TYPE_ALIAS:
			return ScriptLanguageExtension::CODE_COMPLETION_KIND_CLASS;
		case VH_LOOKUP_ENUM:
			return ScriptLanguageExtension::CODE_COMPLETION_KIND_ENUM;
		case VH_LOOKUP_MODULE:
			return ScriptLanguageExtension::CODE_COMPLETION_KIND_FILE_PATH;
		default:
			return ScriptLanguageExtension::CODE_COMPLETION_KIND_MEMBER;
	}
}

// Whether a call to a function has to be written with brackets rather than parentheses: Verse
// spells a `<decides>` call `GetChild[0]`, and completing it with `(` is a compile error at the
// moment it lands.
//
// Read off the effect specifiers alone rather than searched for anywhere in the signature, because
// a parameter or the result type can be a fallible function *type* -- the infallible
// `(Pred:(:int)<decides>->logic)<transacts>:void` spells `<decides>` too. SpellSignature puts the
// specifiers between the parameter list's closing parenthesis and the `:` before the result type,
// and emits no default values, so counting parentheses finds that one exactly.
static bool is_fallible_call(const String &p_signature) {
	int64_t depth = 0;
	for (int64_t i = 0; i < p_signature.length(); i++) {
		const char32_t c = p_signature[i];
		if (c == '(') {
			depth++;
		} else if (c == ')' && --depth == 0) {
			const int64_t result_type = p_signature.find(":", i);
			return p_signature.substr(i, result_type < 0 ? -1 : result_type - i).find("<decides>") >= 0;
		}
	}
	return false;
}

// Turns one vh_complete_item into an option.
//
// A call is completed with its brackets, and where the caret lands afterwards depends on whether
// there is anything to type between them: a function with parameters inserts only the opening one,
// so CodeEdit's brace completion closes it and leaves the caret inside, while one without inserts
// the pair and leaves the caret past it. GDScript spells it exactly this way, ellipsis and all.
static Dictionary completion_option_for(const Dictionary &p_item) {
	const String name = p_item["name"];
	const int64_t kind = p_item["kind"];
	const int64_t param_count = p_item["param_count"];
	const bool is_function = kind == VH_LOOKUP_FUNCTION;

	// `location` is the only lever Godot offers over the order options appear in, and the one
	// distinction worth making with it is the mirrored Godot API against everything else --
	// which is a class the author wrote, a local, or a Verse standard-library name. All three
	// are nearer to what is being typed than a thousand generated accessors.
	const String owner = p_item["owner"];
	const int64_t location = verse_godot_class_for(owner) == nullptr
			? ScriptLanguageExtension::LOCATION_LOCAL
			: ScriptLanguageExtension::LOCATION_OTHER;

	Dictionary option = completion_option(name, completion_kind_for(kind), location);
	if (is_function) {
		const bool takes_arguments = param_count > 0;
		const bool fallible = is_fallible_call(p_item["signature"]);
		const String open = fallible ? String("[") : String("(");
		const String close = fallible ? String("]") : String(")");
		option["insert_text"] = name + (takes_arguments ? open : open + close);
		option["display"] = name + open + (takes_arguments ? String::utf8("…") : String()) + close;
	}
	return option;
}

// One inherited method as the declaration that would override it, which is what GDScript
// completes inside a class body -- the whole `func _ready() -> void:` rather than the name.
//
// The Verse spelling of the same thing is the base's own signature with `<override>` after the
// name, which is why the signature is asked of the compiler rather than rebuilt from the type:
// an override must match what it overrides, parameter names included, and the function type
// drops those. The trailing ` =` is where the body goes; the editor's auto-indent takes the
// caret there on the next Enter, so nothing here inserts a newline of its own.
//
// LOCATION_LOCAL unconditionally, mirrored Godot class or not: at a position where a member is
// being declared, an override is what was asked for and belongs above the rest of the scope.
static Dictionary override_option_for(const Dictionary &p_item) {
	const String declaration = String(p_item["name"]) + String("<override>") + String(p_item["signature"]) + String(" =");
	return completion_option(declaration, ScriptLanguageExtension::CODE_COMPLETION_KIND_FUNCTION, ScriptLanguageExtension::LOCATION_LOCAL);
}

// Whether an option is an inherited method worth offering as a declaration -- one the compiler
// would take the override of *and* something would dispatch to.
//
// `is_overridable` answers only the first half, and on its own it offers the whole mirrored Godot
// API: every one of those methods is a class member the compiler would accept an override of. But
// gen_verse_api.py skips Godot's virtuals, so a generated method is never the Verse spelling of
// one -- it is a concrete shim that forwards into Godot through the handle. Overriding GetName
// compiles and changes nothing about what Godot calls. So the mirror is excluded wholesale, which
// is what the class table already answers.
//
// That leaves the two sets that mean something. `object` is hand-written rather than generated and
// so is not in the class table: its Ready/Process/PhysicsProcess are the only Godot virtuals the
// bridge carries at all, and the method table names exactly those three as `object`'s -- which is
// what the table lookup below is for, since anything else `object` ever grows would be a helper
// nothing dispatches to. Anything else is a class the author wrote, and the mirror never contains
// one of those.
//
// A method the class already declares comes back owned by that class -- the host lets a subclass'
// copy win over the superclass' and drops the duplicate -- so comparing the owner is what stops an
// override that is already written from being offered again.
static bool completes_as_override(const Dictionary &p_item, const String &p_enclosing_class) {
	const String owner = p_item["owner"];
	if (!(bool)p_item["is_overridable"] || String(p_item["signature"]).is_empty() || owner == p_enclosing_class) {
		return false;
	}
	if (verse_godot_class_for(owner) != nullptr) {
		return false;
	}
	return owner != String("vh_object") || godot_method_for(owner, p_item["name"]) != nullptr;
}

// A line's indentation width, or -1 for one carrying no code -- blank, or a comment, which sits
// at whatever column it was written at and so says nothing about the block it is in.
static int64_t code_line_indent(const String &p_line) {
	int64_t i = 0;
	while (i < p_line.length() && (p_line[i] == ' ' || p_line[i] == '\t')) {
		i++;
	}
	if (i >= p_line.length() || p_line[i] == '#') {
		return -1;
	}
	return i;
}

// The class whose members are declared at p_line/p_indent, or empty when the cursor is somewhere
// a name is used rather than declared. Both answers come from one scan because an override option
// needs them together: whether to offer declarations at all, and which class' own methods are
// already written and so must not be offered again.
//
// Read from the text, because Godot asks for completion on every keystroke and an analysis of a
// half-written declaration would not report the class it belongs to anyway. The test is the one
// the indentation already encodes: every line of code between the class and the cursor is
// indented at least as far as the cursor, since a line indented less would be the header of the
// block the cursor is really inside.
//
// Only the file's top-level class is found, so a member of a nested one completes as an ordinary
// name. That is the conservative direction, and one class per file is what the flat project scope
// forces in the first place.
static String member_declaration_class(const String &p_source, int64_t p_line, int64_t p_indent) {
	const VerseClassDecl decl = verse_scan_class_decl(p_source.utf8().get_data());
	if (decl.line < 0 || p_line <= decl.line) {
		return String();
	}

	const PackedStringArray lines = p_source.split("\n");
	if (decl.line >= lines.size() || p_line >= lines.size() || p_indent <= code_line_indent(lines[decl.line])) {
		return String();
	}

	for (int64_t i = p_line - 1; i > decl.line; i--) {
		const int64_t indent = code_line_indent(lines[i]);
		if (indent >= 0 && indent < p_indent) {
			return String();
		}
	}
	return String(decl.name.c_str());
}

// Whether the cursor -- the U+FFFF the editor splices in at p_marker -- stands inside a comment.
// The marker is taken back out first: it is three bytes the lexer would count as code, sitting
// exactly where the question is being asked about.
static bool completing_in_comment(const String &p_code, int64_t p_marker) {
	const String before = p_code.substr(0, p_marker);
	const int64_t line_start = before.rfind("\n") + 1;
	const String source = before + p_code.substr(p_marker + 1);
	return verse_position_in_comment(source.utf8().get_data(),
			(int)before.count("\n"), (int)before.substr(line_start).utf8().length());
}

// Completion, answered by the compiler wherever it can be.
//
// Godot marks the cursor by splicing U+FFFF into the buffer, and everything here is derived from
// where that landed: what precedes it -- a `.`, so this completes members of whatever is to the
// left, an `@`, so it completes the attributes in scope, or neither, so it completes names -- and
// what has been typed of the identifier so far.
//
// The buffer handed to the host has that half-typed identifier replaced by a fixed one that
// nothing defines. Replacing rather than deleting keeps the line parsing as the identifier it
// was going to be -- `Position.` and `set X = ` are both syntax errors, and a parse error can
// take the enclosing function's AST with it, while an unknown identifier costs one diagnostic
// nobody sees. And the substitution is what makes every keystroke of one identifier the same
// question, and so a cache hit: an analysis costs ~100ms and Godot re-asks on each of them.
//
// The class-name and keyword sets are still offered alongside the compiler's answer for a bare
// identifier. They cover what a scope walk cannot -- a class the author has not brought into
// view, and the keywords, which are not definitions at all.
//
// One position answers differently: a bare identifier on a line of its own inside a class body is
// a member being declared, and an inherited method offered there completes to the whole
// declaration that overrides it rather than to a call. Everything else in scope is still offered,
// so a misread of the position costs nothing beyond an option that was already going to be there.
Dictionary VerseScriptLanguage::_complete_code(const String &p_code, const String &p_path, Object *p_owner) const {
	Dictionary result;
	result["result"] = (int64_t)OK;
	result["force"] = false;
	result["call_hint"] = String();

	const int64_t marker = p_code.find(String::chr(0xFFFF));
	if (marker < 0) {
		return result;
	}

	// A comment is prose, and every set below is names. Godot raises the popup on its own as soon
	// as one of them matches what is being typed, so answering here puts the Godot API over the
	// middle of a sentence.
	if (completing_in_comment(p_code, marker)) {
		return result;
	}

	// What has been typed, which every set below is trimmed against before it is handed over.
	// Godot filters again and does the ranking, but only after building an option for each of a
	// thousand class names, on every keystroke.
	const String before = p_code.substr(0, marker);
	int64_t prefix_start = before.length();
	while (prefix_start > 0 && is_identifier_char(before[prefix_start - 1])) {
		prefix_start--;
	}
	const String prefix = before.substr(prefix_start);

	// The receiver is whatever ends immediately before the dot. Only a dot: `?` and `^` are
	// postfix operators the host unwraps on its own, and a space between the two is not something
	// Verse writes.
	const int64_t receiver_end = prefix_start - 2;
	const bool completing_members = prefix_start > 0
			&& before[prefix_start - 1] == '.'
			&& !ends_a_number_literal(before, receiver_end);

	// An `@` means an attribute and nothing else, so the whole scope is the wrong answer there:
	// `@e` is reaching for `editable`, not for every name in the project that contains an e.
	//
	// The names offered are bare, the `@` left where it is. Godot's own filter walks back over
	// identifier characters and stops at the symbol, so the text it is matching against and the
	// text it replaces on insert are both the part past it -- GDScript strips the `@` off its
	// annotations for exactly this reason.
	const bool completing_attribute = !completing_members && prefix_start > 0 && before[prefix_start - 1] == '@';

	// The class this is adding a member to, when that is what the cursor is doing: nothing but
	// indentation ahead of the prefix on its line, and that line belonging to the class body. An
	// inherited method offered there is being declared rather than called, and completes to the
	// whole declaration.
	const int64_t line_start = before.rfind("\n") + 1;
	const String ahead_of_prefix = before.substr(line_start, prefix_start - line_start);
	const String declaring_in_class = !completing_members && !completing_attribute && !ahead_of_prefix.is_empty() && ahead_of_prefix.strip_edges().is_empty()
			? member_declaration_class(verse_newline_normalized(p_code), before.count("\n"), ahead_of_prefix.length())
			: String();

	Array options;

	VerseRuntime *runtime = get_runtime();
	const bool host_can_answer = project_built && runtime != nullptr && runtime->is_host_loaded();

	// The buffer as the compiler should see it: marker gone, and the identifier being typed
	// standing in for whatever it will become. The same text for every prefix of one identifier,
	// and one no script can collide with -- a name Verse code could define would make the
	// substitution resolve to it. Shared by the options below and the argument hint, which is what
	// lets the host answer both off one analysis.
	const String source = verse_newline_normalized(before.substr(0, prefix_start) + String(completion_placeholder) + p_code.substr(marker + 1));

	// The zero-based row and utf8 byte column of a character offset into that buffer, which is how
	// the compiler counts and is not how Godot counts.
	auto position_of = [&source](int64_t p_offset, int64_t &r_line, int64_t &r_column) {
		const String up_to = source.substr(0, p_offset);
		r_line = up_to.count("\n");
		const int64_t line_start = up_to.rfind("\n") + 1;
		r_column = up_to.substr(line_start).utf8().length();
	};

	// The argument hint, which is what Godot draws above the caret while a call is open. Asked
	// before the options because it is the answer for a cursor with nothing typed at all -- the
	// moment right after the `(` -- which is exactly where the options below decline.
	if (host_can_answer) {
		const int64_t callee_end = enclosing_call_callee_end(before);
		if (callee_end >= 0) {
			int64_t line = 0;
			int64_t column = 0;
			position_of(callee_end, line, column);

			if (signature_cache_source != source || signature_cache_line != (int32_t)line
					|| signature_cache_column != (int32_t)column) {
				settle_checks();
				const String globalized = ProjectSettings::get_singleton()->globalize_path(p_path);
				signature_cache = runtime->signature_at(globalized, source, (int32_t)line, (int32_t)column);
				signature_cache_source = source;
				signature_cache_line = (int32_t)line;
				signature_cache_column = (int32_t)column;
				analyzed_source_by_path.erase(p_path);
			}

			if (!signature_cache.is_empty()) {
				result["call_hint"] = call_hint_for(signature_cache, argument_index_in_call(before, callee_end), call_opened_with_bracket(before, callee_end));
			}
		}
	}

	// Without a dot, a bare cursor would offer every name in scope as one undifferentiated list;
	// with one, the member set is bounded by the receiver's type and is exactly what was asked
	// for. An `@` bounds it just as tightly, and is worth showing unprompted for the same reason:
	// the attributes in scope are a short list and nothing else can follow it.
	if (!completing_members && !completing_attribute && prefix.is_empty()) {
		return result;
	}
	result["force"] = completing_attribute;

	if (host_can_answer && (!completing_members || receiver_end >= 0)) {
		// Members are asked about the receiver's last byte; a bare identifier about where it
		// would be written, which is where the prefix started. Both sit before the substitution,
		// so neither moves when the placeholder is a different length than what was typed.
		const int64_t position = completing_members ? receiver_end : prefix_start;
		int64_t line = 0;
		int64_t column = 0;
		position_of(position, line, column);
		const int32_t mode = completing_members ? VH_COMPLETE_MEMBERS
				: (completing_attribute ? VH_COMPLETE_ATTRIBUTES : VH_COMPLETE_SCOPE);

		if (completion_cache_source != source || completion_cache_line != (int32_t)line
				|| completion_cache_column != (int32_t)column || completion_cache_mode != mode) {
			// Drain whatever validate had queued first. An analysis that finishes *after* this one
			// would be reaped by a later poll_check, which records its buffer as the text the host
			// holds -- and the host would by then be holding the completion buffer instead. Every
			// locus a hover reads afterwards would be attributed to the wrong text. Settling costs
			// the wait once per completion context rather than once per keystroke, because the
			// cache above is what the rest of a prefix hits.
			settle_checks();

			const String globalized = ProjectSettings::get_singleton()->globalize_path(p_path);
			completion_cache_options = runtime->complete_symbol(globalized, source, (int32_t)line, (int32_t)column, mode);
			completion_cache_source = source;
			completion_cache_line = (int32_t)line;
			completion_cache_column = (int32_t)column;
			completion_cache_mode = mode;

			// The host now holds the completion buffer as this file's text, so the analysis every
			// lookup and every cached validate was answering from is spent. Dropping the entry is
			// what stops a hover from trusting loci that describe a buffer with a placeholder
			// spliced into it; the next validate re-analyses and puts it back.
			analyzed_source_by_path.erase(p_path);
		}

		for (int64_t i = 0; i < completion_cache_options.size(); i++) {
			const Dictionary item = completion_cache_options[i];
			const String name = item["name"];
			if (!matches_typed_prefix(name, prefix)) {
				continue;
			}
			if (!declaring_in_class.is_empty() && completes_as_override(item, declaring_in_class)) {
				options.push_back(override_option_for(item));
			} else {
				options.push_back(completion_option_for(item));
			}
		}
	}

	// A dot and an `@` have both answered everything they are going to; the sets below are names
	// that could be written on their own, which is neither a member nor an attribute.
	if (completing_members || completing_attribute) {
		result["options"] = options;
		return result;
	}

	for (size_t i = 0; i < std::size(verse_api::classes); i++) {
		const String name = verse_api::classes[i].verse_name;
		if (matches_typed_prefix(name, prefix)) {
			options.push_back(completion_option(name, ScriptLanguageExtension::CODE_COMPLETION_KIND_CLASS, ScriptLanguageExtension::LOCATION_OTHER));
		}
	}

	const PackedStringArray class_names = script_class_names();
	for (int64_t i = 0; i < class_names.size(); i++) {
		if (matches_typed_prefix(class_names[i], prefix)) {
			options.push_back(completion_option(class_names[i], ScriptLanguageExtension::CODE_COMPLETION_KIND_CLASS, ScriptLanguageExtension::LOCATION_OTHER_USER_CODE));
		}
	}

	for (size_t i = 0; i < std::size(verse_keywords::reserved_words); i++) {
		const String word = verse_keywords::reserved_words[i];
		if (matches_typed_prefix(word, prefix)) {
			options.push_back(completion_option(word, ScriptLanguageExtension::CODE_COMPLETION_KIND_PLAIN_TEXT, ScriptLanguageExtension::LOCATION_OTHER));
		}
	}

	result["options"] = options;
	return result;
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
	const String normalized = verse_newline_normalized(before + p_code.substr(marker + 1));
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

	// A definition that came from the mirrored Godot API is described by Godot's own class
	// documentation, which is better than anything this could say and is already installed. Both
	// paths key off class_name: the click sends it to the help viewer instead of jumping, and the
	// tooltip fetches the description out of the same doc data. Naming it here is what turns a
	// Verse identifier into a Godot doc page, and there is no source in the project to jump to
	// anyway -- the generated API is compiled from the engine tree.
	const String found_name = found["name"];
	const String found_owner = found["owner"];
	const int64_t kind = found["kind"];

	// GodotVerse::LookupSymbol only fills Type for a CDataDefinition or CFunction, so an enum's own
	// name -- neither -- would otherwise tooltip with a blank type beside "Local Constant". Wrong
	// label aside (Godot's lookup result has no "local type" of its own to ask for instead, and an
	// enumerator genuinely is one), a blank type says nothing at all.
	if (kind == VH_LOOKUP_ENUM && String(result["doc_type"]).is_empty()) {
		result["doc_type"] = String("enum");
	}

	// An override means something the declaration itself does not say. Only at a declaration: a
	// call site already resolves to the implementation that will run, and sending that to the
	// parent would be wrong rather than merely unhelpful.
	const String overridden_owner = found["overridden_owner"];
	const bool is_definition = bool(found["is_definition"]);
	const bool overrides_something = is_definition && !overridden_owner.is_empty();

	// A parameter where it is declared describes nothing the line does not already say, and it is
	// the one place a jump has nowhere to go -- the declaration is the line the cursor is on. So
	// the answer is a refusal, which is a hover with no tooltip at all, the way GDScript leaves it.
	const bool is_parameter = bool(found["is_parameter"]);
	if (is_parameter && is_definition) {
		return result;
	}

	// The comment block above a definition, wherever it was written. A file the project does not
	// own -- Godot.native.verse in the engine tree -- cannot be jumped to, but its comment is
	// still the best description of what a script is overriding.
	auto comment_at = [&](const String &p_definition_path, int64_t p_line) -> String {
		if (p_line < 0 || p_definition_path.is_empty()) {
			return String();
		}
		// The buffer for the file being edited may be ahead of what is on disk; anything else is
		// at worst as stale as the analysis that pointed here.
		if (p_definition_path == globalized) {
			return verse_doc_comment_above(normalized, p_line);
		}
		const String res_path = path_by_globalized.get(p_definition_path, String());
		const String source = FileAccess::get_file_as_string(res_path.is_empty() ? p_definition_path : res_path);
		return verse_doc_comment_above(verse_newline_normalized(source), p_line);
	};

	const int64_t own_line = found["line"];
	const String own_path = found["path"];
	const int64_t overridden_line = found["overridden_line"];
	const String overridden_path = found["overridden_path"];
	// A parameter's source line is the line its whole function is declared on, so the comment
	// "above" it is the function's -- describing an argument with the method's prose. It has no
	// documentation of its own, and GDScript gives one none either.
	const String own_description = is_parameter ? String() : comment_at(own_path, own_line);

	if (kind == VH_LOOKUP_CLASS) {
		if (const char *godot_class = godot_doc_class_for(found_name)) {
			result["type"] = (int64_t)ScriptLanguageExtension::LOOKUP_RESULT_CLASS;
			result["class_name"] = String(godot_class);
			return result;
		}
	} else if (kind == VH_LOOKUP_FUNCTION || kind == VH_LOOKUP_DATA) {
		// A global is a member of nothing, so the method table has no owner to answer it by, and
		// the file it is declared in is in the engine tree rather than in the project -- leaving
		// it, before this, described by its own comment and with nowhere to click through to.
		if (is_godot_package_global(found_owner, own_path)) {
			if (const char *global_function = godot_global_for(found_name)) {
				result["type"] = (int64_t)ScriptLanguageExtension::LOOKUP_RESULT_CLASS_METHOD;
				result["class_name"] = String("@GlobalScope");
				result["class_member"] = String(global_function);
				return result;
			}
			const String singleton_class = godot_singleton_class_for(found_name);
			if (!singleton_class.is_empty()) {
				result["type"] = (int64_t)ScriptLanguageExtension::LOOKUP_RESULT_CLASS;
				result["class_name"] = singleton_class;
				return result;
			}
		}

		// A mirrored property is a var, so the kind alone cannot separate it from a script's own
		// @editable member; the owner does, since only a mirrored class appears in the table.
		const verse_api::method_mapping *method = godot_method_for(found_owner, found_name);

		// An override is looked up under what it overrides: a script's own class is never in the
		// table, and `Ready<override>()` means Godot's _ready however the script spells it. Only
		// when the override says nothing itself, though -- routing into Godot's documentation
		// hands the tooltip to Godot's doc data too, which would throw away prose written here.
		if (method == nullptr && overrides_something && own_description.is_empty()) {
			method = godot_method_for(overridden_owner, found_name);
		}
		if (method != nullptr) {
			result["type"] = (int64_t)(kind == VH_LOOKUP_FUNCTION
							? ScriptLanguageExtension::LOOKUP_RESULT_CLASS_METHOD
							: ScriptLanguageExtension::LOOKUP_RESULT_CLASS_PROPERTY);
			result["class_name"] = String(method->godot_class);
			result["class_member"] = String(method->godot_method);
			return result;
		}

		// A member of a class the project itself declares is a property or a method, and saying so
		// is the whole difference between the editor calling it that and calling it a local
		// variable. It takes naming the class it belongs to, which is only safe because that name
		// is registered as a *script* doc: the click path diverts a class_name into the help viewer
		// only when the class is one of Godot's own, so this one still falls through to the jump
		// below. The description comes from the same registered doc rather than from `description`,
		// which Godot reads for the local results alone.
		//
		// Only a class: a parameter's owner is the function that declares it, and a local's is a
		// block. Neither is a property of anything, and both keep the local results, which are the
		// only ones that can carry prose this has read out of the source itself.
		if (script_class_names().has(found_owner)) {
			result["type"] = (int64_t)(kind == VH_LOOKUP_FUNCTION
							? ScriptLanguageExtension::LOOKUP_RESULT_CLASS_METHOD
							: ScriptLanguageExtension::LOOKUP_RESULT_CLASS_PROPERTY);
			result["class_name"] = found_owner;
			result["class_member"] = found_name;
		}
	}

	String description = own_description;
	if (description.is_empty() && overrides_something) {
		description = comment_at(overridden_path, overridden_line);
	}
	if (!description.is_empty()) {
		result["description"] = description;
	}

	// At a declaration that overrides, the parent is the only useful destination: this
	// definition's own line is the one the cursor is already on.
	const int64_t target_line = overrides_something ? overridden_line : own_line;
	const String target_path = overrides_something ? overridden_path : own_path;
	if (target_line < 0 || target_path.is_empty()) {
		return result;
	}

	// A location with no script beside it is read as a line in the file being edited, so a
	// cross-file definition we cannot name gets no location at all rather than a jump to that
	// line of the wrong file.
	const bool same_file = target_path == globalized;
	const String target_res_path = same_file ? p_path : String(path_by_globalized.get(target_path, String()));
	if (target_res_path.is_empty()) {
		return result;
	}

	result["location"] = target_line + 1;
	if (!same_file) {
		result["script"] = ResourceLoader::get_singleton()->load(target_res_path);
		result["script_path"] = target_res_path;
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
	return p_type == _get_type();
}

// Read from the file's text, never from the host.
//
// EditorFileSystem asks this from its scan thread, for every .verse in the project, during the
// startup scan -- and both halves of that are out of the ABI's reach. Every vh_ entry point has
// to be called on the vh_init thread, and vh_compile_project may run only once per process, so a
// filesystem scan is the last thing that should be able to trigger a build. GDScript answers the
// same question from a tokenizer-only pass for the same reason.
Dictionary VerseScriptLanguage::_get_global_class_name(const String &p_path) const {
	const String source = FileAccess::get_file_as_string(p_path);
	if (FileAccess::get_open_error() != OK) {
		return Dictionary();
	}

	const VerseClassDecl decl = verse_scan_class_decl(source.utf8().get_data());
	if (decl.name.empty()) {
		return Dictionary();
	}

	Dictionary result;
	result["base_type"] = base_types_for(decl).registry_base;
	result["is_abstract"] = decl.is_abstract;
	result["is_tool"] = false;
	// Presence of "name" is what registers the class: ScriptLanguageExtension::get_global_class_name
	// returns empty the moment the key is absent, so a script without the attribute must not set
	// it. The other keys are filled either way, as C#'s ScriptManagerBridge does.
	if (decl.is_global) {
		result["name"] = String(verse_pascal_case(decl.name).c_str());
	}
	return result;
}

String VerseScriptLanguage::script_path_for_class(const String &p_class_name) const {
	const PackedStringArray sources = find_verse_sources("res://");
	for (int64_t i = 0; i < sources.size(); i++) {
		if (sources[i].get_file().get_basename() == p_class_name) {
			return sources[i];
		}
	}
	return String();
}

VerseScriptLanguage::BaseTypes VerseScriptLanguage::base_types_for(const VerseClassDecl &p_decl) const {
	// The chain is walked rather than only its first link, so a script three deep still finds the
	// mirrored class and the global ancestor above it. The bound is the cycle guard: Verse rejects
	// a cyclic hierarchy, but this reads unbuilt text that may still contain one.
	constexpr int max_depth = 32;

	BaseTypes result;
	result.instance_base = StringName("Node");
	bool found_global = false;

	VerseClassDecl decl = p_decl;
	for (int depth = 0; depth < max_depth && !decl.base.empty(); depth++) {
		// A name is either one of the generated Godot mirrors or another script's class; the two
		// sets cannot overlap, so a mirror ends the walk.
		const String mirrored = godot_class_for(decl.base);
		if (!mirrored.is_empty()) {
			result.instance_base = StringName(mirrored);
			break;
		}

		const String base = String(decl.base.c_str());
		const String base_path = script_path_for_class(base);
		if (base_path.is_empty()) {
			break;
		}
		const String base_source = FileAccess::get_file_as_string(base_path);
		if (FileAccess::get_open_error() != OK) {
			break;
		}
		const VerseClassDecl base_decl = verse_scan_class_decl(base_source.utf8().get_data());
		if (base_decl.name.empty()) {
			break;
		}

		// Nearest wins, so only the first global ancestor is recorded -- but the walk continues,
		// because instance_base still needs the mirrored class further up. The registered name,
		// not the Verse one: this has to name the ancestor as Godot knows it.
		if (base_decl.is_global && !found_global) {
			result.registry_base = String(verse_pascal_case(base_decl.name).c_str());
			found_global = true;
		}
		decl = base_decl;
	}

	if (!found_global) {
		result.registry_base = String(result.instance_base);
	}
	return result;
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

#ifdef TOOLS_ENABLED
		// Deliberately here rather than in poll_check: settle_checks reaps an analysis from the
		// middle of a completion request, and re-entering the script editor from there would
		// rebuild its error list while it is drawing a popup.
		if (editor_refresh_pending) {
			editor_refresh_pending = false;
			refresh_current_script_editor();
		}
#endif

		runtime->tick(frame_budget_ms / 1000.0);

		// Last, so everything above answers against a host that is not mid-analysis. A queued
		// buffer waits a frame for this; a blocked editor would wait the whole analysis.
		start_pending_check();
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

PackedStringArray VerseScriptLanguage::script_class_names() const {
	const PackedStringArray sources = find_verse_sources("res://");
	PackedStringArray names;
	for (int64_t i = 0; i < sources.size(); i++) {
		names.push_back(sources[i].get_file().get_basename());
	}
	return names;
}

Error VerseScriptLanguage::ensure_project_built() {
	if (project_built) {
		return project_build_status;
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
			analyzed_source_by_path[sources[i]] = verse_newline_normalized(text);
		}
	}

	const Array reported = errors_by_globalized.keys();
	for (int64_t i = 0; i < reported.size(); i++) {
		log_new_diagnostics(reported[i], TypedArray<Dictionary>(errors_by_globalized[reported[i]]));
	}

	project_built = true;
	project_build_status = status;

	// Code generation gets one attempt per process and this was it, so no later edit can produce
	// a runnable program -- however clean the file becomes, has_class keeps answering no. The
	// diagnostics above say what is wrong; this says what fixing them will and will not buy.
	if (status != OK) {
		UtilityFunctions::push_warning(
				"Verse: the project did not build. Fixing the errors restores the editor's analysis -- "
				"exported properties, completion and lookup -- but a script cannot run until the editor "
				"is restarted, because Verse generates code once per process.");
	}

	return status;
}

TypedArray<Dictionary> VerseScriptLanguage::check_buffer(const String &p_path, const String &p_source) const {
	VerseRuntime *runtime = get_runtime();
	if (!project_built || runtime == nullptr || !runtime->is_host_loaded()) {
		return diagnostics_for(p_path);
	}

	// Anything the host does not already hold needs a fresh analysis, which takes about as long as
	// three frames. Start it on the host's thread and answer from the last one: returning stale
	// diagnostics for a moment is a far smaller cost than freezing the editor on every keystroke.
	// _frame picks the result up, and Godot re-validates often enough that the fresh answer lands
	// on its own.
	//
	// That answer is deliberately not logged. It describes whatever the file said before this
	// edit, which may be a mistake the author has already undone, and the output log has no way
	// to retract a line. The script editor's own error list is free to show it because Godot
	// replaces it wholesale on the next validate; the log is not.
	if (!analysis_is_current(p_path, p_source)) {
		queue_check(p_path, p_source);
		return diagnostics_for(p_path);
	}

	const String globalized = ProjectSettings::get_singleton()->globalize_path(p_path);

	// Analysis covers the whole project, so a broken file elsewhere reports against its own path;
	// the editor asked about this one.
	const TypedArray<Dictionary> errors = diagnostics_for(p_path);
	log_new_diagnostics(globalized, errors);
	return errors;
}

bool VerseScriptLanguage::analysis_is_current(const String &p_path, const String &p_source) const {
	VerseRuntime *runtime = get_runtime();
	if (!project_built || runtime == nullptr || !runtime->is_host_loaded()) {
		return true;
	}

	// The common case by far: opening a file, switching to its tab and saving it all ask about a
	// buffer nothing has touched since the last analysis.
	return analyzed_source_by_path.has(p_path)
			&& String(analyzed_source_by_path[p_path]) == verse_newline_normalized(p_source);
}

void VerseScriptLanguage::queue_check(const String &p_path, const String &p_source) const {
	request_check(p_path, verse_newline_normalized(p_source));
}

void VerseScriptLanguage::register_script(VerseScript *p_script) {
	live_scripts.push_back(p_script);
}

void VerseScriptLanguage::register_instance(int64_t p_object_id, VerseScriptInstance *p_instance) {
	live_instances[p_object_id] = p_instance;
}

void VerseScriptLanguage::unregister_instance(int64_t p_object_id) {
	live_instances.erase(p_object_id);
}

VerseScriptInstance *VerseScriptLanguage::instance_for(int64_t p_object_id) const {
	const auto found = live_instances.find(p_object_id);
	return found != live_instances.end() ? found->second : nullptr;
}

void VerseScriptLanguage::unregister_script(VerseScript *p_script) {
	live_scripts.erase(std::remove(live_scripts.begin(), live_scripts.end(), p_script), live_scripts.end());
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
	// a second time -- putting a whole extra analysis between a save and the result it settles on.
	if (p_path == in_flight_path && p_normalized_source == in_flight_source) {
		return;
	}

	// Newest buffer wins: while an analysis runs the editor keeps typing, and every intermediate
	// state is worth less than the one the author is looking at now.
	pending_check_path = p_path;
	pending_check_source = p_normalized_source;
	has_pending_check = true;

	// Queued, not started. Every host entry point that reads the semantic program joins the
	// analysis thread before it answers -- vh_has_class, vh_class_members, vh_class_export_list,
	// all of them -- so an analysis begun here is one whatever the caller does next pays for.
	// Saving is where that bites: ScriptEditor::save_current_script asks the script for its
	// documentation the moment save_resource returns, and starting the analysis inside the save
	// put the whole ~100ms right back into Ctrl+S. _frame starts it once the frame's own work is
	// done instead.
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
		editor_refresh_pending = record_diagnostics(errors_by_globalized) || editor_refresh_pending;

		// This is the authoritative moment for the file that was analysed, and the only one a
		// validate is not guaranteed to follow, so the log is written from here.
		const String globalized = ProjectSettings::get_singleton()->globalize_path(in_flight_path);
		log_new_diagnostics(globalized, diagnostics_for(in_flight_path));
		refresh_export_warnings(in_flight_path);

		in_flight_path = String();
		in_flight_source = String();

		// Every script whose compile() declined to wait for this. Told one at a time rather than
		// only the analysed file's script, because a save can be waiting on a result its own
		// buffer did not start. Snapshotted: telling a script republishes its export list, and
		// Godot is free to drop a script while that runs.
		const std::vector<VerseScript *> scripts = live_scripts;
		for (VerseScript *script : scripts) {
			editor_refresh_pending = script->analysis_landed() || editor_refresh_pending;
		}
	}
}

// What a rejected export has to say for itself, at the line that declared it.
//
// The two rules read as instructions because they have a fix the author can apply. The third does
// not: it is the bridge's own coverage, and saying so plainly is better than a suggestion that
// would not work.
static String export_rejection_message(const Dictionary &p_entry) {
	const String name = p_entry["name"];
	const String class_name = p_entry["hint_string"];
	switch ((int64_t)p_entry["reject"]) {
		case VH_EXPORT_OBJECT_NOT_OPTIONAL:
			return name + String(" is a ") + class_name
					+ String(", and the inspector may leave that slot empty. Declare it `?") + class_name
					+ String("` so the member can hold the empty case.");
		case VH_EXPORT_OPTION_NOT_OBJECT:
			return name + String(" is an option around a value the inspector has no empty slot for. ")
					+ String("Only a node or a resource can be left unassigned.");
		case VH_EXPORT_SCRIPT_CLASS_NOT_GLOBAL:
			return name + String(" refers to ") + class_name
					+ String(", which is not registered with Godot. The inspector filters the slot by a ")
					+ String("Godot class name, so add `@global_class` to ") + class_name + String(".");
		default:
			return name + String(" has a type godot-verse cannot carry to the inspector yet, so it is not exported.");
	}
}

static String export_rejection_code(int64_t p_reject) {
	switch (p_reject) {
		case VH_EXPORT_OBJECT_NOT_OPTIONAL:
			return String("OBJECT_EXPORT_NOT_OPTIONAL");
		case VH_EXPORT_OPTION_NOT_OBJECT:
			return String("OPTION_EXPORT_NOT_OBJECT");
		case VH_EXPORT_SCRIPT_CLASS_NOT_GLOBAL:
			return String("SCRIPT_CLASS_EXPORT_NOT_GLOBAL");
		default:
			return String("EXPORT_TYPE_UNSUPPORTED");
	}
}

void VerseScriptLanguage::refresh_export_warnings(const String &p_path) const {
	VerseRuntime *runtime = get_runtime();
	if (runtime == nullptr || !runtime->is_host_loaded()) {
		return;
	}

	TypedArray<Dictionary> warnings;
	bool found = false;
	const TypedArray<Dictionary> exports = runtime->class_exports(p_path.get_file().get_basename(), &found);
	for (int64_t i = 0; found && i < exports.size(); i++) {
		const Dictionary entry = exports[i];
		const int64_t reject = entry["reject"];
		if (reject == VH_EXPORT_OK) {
			continue;
		}

		// The host counts rows from zero and the editor from one. A member with no source location
		// is not worth a warning nobody can find, so it is dropped rather than parked on line one.
		const int64_t line = entry["line"];
		if (line < 0) {
			continue;
		}

		Dictionary warning;
		warning["start_line"] = line + 1;
		warning["end_line"] = line + 1;
		warning["leftmost_column"] = (int64_t)entry["column"] + 1;
		warning["rightmost_column"] = (int64_t)entry["column"] + 1;
		warning["code"] = reject;
		warning["string_code"] = export_rejection_code(reject);
		warning["message"] = export_rejection_message(entry);
		warnings.push_back(warning);
	}

	// Written even when empty: a member the author has just fixed has to lose its warning, and the
	// analysis that proves it is this one.
	export_warnings_by_path[p_path] = warnings;
}

bool VerseScriptLanguage::record_diagnostics(const Dictionary &p_errors_by_globalized) const {
	const PackedStringArray previous = flattened_diagnostics(diagnostics_by_path);

	diagnostics_by_path.clear();

	const Array reported = p_errors_by_globalized.keys();
	for (int64_t i = 0; i < reported.size(); i++) {
		const String globalized = reported[i];
		const String path = path_by_globalized.has(globalized) ? String(path_by_globalized[globalized]) : globalized;
		const TypedArray<Dictionary> errors = p_errors_by_globalized[globalized];

		// The host reports the absolute path it was handed, but the script editor compares an
		// error's path against the *script's* -- `res://scripts/mover.verse` -- and moves every
		// error that does not match into its depended-errors list. Those are listed but never
		// marked: the line highlight and the error bar both read the list this filters.
		for (int64_t e = 0; e < errors.size(); e++) {
			Dictionary error = errors[e];
			error["path"] = path;
		}
		diagnostics_by_path[path] = errors;
	}

	return flattened_diagnostics(diagnostics_by_path) != previous;
}

// Godot re-validates the edited buffer on an idle timer and again on save, so one compile error
// reaches this several times over. The script editor shows every result itself; the output log
// only wants a file's diagnostics when they change.
void VerseScriptLanguage::log_new_diagnostics(const String &p_globalized_path, const TypedArray<Dictionary> &p_errors) const {
	PackedStringArray formatted;
	for (int64_t i = 0; i < p_errors.size(); i++) {
		formatted.push_back(formatted_diagnostic(p_errors[i]));
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
