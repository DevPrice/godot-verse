#include "verse_script_language.h"

#include "verse_api_classes.h"
#include "verse_api_skipped.h"
#include "verse_class_decl.h"
#include "verse_keywords.h"
#include "verse_lexer.h"
#include "verse_module_map.h"
#include "verse_runtime.h"
#include "verse_script.h"

#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/engine_debugger.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/memory.hpp>
#include <godot_cpp/templates/hash_set.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#ifdef TOOLS_ENABLED
#include <godot_cpp/classes/code_edit.hpp>
#include <godot_cpp/classes/editor_file_system.hpp>
#include <godot_cpp/classes/editor_file_system_directory.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/script_editor.hpp>
#include <godot_cpp/classes/script_editor_base.hpp>
#endif

#include <algorithm>
#include <cctype>
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
	// Only so EditorFileSystem's filesystem_changed has something to connect to; a signal needs a
	// bound method, and nothing else here is reachable from Godot by name.
	ClassDB::bind_method(D_METHOD("on_filesystem_changed"), &VerseScriptLanguage::on_filesystem_changed);
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

// The one template, and a direct translation of GDScript's -- the same two virtuals, the same two
// comments above them, and a body that compiles as generated, which `{}` is: Verse has no `pass`.
//
// It said more than this once, and deliberately: two lines about `<transacts>` on a helper of your
// own, and four about `spawn{...}` being how a `<suspends>` method is started. Both described real
// walls, and both were cut, because a template is the first Verse an author reads and six lines of
// caveat before the first line of code is not an introduction. What they were for has not gone
// away -- `docs/dodge-the-creeps.md` wall 8 is still standing.
//
// Tabs, not spaces. Verse's own style guide prefers spaces and this deliberately does not follow
// it: Godot's `text_editor/behavior/indent/type` defaults to Tabs, so the first line the author
// types into a space-indented template *mixes* the two, which Verse rejects outright. The editor
// that will edit the file wins over the style guide that will not.
static const char *DEFAULT_TEMPLATE =
		"using { /Godot.org/Godot }\n"
		"\n"
		"# The class is named after this file, which is how the node it is attached to finds it.\n"
		"_CLASS_ := class(_BASE_):\n"
		"\n"
		"\t# Called when the node enters the scene tree for the first time.\n"
		"\t_Ready<override>():void =\n"
		"\t\t{} # Replace with function body.\n"
		"\n"
		"\t# Called every frame. `Delta` is the elapsed time since the previous frame.\n"
		"\t_Process<override>(Delta:float):void =\n"
		"\t\t{}\n";

// Godot hands the chosen template's content back here to be filled in, which is what GDScript's
// make_template does with it and what this used to ignore -- building the source from scratch and
// leaving `_get_built_in_templates` empty, so the Attach Script dialog reported "No suitable
// template." over a dialog that then wrote one (by-hand-findings.md B5).
//
// `_BASE_` is not GDScript's substitution: the dialog names a *Godot* class and the template needs
// the mirrored Verse one, so it goes through the same inversion the class table answers.
Ref<Script> VerseScriptLanguage::_make_template(const String &p_template, const String &p_class_name, const String &p_base_class_name) const {
	const String class_name = p_class_name.is_empty() ? String("script") : p_class_name;

	// No fallback: an empty template is an empty file, which is what the dialog asks for when its
	// Template checkbox is unchecked. Substituting the default here is what made that checkbox do
	// nothing (by-hand-findings.md B5).
	String source = p_template;
	source = source.replace("_CLASS_", class_name.to_snake_case());
	source = source.replace("_BASE_", verse_base_class_for(p_base_class_name));

	// memnew of a RefCounted answers a Ref<T> from Godot 4.7 on (godot-cpp's memnew_result
	// specialisation in ref.hpp, gated on GODOT_VERSION_MINOR >= 7), and the reference is already
	// counted -- so this hands the Ref straight back rather than taking a raw pointer.
	Ref<VerseScript> script = memnew(VerseScript);
	script->set_source_code(source);
	return script;
}

bool VerseScriptLanguage::_is_using_templates() {
	return true;
}

Object *VerseScriptLanguage::_create_script() const {
	Ref<VerseScript> script = memnew(VerseScript);

	// The caller takes ownership of a raw pointer, and since 4.7 the Ref above already holds the
	// one reference memnew counted -- so without this the script is freed the moment this returns.
	// One more reference leaves the count at one when the Ref goes out of scope, which is the state
	// Godot's own ScriptLanguage::create_script() hands back.
	script->reference();
	return script.ptr();
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
static TypedArray<Dictionary> diagnostics_fitted_to(const TypedArray<Dictionary> &p_diagnostics, const String &p_source) {
	const PackedStringArray lines = verse_newline_normalized(p_source).split("\n");
	if (lines.is_empty()) {
		return p_diagnostics;
	}

	TypedArray<Dictionary> fitted;
	for (int64_t i = 0; i < p_diagnostics.size(); i++) {
		Dictionary error = Dictionary(p_diagnostics[i]).duplicate();

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

// A compiler warning in the shape _validate's warnings array wants, which is not its errors'
// shape: Godot reads start_line, end_line and a string_code where an error has line and column.
// The string code is the compiler's own glitch number, which is what its Glitch.h is indexed by
// and how this repo refers to one; the ABI carries no name for it.
static Dictionary editor_warning_from(const Dictionary &p_diagnostic) {
	const int64_t line = p_diagnostic["line"];
	const int64_t column = p_diagnostic["column"];
	const int64_t code = p_diagnostic["code"];

	Dictionary warning;
	warning["start_line"] = line;
	warning["end_line"] = line;
	warning["leftmost_column"] = column;
	warning["rightmost_column"] = column;
	warning["code"] = code;
	warning["string_code"] = String("GLITCH_") + String::num_int64(code);
	warning["message"] = p_diagnostic["message"];
	return warning;
}

// The one warning a file that compiles can still need: some *other* file does not, so the next
// build publishes nothing and Play is refused.
//
// Named rather than counted where the list is short, because "4 files do not compile" sends the
// author looking and "mover.verse does not compile" sends them to the file. On line 1, since it is
// about the project and not about anything the author wrote here.
static Dictionary project_build_warning(const PackedStringArray &p_paths) {
	constexpr int64_t max_named = 3;
	String named;
	for (int64_t i = 0; i < p_paths.size() && i < max_named; i++) {
		named += (i > 0 ? String(", ") : String()) + p_paths[i].get_file();
	}
	const int64_t remaining = p_paths.size() - max_named;
	if (remaining > 0) {
		named += String(" and ") + String::num_int64(remaining) + String(remaining == 1 ? " more" : " more files");
	}

	Dictionary warning;
	warning["start_line"] = 1;
	warning["end_line"] = 1;
	warning["leftmost_column"] = 1;
	warning["rightmost_column"] = 1;
	warning["code"] = 0;
	warning["string_code"] = String("VERSE_PROJECT_BUILD");
	warning["message"] = String("This file compiles, but ") + named
			+ String(p_paths.size() == 1 ? " does not" : " do not")
			+ String(", so the project will not build and Play will be refused. Open ")
			+ String(p_paths.size() == 1 ? "it" : "them")
			+ String(" to see why, or Project > Tools > Build Verse to log every error at once.");
	return warning;
}

// `@global_class` on a class that is not the one named after its file registers nothing at all, and
// until now did so silently -- which is the defect, not the limitation. Godot collects one global
// class per script *path*: `_get_global_class_name` is a per-path virtual answering a single name,
// and ScriptServer maps that name back to the path, so a second name in one file has nowhere to
// live. GDScript has the same ceiling and gives no syntax for asking; this bridge accepts the
// attribute, so it owes the author a sentence. docs/property-export.md §"A second class in one file".
//
// A warning and not an error: the file compiles, the class is ordinary Verse, and a member typed as
// one still exports -- filtered by its nearest mirrored Godot class, which is what Stage A1 built.
// Only the request the attribute makes is impossible, so only that is reported.
//
// One sentence, two reporters. `_validate` puts it on the attribute's line in the script editor,
// where it appears and clears as the attribute is typed; report_name_collisions prints it once per
// build, which is the half a test can read -- a `_validate` warning is returned to the editor's C++
// and never reaches the log.
static String inert_global_class_message(const String &p_class_name, const String &p_file_stem) {
	return String("`@global_class` on `") + p_class_name
			+ String("` registers nothing. Godot collects one global class per script file, and only `")
			+ p_file_stem + String("` -- the class named after this file -- can be that class. Move `")
			+ p_class_name + String("` into a file of its own to register it, or drop the attribute: a ")
			+ String("member typed as `") + p_class_name
			+ String("` still exports, filtered by its nearest Godot base class.");
}

static Dictionary inert_global_class_warning(const String &p_class_name, const String &p_file_stem, int64_t p_line) {
	Dictionary warning;
	warning["start_line"] = p_line;
	warning["end_line"] = p_line;
	warning["leftmost_column"] = 1;
	warning["rightmost_column"] = 1;
	warning["code"] = 0;
	warning["string_code"] = String("VERSE_GLOBAL_CLASS_INERT");
	warning["message"] = inert_global_class_message(p_class_name, p_file_stem);
	return warning;
}

Dictionary VerseScriptLanguage::_validate(const String &p_script, const String &p_path, bool p_validate_functions, bool p_validate_errors, bool p_validate_warnings, bool p_validate_safe_lines) const {
	TypedArray<Dictionary> errors = diagnostics_fitted_to(check_buffer(p_path, p_script), p_script);

	// A Verse build is the whole project, so a file elsewhere that fails to compile refuses Play
	// with no sign in the editor until that moment. diagnostics_by_path already holds every file's
	// errors from the last whole-project analysis, each already carrying its own "path" key, and
	// ScriptTextEditor::_validate_script partitions any error whose path differs from this one into
	// its own clickable section of the errors panel.
	//
	// It reads that partition **only on the invalid branch**, though, and answering invalid is
	// expensive in a way that has nothing to do with this file: the connection gutter is cleared,
	// the method outline stops refreshing, the script list marks the tab as errored, and a stale
	// error bar is never cleared. GDScript pays that for a file this one *depends on*; here the
	// whole project is the dependency, so an unrelated broken file would degrade every open tab.
	//
	// So the list is appended only when this file is already invalid, where all of that is being
	// paid anyway and the extra sections are free. A clean file gets the warning below instead.
	PackedStringArray broken_elsewhere;
	TypedArray<Dictionary> elsewhere;
	const Array other_paths = diagnostics_by_path.keys();
	for (int64_t i = 0; i < other_paths.size(); i++) {
		const String other_path = other_paths[i];
		if (other_path == p_path) {
			continue;
		}
		const TypedArray<Dictionary> filed = TypedArray<Dictionary>(diagnostics_by_path[other_path]);
		if (filed.is_empty()) {
			continue;
		}
		broken_elsewhere.push_back(other_path);
		// Fitted against that file's own last-analyzed source, never p_script's -- a depended error
		// is only listed and clicked through here, but the column still has to sit inside a real
		// line before the editor displays it (see diagnostics_fitted_to above). No entry means no
		// analysis has read that file yet; pass its diagnostics through rather than fit them to the
		// wrong buffer.
		if (analyzed_source_by_path.has(other_path)) {
			elsewhere.append_array(diagnostics_fitted_to(filed, String(analyzed_source_by_path[other_path])));
		} else {
			elsewhere.append_array(filed);
		}
	}

	const bool own_errors = !errors.is_empty();
	if (own_errors) {
		errors.append_array(elsewhere);
	}

	Dictionary result;
	result["valid"] = errors.is_empty();
	result["errors"] = errors;

	// An export the host refused is not an error -- the script compiles and runs, it just has one
	// fewer property than the author asked for -- and it would be invisible without this: Godot
	// draws what the property list holds, and a member that never reaches it leaves nothing behind
	// to explain its absence. The list is whatever the last analysis of this file left behind,
	// which is the same staleness every diagnostic here has. The compiler's own warnings --
	// unreachable code, an empty block -- follow, fitted to the buffer the way the errors are.
	if (p_validate_warnings) {
		TypedArray<Dictionary> warnings;
		// The half of the cross-file report a clean file can afford. Godot reads the warnings key
		// whatever `valid` says, so this costs none of what appending the errors above would: it
		// says the build will be refused and by whom, and the errors themselves are one click away
		// in that file or one build away in the log.
		if (!own_errors && !broken_elsewhere.is_empty()) {
			warnings.push_back(project_build_warning(broken_elsewhere));
		}
		if (script_warnings_by_path.has(p_path)) {
			warnings.append_array(TypedArray<Dictionary>(script_warnings_by_path[p_path]));
		}
		// Read from the buffer rather than from the last analysis, so it appears and clears as the
		// attribute is typed and deleted: the scanner is text-only and has no host call in it. The
		// host could not answer this anyway -- the semantic program records the attribute as applied,
		// because it *is* applied; what it cannot know is that Godot has one slot per file.
		const VerseClassDecl decl = verse_scan_class_decl(p_script.utf8().get_data(),
				p_path.get_file().get_basename().utf8().get_data());
		for (const VerseClassDecl::InertGlobalClass &inert : decl.inert_global_classes) {
			warnings.push_back(inert_global_class_warning(String(inert.name.c_str()),
					p_path.get_file().get_basename(),
					// The scanner counts rows from zero; Godot's warning lines start at one.
					inert.attribute_line + 1));
		}
		const TypedArray<Dictionary> compiler_warnings = diagnostics_fitted_to(compiler_warnings_for(p_path), p_script);
		for (int64_t i = 0; i < compiler_warnings.size(); i++) {
			warnings.push_back(editor_warning_from(compiler_warnings[i]));
		}
		result["warnings"] = warnings;
	}

	// ScriptTextEditor::get_functions() reads this key alone to build the script editor's method
	// outline *and* to place the connection gutter icon beside a handler a scene connects to --
	// none of Script's own method-list virtuals are involved. Left unset when there is nothing to
	// answer from, the same as every other optional key here.
	//
	// Module-qualified, because every ClassNameUtf8 in the ABI is. Asking by bare stem answered
	// nothing for any script under a `.vmodule` marker, which cost those scripts both the outline
	// and the gutter (by-hand-findings.md B4).
	VerseRuntime *runtime = get_runtime();
	if (p_validate_functions && runtime != nullptr) {
		PackedStringArray functions;
		const TypedArray<Dictionary> members = runtime->class_members(qualified_class_name(p_path));
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
	return true;
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
	// Refresh, not rebuild. Godot calls this when the filesystem moved under it, and what a script
	// has to catch up with then is the analysis; publishing a generation is Play's job and the
	// Build action's. Snapshotted because compile() can republish an export list, which Godot is
	// free to drop a script in the middle of.
	const std::vector<VerseScript *> scripts = live_scripts;
	for (VerseScript *script : scripts) {
		script->compile();
	}
}

// Both of these mean "this script's source changed", which is the one thing that has to reach the
// objects holding it: an instance is bound to the generation it was made against and adopts
// nothing, and whether an object gets a real instance at all is decided by `@tool` at creation and
// never revisited. `_reload_all_scripts` above deliberately does not do this -- Godot calls that
// when the filesystem moved, which is a refresh rather than an edit.
void VerseScriptLanguage::_reload_tool_script(const Ref<Script> &p_script, bool p_soft_reload) {
	VerseScript *script = Object::cast_to<VerseScript>(p_script.ptr());
	if (script != nullptr) {
		script->_reload(p_soft_reload);
	}
}

void VerseScriptLanguage::_reload_scripts(const Array &p_scripts, bool p_soft_reload) {
	for (int64_t i = 0; i < p_scripts.size(); i++) {
		VerseScript *script = Object::cast_to<VerseScript>(p_scripts[i]);
		if (script != nullptr) {
			script->_reload(p_soft_reload);
		}
	}
}

String VerseScriptLanguage::_validate_path(const String &p_path) const {
	return String();
}

// The templates the Attach Script dialog lists, which is two.
//
// Answered against `Object` alone: ScriptCreateDialog walks the new script's base class up through
// ClassDB and asks per ancestor, and every hierarchy ends there, so both rows are offered whatever
// node the script is being attached to. Returning nothing -- which this did -- is how the dialog
// came to say "No suitable template." over a dialog that then wrote one, since `_make_template`
// runs either way (by-hand-findings.md B5).
//
// **"Empty" is a name Godot matches on**, not a label: unchecking the dialog's Template checkbox
// does not clear the content, it looks through this list for a built-in called exactly that
// (`ScriptCreateDialog::_get_current_template`). The row is what makes the checkbox mean anything.
//
// Godot drops a row missing any of these six keys and prints an error for it, and it assigns the
// real `id` itself while building the menu, so the one here only has to be unique. `origin` is
// ScriptLanguage::TEMPLATE_BUILT_IN, which godot-cpp does not expose as an enum.
TypedArray<Dictionary> VerseScriptLanguage::_get_built_in_templates(const StringName &p_object) const {
	TypedArray<Dictionary> templates;
	if (String(p_object) != String("Object")) {
		return templates;
	}

	auto add = [&templates](const String &p_name, const String &p_description, const char *p_content, int64_t p_id) {
		Dictionary entry;
		entry["inherit"] = String("Object");
		entry["name"] = p_name;
		entry["description"] = p_description;
		entry["content"] = String(p_content);
		entry["id"] = p_id;
		entry["origin"] = (int64_t)0;
		templates.push_back(entry);
	};

	add("Default", "A class named after the file, with _Ready and _Process.", DEFAULT_TEMPLATE, 0);
	add("Empty", "A blank file.", "", 1);
	return templates;
}

// The Verse spelling of a Godot type as the connect dialog names it (R-SIG-4).
//
// Godot hands make_function its arguments as "name:Type" pairs built from the signal's MethodInfo,
// so the type is a Godot *name* -- `int`, `String`, `Vector2`, `Node2D` -- and never a Verse one.
// A class goes through the generated table, which is the same inversion _make_template does.
//
// An unrecognised type answers empty rather than guessing, and the caller drops the parameter's
// annotation instead. A wrong type in a generated stub is worse than a missing one: the author sees
// a compile error on a line they did not write.
static String verse_type_for_godot_type(const String &p_godot_type) {
	if (p_godot_type.is_empty() || p_godot_type == "Variant") {
		return String();
	}
	if (p_godot_type == "bool") {
		return String("logic");
	}
	if (p_godot_type == "int" || p_godot_type == "float") {
		return p_godot_type;
	}
	if (p_godot_type == "String" || p_godot_type == "StringName" || p_godot_type == "NodePath") {
		return String("string");
	}
	if (p_godot_type == "Array") {
		return String("godot_array");
	}
	if (p_godot_type == "Dictionary") {
		return String("dictionary");
	}
	if (p_godot_type == "Callable") {
		return String("callable");
	}
	for (size_t i = 0; i < std::size(verse_api::classes); i++) {
		if (p_godot_type == verse_api::classes[i].godot_name) {
			return String(verse_api::classes[i].verse_name);
		}
	}
	return String();
}

// R-SIG-4's editor half: the handler the Node dock writes when "Make Function" is checked.
//
// Godot does the inserting -- ScriptTextEditor::add_callback finds the end of the file and writes
// what this returns -- so the whole job is the text, and the two things that make it Verse rather
// than GDScript are `<public>` and tabs. **Tabs are not a preference**: Godot's script editor writes
// tabs, and Verse rejects a file that mixes them with spaces, so a space-indented body would produce
// a stub that does not compile the moment the author types a second line.
//
// `<transacts>` because a signal handler is one: `Subscribe` fixes its callback at that effect, and
// a specifier-less function carries the wider default set, which a <transacts> context may not call.
// Writing it here is the difference between a stub that compiles and the wall this repository has
// tripped over most (`dodge-the-creeps.md` wall 8).
String VerseScriptLanguage::_make_function(const String &p_class_name, const String &p_function_name, const PackedStringArray &p_function_args) const {
	String out = String("\t") + p_function_name + String("<public>(");
	for (int64_t i = 0; i < p_function_args.size(); i++) {
		const String arg = p_function_args[i];
		const String name = arg.get_slice(":", 0);
		const String verse_type = verse_type_for_godot_type(arg.get_slice(":", 1));
		if (i > 0) {
			out += String(", ");
		}
		// A parameter Godot named but whose type has no Verse spelling still gets its name, so the
		// author has something to edit rather than a stub they have to re-derive from the dialog.
		out += name + (verse_type.is_empty() ? String(":?") : String(":") + verse_type);
	}
	// `{}` is Verse's `pass`, and the stub does not compile without it: a comment is not an
	// expression, so `= \n\t\t# TODO` is "Dangling `=` assignment with no expressions or empty
	// braced block `{}` on its right hand side" -- on a line the author did not write, in a file
	// the editor wrote for them (by-hand-findings.md B3). The wording is GDScript's own, because
	// the rest of what this language shows an author now is too.
	out += String(")<transacts>:void =\n\t\t{} # Replace with function body.\n");
	return out;
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

// The same test for the class-name sets, which are the two large ones, with the subsequence rule
// withheld until there is enough typed for it to be a search.
//
// A one-character prefix admits 927 of the 1026 mirrored class names as a subsequence, and CodeEdit
// rebuilds every possible subsequence match of every option it is handed, on the editor's thread,
// on every keystroke. Nothing is gained for it: ranking by contiguity cannot make one of 927 the
// answer either, so under the threshold the set is what the author has actually begun to spell.
// Past it `Prcs` still reaches `Process`, which is the half of fuzzy matching worth having.
static bool matches_typed_class_prefix(const String &p_name, const String &p_prefix) {
	constexpr int64_t SUBSEQUENCE_FROM = 3;
	return p_prefix.length() >= SUBSEQUENCE_FROM
			? matches_typed_prefix(p_name, p_prefix)
			: p_name.findn(p_prefix) == 0;
}

// Stands in for the identifier being typed while the completion buffer is analysed. A legal Verse
// identifier, so the line parses; one no project would write, so it resolves to nothing and the
// answer is about the position rather than about whatever it collided with.
static const char *completion_placeholder = "VhCompletionCursor";

#ifdef TOOLS_ENABLED

// The buffer _complete_code would hand the host for the caret where it is now: the identifier the
// caret is inside replaced by the placeholder, newlines normalised.
//
// Rebuilt from the editor rather than remembered, because it is the test for "still the same
// question". Two keystrokes into one identifier produce the same buffer, which is exactly right --
// the answer that just landed is the answer for both -- while a caret moved elsewhere, or a line
// edited, produces a different one and the refresh is dropped.
static String completion_placeholder_buffer(CodeEdit *p_code_edit) {
	const String text = p_code_edit->get_text();
	const int64_t caret_line = p_code_edit->get_caret_line();
	const int64_t caret_column = p_code_edit->get_caret_column();

	// get_text joins the lines with "\n", so one separator per line ahead of the caret's.
	int64_t offset = 0;
	for (int64_t i = 0; i < caret_line; i++) {
		if (i >= p_code_edit->get_line_count()) {
			return String();
		}
		offset += p_code_edit->get_line(i).length() + 1;
	}
	offset += caret_column;
	if (offset < 0 || offset > text.length()) {
		return String();
	}

	int64_t prefix_start = offset;
	while (prefix_start > 0 && is_identifier_char(text[prefix_start - 1])) {
		prefix_start--;
	}
	return verse_newline_normalized(text.substr(0, prefix_start) + String(completion_placeholder) + text.substr(offset));
}

#endif

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

// The innermost bracket before p_from that nothing has closed, or -1, with the character that
// opened it in r_opener.
//
// Crosses newlines, unlike enclosing_call_callee_end: a class header's parentheses and an
// archetype's braces both wrap, and the positions below are about which construct encloses the
// cursor rather than about one line. Strings and comments are not skipped for the same reason and
// at the same cost as that scan -- and completion declines inside both before reaching here.
static int64_t enclosing_open_bracket(const String &p_before, int64_t p_from, char32_t &r_opener) {
	int64_t depth = 0;
	for (int64_t i = p_from - 1; i >= 0; i--) {
		const char32_t c = p_before[i];
		if (c == ')' || c == ']' || c == '}') {
			depth++;
		} else if (c == '(' || c == '[' || c == '{') {
			if (depth > 0) {
				depth--;
				continue;
			}
			r_opener = c;
			return i;
		}
	}
	return -1;
}

static String word_ending_at(const String &p_text, int64_t p_end) {
	if (p_end < 0 || p_end >= p_text.length() || !is_identifier_char(p_text[p_end])) {
		return String();
	}
	int64_t start = p_end;
	while (start > 0 && is_identifier_char(p_text[start - 1])) {
		start--;
	}
	return p_text.substr(start, p_end - start + 1);
}

// Whether the identifier being typed stands where a type is expected.
//
// A `:` immediately before it is the whole test, because nothing else in Verse puts an identifier
// directly after one on the same line: a block header's `:` ends its line, and the `:` of `:=`
// cannot be adjacent to the name it binds. The run skipped over is the punctuation a type spelling
// puts between the colon and the name -- `:?node`, `:[]string`.
static bool completing_a_type(const String &p_before, int64_t p_prefix_start) {
	int64_t at = p_prefix_start;
	while (at > 0) {
		const char32_t c = p_before[at - 1];
		if (c == '?' || c == '[' || c == ']' || c == ' ' || c == '\t') {
			at--;
			continue;
		}
		return c == ':';
	}
	return false;
}

// Whether the cursor stands in the parentheses of a `class(...)`, `struct(...)` or
// `interface(...)` header, where only a class or an interface may be named.
static bool completing_a_supertype(const String &p_before, int64_t p_prefix_start) {
	char32_t opener = 0;
	const int64_t bracket = enclosing_open_bracket(p_before, p_prefix_start, opener);
	if (bracket < 0 || opener != '(') {
		return false;
	}
	int64_t end = bracket - 1;
	while (end >= 0 && (p_before[end] == ' ' || p_before[end] == '\t')) {
		end--;
	}
	// `class<unique>(...)` puts the specifiers between the keyword and its parentheses.
	if (end >= 0 && p_before[end] == '>') {
		while (end >= 0 && p_before[end] != '<') {
			end--;
		}
		end--;
		while (end >= 0 && (p_before[end] == ' ' || p_before[end] == '\t')) {
			end--;
		}
	}
	const String word = word_ending_at(p_before, end);
	return word == "class" || word == "struct" || word == "interface";
}

static bool is_reserved_word(const String &p_word) {
	for (size_t i = 0; i < std::size(verse_keywords::reserved_words); i++) {
		if (p_word == verse_keywords::reserved_words[i]) {
			return true;
		}
	}
	return false;
}

// The last byte of the class named before the `{` the cursor is inside, or -1 when the cursor is
// not naming one of its fields.
//
// Three things disqualify a cursor that is inside the braces. A `{` with no name in front of it is
// a block rather than an archetype. A field that has already been given its `:=` puts the cursor in
// the *value* instead, and the commas separate one field from the next, so the scan back stops at
// one. And a reserved word in front of the brace is one of the constructs that borrows the same
// syntax without taking field names -- `array{...}`, `map{...}`, `enum{...}`, `spawn{...}` -- where
// a field list is not merely the wrong answer but an empty popup, since the position declines to
// append anything else.
static int64_t archetype_class_end(const String &p_before, int64_t p_prefix_start) {
	char32_t opener = 0;
	const int64_t bracket = enclosing_open_bracket(p_before, p_prefix_start, opener);
	if (bracket < 0 || opener != '{') {
		return -1;
	}
	for (int64_t i = p_prefix_start - 1; i > bracket; i--) {
		const char32_t c = p_before[i];
		if (c == ',') {
			break;
		}
		if (c == '=') {
			return -1;
		}
	}
	const String name = word_ending_at(p_before, bracket - 1);
	return !name.is_empty() && !is_reserved_word(name) ? bracket - 1 : -1;
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

	// `location` is the only lever Godot offers over the order options appear in, and the host now
	// says how far each item is from what was asked about -- in the same hops Godot's own
	// LOCATION_PARENT_MASK counts, so a class's own members sort above its parent's above Object's.
	// Before this the whole mirror tied at LOCATION_OTHER and `Position`, `Name` and `Connect` came
	// back in one undifferentiated two-thousand-item list.
	//
	// A -1 is not a distance: it is a name reached through a `using`, which is how all 1026
	// mirrored classes and the Verse standard library come into scope at once. Those keep the older
	// distinction, the only one that could be drawn without the host -- the generated API below
	// anything a script or Verse itself declared.
	const String owner = p_item["owner"];
	const int64_t distance = p_item.get("owner_distance", -1);
	int64_t location = verse_godot_class_for(owner) == nullptr
			? ScriptLanguageExtension::LOCATION_LOCAL
			: ScriptLanguageExtension::LOCATION_OTHER;
	if (distance == 0) {
		location = ScriptLanguageExtension::LOCATION_LOCAL;
	} else if (distance > 0) {
		// Godot reads the low byte as the hop count, so a hierarchy deeper than that saturates
		// rather than wrapping into LOCATION_OTHER_USER_CODE.
		location = ScriptLanguageExtension::LOCATION_PARENT_MASK | (distance > 255 ? 255 : distance);
	}

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

// Every class a receiver of type p_verse_class reaches a member of, nearest first.
//
// Only a mirrored hierarchy is walked past its first link. A script class's own base is written in
// its file and in no table the host answers for, so following it would mean reading another file on
// the keystroke that opened the popup -- which costs more than a partial answer is worth when the
// full one is a frame or two behind it.
static PackedStringArray member_bearing_chain(const String &p_verse_class) {
	PackedStringArray chain;
	if (p_verse_class.is_empty()) {
		return chain;
	}
	chain.push_back(p_verse_class);
	const char *godot_name = verse_godot_class_for(p_verse_class);
	if (godot_name == nullptr) {
		return chain;
	}
	for (String name = ClassDB::get_parent_class(String(godot_name)); !name.is_empty();
			name = ClassDB::get_parent_class(name)) {
		if (const char *verse_name = mirrored_class(name)) {
			chain.push_back(String(verse_name));
		}
	}
	return chain;
}

// The class a member's declared type names, or empty. A `var` reads as `^node2d` and an `option`
// as `?node2d`; anything with a bracket in it is a container or a function type, which names no one
// class and is not worth a guess.
static String class_named_by_type(const String &p_type) {
	String name = p_type.strip_edges();
	while (!name.is_empty() && (name[0] == '^' || name[0] == '?')) {
		name = name.substr(1).strip_edges();
	}
	if (name.is_empty() || name.find("(") >= 0 || name.find("[") >= 0 || name.find(":") >= 0) {
		return String();
	}
	return name;
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
// API: every one of those 9597 methods is a class member the compiler would accept an override of,
// and overriding `GetName` compiles and changes nothing, because the body forwards through the
// handle either way. The second half is `is_virtual` on the method table, which is generated from
// the same `is_virtual` in extension_api.json that decided how to emit the member in the first
// place -- so the two cannot disagree.
//
// This used to exclude the mirror wholesale, and that was right until it wasn't: when the guard
// was written Godot's virtuals were hand-written on the native root, so "not a mirrored class" and
// "a virtual" named the same set. Phase 4 generated all 1413 of them onto the classes that declare
// them -- `_Ready` onto `node`, `_Draw` onto `canvas_item` -- and the guard silently began
// rejecting every override this exists to offer. Ask the table what the member *is* rather than
// where it lives (by-hand-findings.md B1).
//
// A name the table does not carry at all is a class the author wrote, which is worth offering, or
// a member of the native root that is not `_Notification`, which is not.
//
// A method the class already declares comes back owned by that class -- the host lets a subclass'
// copy win over the superclass' and drops the duplicate -- so comparing the owner is what stops an
// override that is already written from being offered again.
static bool completes_as_override(const Dictionary &p_item, const String &p_enclosing_class) {
	const String owner = p_item["owner"];
	if (!(bool)p_item["is_overridable"] || String(p_item["signature"]).is_empty() || owner == p_enclosing_class) {
		return false;
	}
	if (const verse_api::method_mapping *mirrored = godot_method_for(owner, p_item["name"])) {
		return mirrored->is_virtual;
	}
	return verse_godot_class_for(owner) == nullptr && owner != String("vh_object");
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

// The name a line of code begins with, or empty for a line that begins with anything else.
static String leading_identifier(const String &p_line) {
	int64_t start = 0;
	while (start < p_line.length() && (p_line[start] == ' ' || p_line[start] == '\t')) {
		start++;
	}
	int64_t end = start;
	while (end < p_line.length() && is_identifier_char(p_line[end])) {
		end++;
	}
	return p_line.substr(start, end - start);
}

// The class whose members are declared at p_line/p_indent, or empty when the cursor is somewhere
// a name is used rather than declared. Both answers come from one scan because an override option
// needs them together: whether to offer declarations at all, and which class' own methods are
// already written and so must not be offered again.
//
// r_declared is the second: every name the class body already begins a line with at the cursor's
// own indentation, which is where its members are. The host answers the same question off its
// snapshot, and correctly -- but the snapshot describes the last analysed text, and the method the
// author just finished typing is exactly what is not in it.
//
// The cursor's own line is left out. It carries whatever has been typed of the name being
// completed, and a prefix that happens to spell a whole inherited name would otherwise withdraw
// the one candidate the author is reaching for.
//
// Read from the text, because Godot asks for completion on every keystroke and an analysis of a
// half-written declaration would not report the class it belongs to anyway. The test is the one
// the indentation already encodes: every line of code between the class and the cursor is
// indented at least as far as the cursor, since a line indented less would be the header of the
// block the cursor is really inside.
//
// Only the class named after the file is found, so a member being added to one of the file's
// *other* top-level classes completes as an ordinary name. That is the conservative direction,
// and the file's own class is the one a script is actually written in.
static String member_declaration_class(const String &p_source, const String &p_file_stem, int64_t p_line, int64_t p_indent, PackedStringArray &r_declared) {
	const VerseClassDecl decl = verse_scan_class_decl(p_source.utf8().get_data(), p_file_stem.utf8().get_data());
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

	// Forward as well as back: a member declared below the cursor is as written as one above it.
	for (int64_t i = decl.line + 1; i < lines.size(); i++) {
		const int64_t indent = code_line_indent(lines[i]);
		if (indent >= 0 && indent < p_indent) {
			break;
		}
		if (indent != p_indent || i == p_line) {
			continue;
		}
		const String name = leading_identifier(lines[i]);
		if (!name.is_empty()) {
			r_declared.push_back(name);
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

// Same question, for a string literal. Same marker-removal reasoning as completing_in_comment.
static bool completing_in_string(const String &p_code, int64_t p_marker) {
	const String before = p_code.substr(0, p_marker);
	const int64_t line_start = before.rfind("\n") + 1;
	const String source = before + p_code.substr(p_marker + 1);
	return verse_position_in_string(source.utf8().get_data(),
			(int)before.count("\n"), (int)before.substr(line_start).utf8().length());
}

// The offset of the quote that opened the string literal the cursor is inside, or -1.
//
// Only ever asked after the lexer has said the cursor is in one, so this is a backward scan for the
// nearest unescaped quote rather than a second opinion about where strings begin. A Verse string
// does not span lines, so the scan stops at one.
static int64_t enclosing_string_start(const String &p_before) {
	for (int64_t i = p_before.length() - 1; i >= 0; i--) {
		const char32_t c = p_before[i];
		if (c == '\n') {
			return -1;
		}
		if (c != '"') {
			continue;
		}
		int64_t backslashes = 0;
		while (i - 1 - backslashes >= 0 && p_before[i - 1 - backslashes] == '\\') {
			backslashes++;
		}
		if (backslashes % 2 == 0) {
			return i;
		}
	}
	return -1;
}

// What a string literal is naming, when it is naming something the editor can enumerate.
enum class string_argument {
	none,
	node_path,
	resource_path,
	input_action,
	signal_name,
};

// The callee whose argument it is, which is the same key GDScript answers this question by --
// Object::get_argument_options, a virtual no GDExtension can reach. The callee is a word in the
// buffer, though, so the table is written out here instead of asked for.
//
// Names are the mirror's, which is Godot's own PascalCased: `get_node` is `GetNode`. An argument of
// -1 means every string argument of that call is one of these, which is what `GetVector`'s four
// action names need.
struct string_argument_rule {
	const char *callee;
	string_argument kind;
	int argument;
};

constexpr string_argument_rule string_argument_rules[] = {
	{ "GetNode", string_argument::node_path, 0 },
	{ "GetNodeOrNull", string_argument::node_path, 0 },
	{ "HasNode", string_argument::node_path, 0 },
	{ "FindChild", string_argument::node_path, 0 },
	{ "FindChildren", string_argument::node_path, 0 },
	{ "GetNodeAndResource", string_argument::node_path, 0 },

	{ "Load", string_argument::resource_path, 0 },
	{ "Exists", string_argument::resource_path, 0 },
	{ "LoadThreadedRequest", string_argument::resource_path, 0 },
	{ "ChangeSceneToFile", string_argument::resource_path, 0 },
	{ "Save", string_argument::resource_path, 1 },

	{ "IsActionPressed", string_argument::input_action, 0 },
	{ "IsActionJustPressed", string_argument::input_action, 0 },
	{ "IsActionJustReleased", string_argument::input_action, 0 },
	{ "IsActionReleased", string_argument::input_action, 0 },
	{ "IsAction", string_argument::input_action, 0 },
	{ "GetActionStrength", string_argument::input_action, 0 },
	{ "GetActionRawStrength", string_argument::input_action, 0 },
	{ "ActionPress", string_argument::input_action, 0 },
	{ "ActionRelease", string_argument::input_action, 0 },
	{ "HasAction", string_argument::input_action, 0 },
	{ "EraseAction", string_argument::input_action, 0 },
	{ "GetAxis", string_argument::input_action, -1 },
	{ "GetVector", string_argument::input_action, -1 },

	{ "Connect", string_argument::signal_name, 0 },
	{ "Disconnect", string_argument::signal_name, 0 },
	{ "IsConnected", string_argument::signal_name, 0 },
	{ "HasSignal", string_argument::signal_name, 0 },
	{ "GetSignalConnectionList", string_argument::signal_name, 0 },
	{ "MakeSignal", string_argument::signal_name, 1 },
};

static string_argument string_argument_kind(const String &p_callee, int64_t p_argument) {
	for (size_t i = 0; i < std::size(string_argument_rules); i++) {
		const string_argument_rule &rule = string_argument_rules[i];
		if (p_callee == rule.callee && (rule.argument < 0 || rule.argument == p_argument)) {
			return rule.kind;
		}
	}
	return string_argument::none;
}

// Every descendant of p_base, spelled the way a NodePath argument to GetNode wants it.
static void collect_node_paths(Node *p_base, Node *p_from, Array &r_options) {
	for (int64_t i = 0; i < p_from->get_child_count(); i++) {
		Node *child = p_from->get_child(i);
		if (child == nullptr) {
			continue;
		}
		r_options.push_back(completion_option(String(p_base->get_path_to(child)),
				ScriptLanguageExtension::CODE_COMPLETION_KIND_NODE_PATH,
				ScriptLanguageExtension::LOCATION_LOCAL));
		collect_node_paths(p_base, child, r_options);
	}
}

// Every file the editor's filesystem knows about, which is where a res:// path argument points.
// EditorFileSystem rather than a DirAccess walk for the reason GDScript uses it: the editor has
// already scanned the project, and a completion is not the place to scan it again.
static void collect_resource_paths(Array &r_options) {
#ifdef TOOLS_ENABLED
	EditorInterface *editor = EditorInterface::get_singleton();
	EditorFileSystem *filesystem = editor != nullptr ? editor->get_resource_filesystem() : nullptr;
	if (filesystem == nullptr) {
		return;
	}
	std::vector<EditorFileSystemDirectory *> pending;
	pending.push_back(filesystem->get_filesystem());
	while (!pending.empty()) {
		EditorFileSystemDirectory *directory = pending.back();
		pending.pop_back();
		if (directory == nullptr) {
			continue;
		}
		for (int32_t i = 0; i < directory->get_file_count(); i++) {
			r_options.push_back(completion_option(directory->get_file_path(i),
					ScriptLanguageExtension::CODE_COMPLETION_KIND_FILE_PATH,
					ScriptLanguageExtension::LOCATION_OTHER_USER_CODE));
		}
		for (int32_t i = 0; i < directory->get_subdir_count(); i++) {
			pending.push_back(directory->get_subdir(i));
		}
	}
#endif
}

// The project's input actions, which live in the settings under `input/` and nowhere else -- there
// is no InputMap to ask in the editor, because the editor does not load the project's own map.
static void collect_input_actions(Array &r_options) {
	const TypedArray<Dictionary> settings = ProjectSettings::get_singleton()->get_property_list();
	for (int64_t i = 0; i < settings.size(); i++) {
		const String name = Dictionary(settings[i])["name"];
		if (!name.begins_with("input/")) {
			continue;
		}
		r_options.push_back(completion_option(name.substr(6),
				ScriptLanguageExtension::CODE_COMPLETION_KIND_CONSTANT,
				ScriptLanguageExtension::LOCATION_OTHER_USER_CODE));
	}
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
// question, and so a cache hit: the host is already holding that buffer, so the answer is the warm
// half-millisecond rather than a ~750 ms analysis behind every character.
//
// The class-name and keyword sets are still offered alongside the compiler's answer for a bare
// identifier. They cover what a scope walk cannot -- a class the author has not brought into
// view, and the keywords, which are not definitions at all.
//
// One position answers differently: a bare identifier on a line of its own inside a class body is
// a member being declared, and an inherited method offered there completes to the whole
// declaration that overrides it rather than to a call. Everything else in scope is still offered,
// so a misread of the position costs nothing beyond an option that was already going to be there.
// The classes whose members a `.` at p_receiver_end reaches, nearest first, decided from the
// buffer and the snapshot alone -- which is the whole of what exists while the analysis that would
// answer properly is still running.
//
// Three receivers are knowable without one. `Self` is the class this file declares, and the class
// header names its base. A bare name the class declares carries a declared type, which the snapshot
// spells. And a mirrored class written outright is its own answer. Everything else -- a call's
// result, a local, a dotted chain -- needs the types this deliberately does not build, and answers
// nothing rather than guessing: a wrong member list is worse than a late one, because the author
// acts on it.
PackedStringArray VerseScriptLanguage::receiver_classes_from_text(const String &p_source, const String &p_path, int64_t p_receiver_end) const {
	VerseRuntime *runtime = get_runtime();
	const String word = word_ending_at(p_source, p_receiver_end);
	if (runtime == nullptr || word.is_empty()) {
		return PackedStringArray();
	}
	const int64_t word_start = p_receiver_end - word.length() + 1;
	if (word_start > 0 && p_source[word_start - 1] == '.') {
		return PackedStringArray();
	}

	const String own_class = qualified_class_name(p_path);
	if (word == String("Self")) {
		PackedStringArray chain;
		chain.push_back(own_class);
		// The base out of the buffer rather than out of the snapshot: a class header the author
		// has just changed is exactly the case this branch exists for, and verse_scan_class_decl
		// reads the text in front of them.
		const VerseClassDecl decl = verse_scan_class_decl(
				verse_newline_normalized(p_source).utf8().get_data(),
				p_path.get_file().get_basename().utf8().get_data());
		if (!decl.name.empty()) {
			if (const char *verse_base = mirrored_class(base_types_for(decl).instance_base)) {
				chain.append_array(member_bearing_chain(String(verse_base)));
			}
		}
		return chain;
	}

	const TypedArray<Dictionary> members = runtime->class_members(own_class);
	for (int64_t i = 0; i < members.size(); i++) {
		const Dictionary item = members[i];
		if (String(item["name"]) == word) {
			return member_bearing_chain(class_named_by_type(item["type"]));
		}
	}

	return verse_godot_class_for(word) != nullptr ? member_bearing_chain(word) : PackedStringArray();
}

// What a string literal at the cursor can be completed to, or nothing.
//
// Godot re-quotes every option handed back while the caret is inside a string (CodeEdit's
// _filter_code_completion_candidates), so the names here are bare and the editor puts the quotes
// back. Forced, because the popup is worth opening on the quote itself: none of these four is a
// name the author can be expected to have typed a prefix of.
void VerseScriptLanguage::complete_in_string(const String &p_code, const String &p_path, int64_t p_marker, Object *p_owner, Dictionary &r_result) const {
	const String before = p_code.substr(0, p_marker);
	const int64_t quote = enclosing_string_start(before);
	if (quote < 0) {
		return;
	}

	// The call scan starts at the quote rather than at the cursor: a bracket inside the literal is
	// text, and letting it count would make `GetNode("(")` look like a call that opened one.
	const String head = before.substr(0, quote);
	const int64_t callee_end = enclosing_call_callee_end(head);
	if (callee_end < 0) {
		return;
	}
	const String callee = word_ending_at(head, callee_end);
	const string_argument kind = string_argument_kind(callee, argument_index_in_call(head, callee_end));

	Array options;
	switch (kind) {
		case string_argument::node_path: {
			// The node this script is attached to in the edited scene, which Godot resolves before
			// asking and hands over here. Without one -- a script open with no scene around it --
			// there is no tree to name paths against.
			if (Node *base = Object::cast_to<Node>(p_owner)) {
				collect_node_paths(base, base, options);
			}
		} break;
		case string_argument::resource_path:
			collect_resource_paths(options);
			break;
		case string_argument::input_action:
			collect_input_actions(options);
			break;
		case string_argument::signal_name:
			collect_signal_names(p_code, p_path, callee_end - callee.length() - 1, options);
			break;
		case string_argument::none:
			return;
	}

	if (options.is_empty()) {
		return;
	}
	r_result["options"] = options;
	r_result["force"] = true;
}

// The signals the receiver of a `Connect`-like call can be asked for.
//
// Two sources, because a scripted node has two kinds: the Verse-spelled ones its own class declares
// (`Hit`, registered with Godot under that name) and Godot's own snake_case ones, which the nearest
// mirrored ancestor answers for with inheritance included. The chain is the same one member
// completion resolves a receiver by, so `Self.Connect("` and `Enemy.Connect("` both work.
void VerseScriptLanguage::collect_signal_names(const String &p_source, const String &p_path, int64_t p_receiver_end, Array &r_options) const {
	VerseRuntime *runtime = get_runtime();
	if (runtime == nullptr || !runtime->is_host_loaded()) {
		return;
	}

	PackedStringArray chain = receiver_classes_from_text(p_source, p_path, p_receiver_end);
	if (chain.is_empty()) {
		chain.push_back(qualified_class_name(p_path));
	}

	for (int64_t i = 0; i < chain.size(); i++) {
		if (const char *godot_class = verse_godot_class_for(chain[i])) {
			// With inheritance, so the first mirrored class in the chain is the last one to ask.
			const TypedArray<Dictionary> signals = ClassDB::class_get_signal_list(String(godot_class), false);
			for (int64_t s = 0; s < signals.size(); s++) {
				r_options.push_back(completion_option(Dictionary(signals[s])["name"],
						ScriptLanguageExtension::CODE_COMPLETION_KIND_SIGNAL,
						ScriptLanguageExtension::LOCATION_OTHER));
			}
			return;
		}

		const Vector<VerseSignalInfo> declared = runtime->class_signals(chain[i]);
		for (int64_t s = 0; s < declared.size(); s++) {
			r_options.push_back(completion_option(String(declared[s].name),
					ScriptLanguageExtension::CODE_COMPLETION_KIND_SIGNAL,
					ScriptLanguageExtension::LOCATION_LOCAL));
		}
	}
}

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

	// A string is not names either, and for a while answered nothing for that reason. What it holds
	// is decided by the call it is an argument to, and four of those name something the editor can
	// enumerate -- which is the whole of what GDScript completes inside a string too.
	if (completing_in_string(p_code, marker)) {
		complete_in_string(p_code, p_path, marker, p_owner, result);
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

	// A `<` is the other half of the same idea and a different set of names: Verse refuses an
	// attribute in the wrong position, so `public` is spellable only as `<public>` and `editable`
	// only as `@editable` (SemanticAnalyzer's ErrSemantic_InvalidAttributeScope). The host answers
	// the two as separate modes rather than one list asked for twice.
	//
	// Unlike `@` this is not forced, and a bare `<` with nothing typed falls through to the empty
	// prefix refusal below. `A<B` is a comparison Verse writes without spaces, and popping a
	// specifier list over one is noise; a specifier is always reached for with letters after it.
	const bool completing_specifier = !completing_members && !completing_attribute
			&& prefix_start > 0 && before[prefix_start - 1] == '<';

	const int64_t line_start = before.rfind("\n") + 1;
	const String ahead_of_prefix = before.substr(line_start, prefix_start - line_start);

	// The four positions that bound the answer by what Verse will accept there rather than by what
	// is in scope. Each is decided from the buffer alone, and each is a construct with no second
	// reading -- which is the whole reason to narrow on them and not on, say, an argument, where
	// any expression is legal and a narrowed list would hide the right name.
	//
	// Tested in this order: a `.`, an `@` and a `<` above have already claimed the cursor, and a
	// `set` target and a type position cannot both be true of one caret.
	const bool bounded = !completing_members && !completing_attribute && !completing_specifier;
	const bool completing_assignable = bounded && ahead_of_prefix.strip_edges() == String("set");
	const bool completing_type = bounded && !completing_assignable && completing_a_type(before, prefix_start);
	const bool completing_supertype = bounded && !completing_assignable && !completing_type
			&& completing_a_supertype(before, prefix_start);
	const int64_t archetype_end = bounded && !completing_assignable && !completing_type && !completing_supertype
			? archetype_class_end(before, prefix_start)
			: -1;
	const bool completing_field = archetype_end >= 0;

	// Whether the position's own answer is the whole of it. The class-name and keyword sets
	// appended at the end are names written on their own, so they belong to a bare identifier and
	// to a type position -- a mirrored class is a type -- and nowhere else.
	const bool answers_alone = completing_members || completing_attribute || completing_specifier
			|| completing_assignable || completing_field;
	const bool appends_keywords = !answers_alone && !completing_supertype;

	// A bare identifier, which is the only position the snapshot fallback and the override
	// declarations below are about: the others each have a narrower question to ask.
	const bool completing_a_bare_name = bounded && !completing_assignable && !completing_type
			&& !completing_supertype && !completing_field;

	// The class this is adding a member to, when that is what the cursor is doing: nothing but
	// indentation ahead of the prefix on its line, and that line belonging to the class body. An
	// inherited method offered there is being declared rather than called, and completes to the
	// whole declaration.
	PackedStringArray already_declared;
	const String declaring_in_class = completing_a_bare_name && !ahead_of_prefix.is_empty() && ahead_of_prefix.strip_edges().is_empty()
			? member_declaration_class(verse_newline_normalized(p_code), p_path.get_file().get_basename(),
					  before.count("\n"), ahead_of_prefix.length(), already_declared)
			: String();

	Array options;

	// Every name the host's answer already offered, so the sets appended below skip a row they
	// would otherwise duplicate. The overlap is not partial: a scope the file's `using` has
	// brought /Godot.org/Godot into carries all 1026 mirrored class names, and 28 of the 156
	// reserved words come back as the types they are (`int`, `float`, `logic`, ...), so without
	// this a prefix of `node` drew 178 duplicate rows and `spr` 169. Godot deduplicates nothing.
	//
	// Keyed by the item's bare `name` rather than by the option text: an override completes to a
	// whole declaration, and the appended sets are bare spellings.
	HashSet<String> host_offered_names;

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

			bool have_signature = signature_cache_source == source
					&& signature_cache_line == (int32_t)line
					&& signature_cache_column == (int32_t)column;
			if (!have_signature) {
				const String globalized = ProjectSettings::get_singleton()->globalize_path(p_path);
				bool not_ready = false;
				const Dictionary answer = runtime->signature_at(globalized, source, (int32_t)line, (int32_t)column, &not_ready);
				if (not_ready) {
					// The host has never analysed this buffer, and since ABI v7 it will not do it
					// here. Queue it and draw no hint; the analysis lands in a later _frame, which
					// asks the editor to complete again and arrives back here with an answer.
					request_check(p_path, source, true);
				} else {
					signature_cache = answer;
					signature_cache_source = source;
					signature_cache_line = (int32_t)line;
					signature_cache_column = (int32_t)column;
					have_signature = true;
				}
			}

			// Only ever the hint for *this* buffer. The cache keys are left alone on a refusal, so
			// without the guard the previous call's hint would be drawn over the new one.
			if (have_signature && !signature_cache.is_empty()) {
				result["call_hint"] = call_hint_for(signature_cache, argument_index_in_call(before, callee_end), call_opened_with_bracket(before, callee_end));
			}
		}
	}

	// Without a dot, a bare cursor would offer every name in scope as one undifferentiated list;
	// with one, the member set is bounded by the receiver's type and is exactly what was asked
	// for. An `@` bounds it just as tightly, and is worth showing unprompted for the same reason:
	// the attributes in scope are a short list and nothing else can follow it. So are an
	// archetype's fields and a `set` target, which are shorter still.
	if (!completing_members && !completing_attribute && !completing_assignable && !completing_field
			&& prefix.is_empty()) {
		return result;
	}
	result["force"] = completing_attribute;

	if (host_can_answer && (!completing_members || receiver_end >= 0)) {
		// Two of the modes are asked about a receiver's last byte -- the expression before a `.`,
		// and the class named before an archetype's `{` -- and the rest about where the name would
		// be written, which is where the prefix started. All of them sit before the substitution,
		// so none moves when the placeholder is a different length than what was typed.
		int64_t position = prefix_start;
		int32_t mode = VH_COMPLETE_SCOPE;
		if (completing_members) {
			position = receiver_end;
			mode = VH_COMPLETE_MEMBERS;
		} else if (completing_field) {
			position = archetype_end;
			mode = VH_COMPLETE_ARCHETYPE_FIELDS;
		} else if (completing_attribute) {
			mode = VH_COMPLETE_ATTRIBUTES;
		} else if (completing_specifier) {
			mode = VH_COMPLETE_SPECIFIERS;
		} else if (completing_assignable) {
			mode = VH_COMPLETE_ASSIGNABLE;
		} else if (completing_supertype) {
			mode = VH_COMPLETE_SUPERTYPES;
		} else if (completing_type) {
			mode = VH_COMPLETE_TYPES;
		}

		int64_t line = 0;
		int64_t column = 0;
		position_of(position, line, column);

		bool have_options = completion_cache_source == source && completion_cache_line == (int32_t)line
				&& completion_cache_column == (int32_t)column && completion_cache_mode == mode;
		if (!have_options) {
			const String globalized = ProjectSettings::get_singleton()->globalize_path(p_path);
			bool not_ready = false;
			const TypedArray<Dictionary> answer =
					runtime->complete_symbol(globalized, source, (int32_t)line, (int32_t)column, mode, &not_ready);
			if (not_ready) {
				// Queue the completion buffer and answer now with whatever is free. This is the
				// whole of the change ABI v7 bought: the analysis still costs ~1.3 s, but it is the
				// host's thread that spends it rather than the keystroke.
				request_check(p_path, source, true);
			} else {
				completion_cache_options = answer;
				completion_cache_source = source;
				completion_cache_line = (int32_t)line;
				completion_cache_column = (int32_t)column;
				completion_cache_mode = mode;
				have_options = true;
			}
		}

		if (have_options) {
			for (int64_t i = 0; i < completion_cache_options.size(); i++) {
				const Dictionary item = completion_cache_options[i];
				const String name = item["name"];
				if (!matches_typed_prefix(name, prefix)) {
					continue;
				}
				host_offered_names.insert(name);
				if (!declaring_in_class.is_empty() && completes_as_override(item, declaring_in_class)) {
					options.push_back(override_option_for(item));
				} else {
					options.push_back(completion_option_for(item));
				}
			}
		} else if (completing_members) {
			// The partial answer after a `.`, which without it is an empty popup that opens only
			// once the analysis lands -- late enough that the author has typed past it. What the
			// receiver's class and its bases declare, off the same snapshot and described by the
			// same host code the refined answer will use, so the rows offered here are the rows
			// that replace them.
			//
			// Nearest first, and a name a nearer class already answered is not offered twice: a
			// redeclared member belongs to the class that redeclared it, which is what
			// vh_complete_symbol would say too.
			const PackedStringArray chain = receiver_classes_from_text(source, p_path, receiver_end);
			HashSet<String> offered;
			for (int64_t c = 0; c < chain.size(); c++) {
				const TypedArray<Dictionary> members = runtime->class_members(chain[c]);
				for (int64_t i = 0; i < members.size(); i++) {
					Dictionary item = Dictionary(members[i]).duplicate();
					const String name = item["name"];
					if (!matches_typed_prefix(name, prefix) || offered.has(name)) {
						continue;
					}
					// class_members answers for one class, so every item it hands back calls itself
					// distance zero; here the step along the chain is the distance, and ranking is
					// the whole reason the chain is walked nearest-first.
					item["owner_distance"] = c;
					offered.insert(name);
					options.push_back(completion_option_for(item));
				}
			}
		} else if (completing_a_bare_name) {
			// The partial answer for a bare identifier: what the enclosing class declares, which
			// the analysis snapshot already holds and so costs nothing. LOCATION_LOCAL puts it
			// above the class names and keywords appended below -- Godot ranks by `location` alone
			// when nothing has been typed, and uses it as the fourth tie-break once something has.
			//
			// Members only, not the scope walk: everything else a scope admits lives in the AST,
			// which is exactly what no analysis of this buffer has built yet.
			const String class_name = qualified_class_name(p_path);
			const TypedArray<Dictionary> members = runtime->class_members(class_name);
			for (int64_t i = 0; i < members.size(); i++) {
				const Dictionary item = members[i];
				if (!matches_typed_prefix(item["name"], prefix)) {
					continue;
				}
				host_offered_names.insert(item["name"]);
				if (!declaring_in_class.is_empty() && completes_as_override(item, declaring_in_class)) {
					options.push_back(override_option_for(item));
				} else {
					options.push_back(completion_option_for(item));
				}
			}

			// The other half of what a member declaration is reaching for, and the half the scope
			// walk used to be the only source of: what the class inherits and could override. Off
			// the same snapshot, described by the same host code the refined answer will use, so
			// the declarations offered here are the ones that replace them -- same text, same
			// LOCATION_LOCAL, nothing to jump when the list is swapped.
			//
			// Nothing but overrides: an inherited name that is not one is an ordinary call, and
			// the thousands of them belong to the scope walk that has not run yet.
			if (!declaring_in_class.is_empty()) {
				const TypedArray<Dictionary> candidates = runtime->class_override_candidates(class_name);
				for (int64_t i = 0; i < candidates.size(); i++) {
					const Dictionary item = candidates[i];
					const String name = item["name"];
					if (!matches_typed_prefix(name, prefix) || already_declared.has(name)) {
						continue;
					}
					if (completes_as_override(item, declaring_in_class)) {
						host_offered_names.insert(name);
						options.push_back(override_option_for(item));
					}
				}
			}
		}
	}

	// A dot, an `@` and a `<` have all answered everything they are going to; the sets below are
	// names that could be written on their own, which is none of the three.
	if (answers_alone) {
		result["options"] = options;
		return result;
	}

	const PackedStringArray &mirrored = mirrored_class_names();
	for (int64_t i = 0; i < mirrored.size(); i++) {
		if (matches_typed_class_prefix(mirrored[i], prefix) && !host_offered_names.has(mirrored[i])) {
			options.push_back(completion_option(mirrored[i], ScriptLanguageExtension::CODE_COMPLETION_KIND_CLASS, ScriptLanguageExtension::LOCATION_OTHER));
		}
	}

	const PackedStringArray &class_names = script_class_names();
	for (int64_t i = 0; i < class_names.size(); i++) {
		if (matches_typed_class_prefix(class_names[i], prefix) && !host_offered_names.has(class_names[i])) {
			options.push_back(completion_option(class_names[i], ScriptLanguageExtension::CODE_COMPLETION_KIND_CLASS, ScriptLanguageExtension::LOCATION_OTHER_USER_CODE));
		}
	}

	// Not in a class header: a keyword is never a superclass, and `class(` is the one appended set
	// whose whole point is that only two kinds of name belong there.
	for (size_t i = 0; appends_keywords && i < std::size(verse_keywords::reserved_words); i++) {
		const String word = verse_keywords::reserved_words[i];
		if (matches_typed_prefix(word, prefix) && !host_offered_names.has(word)) {
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

	// GDScript answers a class name out of ClassDB before it parses anything; this is the same trick
	// over the mirror's static table, needing no marker, no analysis and no host call. Every refusal
	// below routes through it instead of returning `result` bare, so a hover on a mirrored class name
	// survives no host, no build yet, a stale buffer, a busy analysis and a lookup that named nothing
	// -- but never preempts an analysed answer, because it only runs where the function was about to
	// give up: a local or member that happens to share a class's spelling still resolves to itself.
	auto refuse_or_mirrored_class = [&]() -> Dictionary {
		if (const char *godot_class = p_symbol.is_empty() ? nullptr : godot_doc_class_for(p_symbol)) {
			result["result"] = (int64_t)OK;
			result["type"] = (int64_t)ScriptLanguageExtension::LOOKUP_RESULT_CLASS;
			result["class_name"] = String(godot_class);
		}
		return result;
	};

	VerseRuntime *runtime = get_runtime();
	if (!project_built || runtime == nullptr || !runtime->is_host_loaded()) {
		return refuse_or_mirrored_class();
	}

	// The editor marks the cursor by splicing U+FFFF into the buffer it hands over, and that is
	// the only place the position arrives: p_symbol is just the word under the pointer, which
	// cannot tell two same-named locals in different functions apart. The underline path asks
	// about the mouse rather than the caret and hands over an empty string when the pointer is
	// off the end of the text, so a missing marker is ordinary rather than a fault.
	const int64_t marker = p_code.find(String::chr(0xFFFF));
	if (marker < 0) {
		return refuse_or_mirrored_class();
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
		return refuse_or_mirrored_class();
	}

	// The host blocks on an in-flight analysis before touching the semantic program, and this
	// runs on the editor's thread. An analysis of some other file is the one case where the
	// buffer can be current and the host still busy; declining costs an underline for a frame.
	if (runtime->is_check_project_busy()) {
		return refuse_or_mirrored_class();
	}

	const String globalized = ProjectSettings::get_singleton()->globalize_path(p_path);
	const Dictionary found = runtime->lookup_symbol(globalized, (int32_t)line, (int32_t)column);
	if (found.is_empty()) {
		return refuse_or_mirrored_class();
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

// The host names a script by the absolute path vh_compile_project was given, verbatim -- which on
// Windows is a mix of separators, because that is what the consumer built the list out of. Godot's
// breakpoint list is keyed by res:// path, so this is the whole of the translation, and it is
// cached because the question is asked once per distinct location per frame.
String VerseScriptLanguage::res_path_for_source(const String &p_path) const {
	const std::string key(p_path.utf8().get_data());
	const auto found = res_path_by_source.find(key);
	if (found != res_path_by_source.end()) {
		return found->second;
	}
	const String localized = ProjectSettings::get_singleton()->localize_path(p_path.replace("\\", "/"));
	res_path_by_source[key] = localized;
	return localized;
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
bool VerseScriptLanguage::debug_should_break(const String &p_path, int32_t p_line, int32_t p_relation) {
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
void VerseScriptLanguage::debug_break() {
	EngineDebugger *debugger = EngineDebugger::get_singleton();
	if (debugger == nullptr) {
		return;
	}
	debugger->script_debug(this, true, false);
}

String VerseScriptLanguage::_debug_get_error() const {
	return break_reason;
}

int32_t VerseScriptLanguage::_debug_get_stack_level_count() const {
	VerseRuntime *runtime = get_runtime();
	return runtime != nullptr ? runtime->debug_stack_count() : 0;
}

int32_t VerseScriptLanguage::_debug_get_stack_level_line(int32_t p_level) const {
	VerseRuntime *runtime = get_runtime();
	if (runtime == nullptr) {
		return 0;
	}
	const Dictionary frame = runtime->debug_stack_frame(p_level);
	return frame.has("line") ? (int32_t)(int64_t)frame["line"] : 0;
}

String VerseScriptLanguage::_debug_get_stack_level_function(int32_t p_level) const {
	VerseRuntime *runtime = get_runtime();
	if (runtime == nullptr) {
		return String();
	}
	const Dictionary frame = runtime->debug_stack_frame(p_level);
	return frame.has("function") ? String(frame["function"]) : String();
}

String VerseScriptLanguage::_debug_get_stack_level_source(int32_t p_level) const {
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
Dictionary VerseScriptLanguage::_debug_get_stack_level_locals(int32_t p_level, int32_t p_max_subitems, int32_t p_max_depth) {
	return debug_values_at(p_level, VH_DEBUG_LOCALS, "locals");
}

Dictionary VerseScriptLanguage::_debug_get_stack_level_members(int32_t p_level, int32_t p_max_subitems, int32_t p_max_depth) {
	// Self and its fields. This is the only place a script instance's state appears -- see
	// _debug_get_stack_level_instance.
	return debug_values_at(p_level, VH_DEBUG_MEMBERS, "members");
}

Dictionary VerseScriptLanguage::debug_values_at(int32_t p_level, int32_t p_kind, const char *p_key) const {
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
// Two things follow. `self` is delivered through _debug_get_stack_level_members instead, and
// expression evaluation is unreachable: Godot's `evaluate` command bails before it would ask us.
void *VerseScriptLanguage::_debug_get_stack_level_instance(int32_t p_level) {
	return nullptr;
}

// Honestly empty, not a stub. A Verse module-level definition is a constant, not a mutable global
// a debugger would watch change, so there is nothing for this to answer with. It is bound REQUIRED,
// so it cannot be omitted the way an unsupported EXBIND virtual can.
Dictionary VerseScriptLanguage::_debug_get_globals(int32_t p_max_subitems, int32_t p_max_depth) {
	return Dictionary();
}

TypedArray<Dictionary> VerseScriptLanguage::_debug_get_current_stack_info() {
	return TypedArray<Dictionary>();
}

void VerseScriptLanguage::_profiling_start() {
	profiling_active = true;
	VerseRuntime *runtime = get_runtime();
	if (runtime != nullptr) {
		runtime->profiling_set_enabled(true);
	}
}

void VerseScriptLanguage::_profiling_stop() {
	profiling_active = false;
	VerseRuntime *runtime = get_runtime();
	if (runtime != nullptr) {
		runtime->profiling_set_enabled(false);
	}
}

// A no-op, and it stays one. Godot's "save native calls" asks a language to attribute time spent
// inside engine calls to the script that made them; the bridge already does that and cannot do
// otherwise -- a mirrored method call happens inside the Verse method's boundary row, so its time
// is in that row's total whether anyone asks for it or not.
void VerseScriptLanguage::_profiling_set_save_native_calls(bool p_enable) {
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

int32_t VerseScriptLanguage::fill_profiling_info(ScriptLanguageExtensionProfilingInfo *p_info_array, int32_t p_info_max, bool p_frame_only) {
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

int32_t VerseScriptLanguage::_profiling_get_accumulated_data(ScriptLanguageExtensionProfilingInfo *p_info_array, int32_t p_info_max) {
	return fill_profiling_info(p_info_array, p_info_max, false);
}

int32_t VerseScriptLanguage::_profiling_get_frame_data(ScriptLanguageExtensionProfilingInfo *p_info_array, int32_t p_info_max) {
	return fill_profiling_info(p_info_array, p_info_max, true);
}

// Attach whenever Godot's debugger is active, which is unconditionally correct and is what D6 asks
// for until a measurement says otherwise. The alternative -- a polled mirror that sweeps
// is_breakpoint over the lines Verse reports and attaches only when one exists -- buys back the
// per-op Notify at the cost of a breakpoint that arms on a delay, and S-2's number did not justify
// it (phase-6-design.md 13.1).
void VerseScriptLanguage::sync_debugger_attachment() {
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
			UtilityFunctions::push_warning(
					"Verse: Godot's debugger is active, but the Verse VM already has a debugger "
					"attached -- verse/host/enable_debugger opened Epic's socket debugger at "
					"startup. Breakpoints in the script editor will not fire. Turn that setting "
					"off to debug through Godot instead.");
		}
		debugger_attached = wanted; // do not ask again every frame
		return;
	}
	debugger_attached = wanted;
}

bool VerseScriptLanguage::_handles_global_class_type(const String &p_type) const {
	return p_type == _get_type();
}

// Read from the file's text, never from the host.
//
// EditorFileSystem asks this from its scan thread, for every .verse in the project, during the
// startup scan -- and both halves of that are out of the ABI's reach. Every vh_ entry point has
// to be called on the vh_init thread, and a build is the whole project at ~200ms, so a filesystem
// scan is the last thing that should be able to trigger one. GDScript answers the same question
// from a tokenizer-only pass for the same reason.
Dictionary VerseScriptLanguage::_get_global_class_name(const String &p_path) const {
	const String source = FileAccess::get_file_as_string(p_path);
	if (FileAccess::get_open_error() != OK) {
		return Dictionary();
	}

	const VerseClassDecl decl =
			verse_scan_class_decl(source.utf8().get_data(), p_path.get_file().get_basename().utf8().get_data());
	if (decl.name.empty()) {
		return Dictionary();
	}

	Dictionary result;
	result["base_type"] = base_types_for(decl).registry_base;
	result["is_abstract"] = decl.is_abstract;
	result["is_tool"] = decl.is_tool;
	// Presence of "name" is what registers the class: ScriptLanguageExtension::get_global_class_name
	// returns empty the moment the key is absent, so a script without the attribute must not set
	// it. The other keys are filled either way, as C#'s ScriptManagerBridge does.
	if (decl.is_global) {
		result["name"] = String(verse_pascal_case(decl.name).c_str());
	}
	return result;
}

PackedStringArray VerseScriptLanguage::script_paths_for_class(const String &p_class_name) const {
	PackedStringArray matches;
	const PackedStringArray sources = find_verse_sources("res://");
	for (int64_t i = 0; i < sources.size(); i++) {
		if (sources[i].get_file().get_basename() == p_class_name) {
			matches.push_back(sources[i]);
		}
	}
	return matches;
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
		const PackedStringArray base_paths = script_paths_for_class(base);
		if (base_paths.is_empty()) {
			break;
		}
		// Modules make a file stem ambiguous -- gameplay/player.verse and ui/player.verse both
		// answer to `player` -- and this scan is the one place that cannot resolve it. The
		// compiler has no such problem: it resolves through modules and `using`, and it is not
		// reachable from here (see _get_global_class_name). Reporting and falling back to Node is
		// the honest answer; the alternative, teaching this scan to follow `using` lines, would be
		// a second resolution path guaranteed to disagree with the compiler somewhere.
		// Saying so is report_name_collisions' job, on the main thread at build time: this runs on
		// EditorFileSystem's scan thread, where a warning would race the log and repeat once per
		// script in the project.
		if (base_paths.size() > 1) {
			break;
		}
		const String base_path = base_paths[0];
		const String base_source = FileAccess::get_file_as_string(base_path);
		if (FileAccess::get_open_error() != OK) {
			break;
		}
		const VerseClassDecl base_decl =
				verse_scan_class_decl(base_source.utf8().get_data(), base.utf8().get_data());
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
		// First, so a breakpoint set before anything else happens this frame is already armed.
		sync_debugger_attachment();
		poll_check();

#ifdef TOOLS_ENABLED
		// Connected here rather than in _init: a ScriptLanguage is registered before the editor's
		// own singletons exist, so there is nothing to connect to at that point. Once per process.
		if (!filesystem_hook_connected) {
			EditorInterface *editor_interface = verse_editor_interface();
			EditorFileSystem *filesystem = editor_interface != nullptr ? editor_interface->get_resource_filesystem() : nullptr;
			if (filesystem != nullptr) {
				filesystem->connect("filesystem_changed", Callable(this, "on_filesystem_changed"));
				filesystem_hook_connected = true;
			}
		}

		// Deliberately here rather than in poll_check: re-entering the script editor from inside
		// the poll would rebuild its error list while it is drawing a popup.
		if (editor_refresh_pending) {
			editor_refresh_pending = false;
			refresh_current_script_editor();
		}

		// After the error list and before the import, for the same reason: this asks the editor to
		// run _complete_code again, and that has to see the buffer everything else has settled on.
		if (completion_refresh_pending) {
			completion_refresh_pending = false;
			refresh_completion_if_current();
		}

		// After the refresh, so the validate the refresh asks for is about the text this is
		// about to change rather than the text before it. R-TOOL-12.
		insert_pending_import();
#endif

		// Before the tick, so the first frame's numbers are readable too. Idempotent.
		runtime->register_monitors();
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
	return find_project_files(p_dir, "verse");
}

PackedStringArray VerseScriptLanguage::find_project_files(const String &p_dir, const String &p_extension) {
	PackedStringArray found;

	Ref<DirAccess> dir = DirAccess::open(p_dir);
	if (dir.is_null()) {
		return found;
	}

	const PackedStringArray files = dir->get_files();
	for (int64_t i = 0; i < files.size(); i++) {
		if (files[i].get_extension().to_lower() == p_extension) {
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
		found.append_array(find_project_files(p_dir.path_join(subdirs[i]), p_extension));
	}

	return found;
}

void VerseScriptLanguage::refresh_module_map() const {
	const PackedStringArray sources = find_verse_sources("res://");
	const PackedStringArray markers = find_project_files("res://", "vmodule");

	std::vector<std::string> source_paths;
	std::vector<std::string> marker_paths;
	source_paths.reserve(sources.size());
	marker_paths.reserve(markers.size());
	for (int64_t i = 0; i < sources.size(); i++) {
		source_paths.push_back(std::string(sources[i].utf8().get_data()));
	}
	for (int64_t i = 0; i < markers.size(); i++) {
		marker_paths.push_back(std::string(markers[i].utf8().get_data()));
	}

	const VerseModuleMap map = verse_build_module_map(source_paths, marker_paths);
	module_by_script = map.module_by_source;
	module_map_built = true;

	for (const VerseModuleDiagnostic &diagnostic : map.diagnostics) {
		UtilityFunctions::push_error(String("res://") + String(diagnostic.marker_path.c_str())
				+ String(": ") + String(diagnostic.message.c_str()));
	}
}

String VerseScriptLanguage::module_for_script(const String &p_res_path) const {
	if (!module_map_built) {
		refresh_module_map();
	}
	const std::string key(p_res_path.utf8().get_data());
	auto found = module_by_script.find(key);
	if (found == module_by_script.end()) {
		// A script added since the last build. One more walk of res:// answers for it and for
		// every other new file at the same time.
		refresh_module_map();
		found = module_by_script.find(key);
	}
	return found == module_by_script.end() ? String() : String(found->second.c_str());
}

// Every warning `_validate` would draw in the gutter, once per build in the log.
//
// The second reporter, and the same pattern and the same reason as Stage B's: a `_validate` warning
// is handed to Godot's own C++ for the gutter and the warnings panel and **reaches no log**, so
// without this copy nothing outside a running editor can see one -- and nothing can assert one.
// Everything refresh_script_warnings produces was untested until this existed: the export rejections
// (R-EXP-2), the signal rejections (R-SIG-1) and B19 Stage C's "cannot be saved".
//
// The map is refreshed first rather than read as it stands, because the lists it is built from are
// the ones the build has just published -- and in a session that has only ever built, nothing has
// called `_validate` and the map is empty.
void VerseScriptLanguage::log_script_warnings(const PackedStringArray &p_sources) const {
	for (int64_t i = 0; i < p_sources.size(); i++) {
		const String path = p_sources[i];
		refresh_script_warnings(path);
		if (!script_warnings_by_path.has(path)) {
			continue;
		}
		const TypedArray<Dictionary> drawn(script_warnings_by_path[path]);
		for (int64_t w = 0; w < drawn.size(); w++) {
			const Dictionary warning = drawn[w];
			// The editor's own shape for a located diagnostic, which is what Stage B's copy uses and
			// what run_tests.py matches against.
			UtilityFunctions::push_warning(path + String(":")
					+ String::num_int64((int64_t)warning["start_line"]) + String(": ")
					+ String(warning["message"]));
		}
	}
}

void VerseScriptLanguage::report_name_collisions(const PackedStringArray &p_sources,
		const std::vector<std::string> &p_texts) const {
	// res:// path of the file that claimed each (module, class) pair and each Godot global name.
	std::map<std::string, String> owner_by_module_class;
	std::map<std::string, String> owner_by_global_name;

	for (int64_t i = 0; i < p_sources.size() && i < (int64_t)p_texts.size(); i++) {
		const String path = p_sources[i];
		const String stem = path.get_file().get_basename();
		const VerseClassDecl decl = verse_scan_class_decl(p_texts[i], stem.utf8().get_data());

		// Reported before the no-class-of-its-own test below, not after: a file whose *only*
		// `@global_class` is on a class that is not the file's declares no script at all, and the
		// attribute is exactly why its author thinks it does.
		//
		// This belongs beside the collision checks rather than apart from them -- both are about
		// what reaches Godot's one flat registry, and this pass is the once-per-build moment where
		// every source is in hand.
		for (const VerseClassDecl::InertGlobalClass &inert : decl.inert_global_classes) {
			UtilityFunctions::push_warning(path + String(":") + String::num_int64(inert.attribute_line + 1)
					+ String(": ") + inert_global_class_message(String(inert.name.c_str()), stem));
		}

		if (decl.name.empty()) {
			continue;
		}

		const String module = module_for_script(path);
		const std::string key = std::string(module.utf8().get_data()) + "/" + decl.name;
		const auto claimed = owner_by_module_class.find(key);
		if (claimed != owner_by_module_class.end()) {
			// The one diagnostic that has to teach the feature: it is the only place an author
			// finds out the marker exists, and its fix is the context-menu action.
			UtilityFunctions::push_error(path + String(" and ") + claimed->second
					+ String(" both declare `") + String(decl.name.c_str()) + String("` in ")
					+ (module.is_empty() ? String("the root module")
										 : String("module `") + module + String("`"))
					+ String(". A name may only be declared once per module. Put one of them in a module of ")
					+ String("its own -- right-click its directory in the FileSystem dock and choose ")
					+ String("\"Make Verse Module\" -- or rename one of the files."));
		} else {
			owner_by_module_class[key] = path;
		}

		if (decl.is_global) {
			const String godot_name = String(verse_pascal_case(decl.name).c_str());
			const std::string global_key(godot_name.utf8().get_data());
			const auto registered = owner_by_global_name.find(global_key);
			if (registered != owner_by_global_name.end()) {
				UtilityFunctions::push_error(path + String(" and ") + registered->second
						+ String(" both register the Godot class name `") + godot_name
						+ String("`. ClassDB is one flat namespace and a module is deliberately not part ")
						+ String("of it, so two @global_class classes may not share a name however far apart ")
						+ String("they are. Rename one of the files."));
			} else {
				owner_by_global_name[global_key] = path;
			}
		}

		// The pre-build class picker resolves a script base by matching the file stem across
		// res://, which modules can make ambiguous. It falls back to Node and says nothing, on a
		// scan thread where it cannot say anything; here is where it can.
		if (!decl.base.empty()) {
			const String base = String(decl.base.c_str());
			if (godot_class_for(decl.base).is_empty()) {
				const PackedStringArray candidates = script_paths_for_class(base);
				if (candidates.size() > 1) {
					String listed;
					for (int64_t c = 0; c < candidates.size(); c++) {
						listed += (c == 0 ? String() : String(", ")) + candidates[c];
					}
					UtilityFunctions::push_warning(path + String(" derives from `") + base
							+ String("`, and more than one script answers to that name (") + listed
							+ String("). The build resolves it through modules; the class picker cannot, ")
							+ String("so until this build finishes it offers Node as this script's base type."));
				}
			}
		}
	}
}

String VerseScriptLanguage::qualified_class_name(const String &p_res_path) const {
	const String stem = p_res_path.get_file().get_basename();
	const String module = module_for_script(p_res_path);
	return module.is_empty() ? stem : module + String("/") + stem;
}

const PackedStringArray &VerseScriptLanguage::script_class_names() const {
	if (!script_class_names_built) {
		const PackedStringArray sources = find_verse_sources("res://");
		script_class_names_cache.clear();
		for (int64_t i = 0; i < sources.size(); i++) {
			script_class_names_cache.push_back(sources[i].get_file().get_basename());
		}
		script_class_names_built = true;
	}
	return script_class_names_cache;
}

void VerseScriptLanguage::invalidate_script_class_names() const {
	script_class_names_built = false;
}

void VerseScriptLanguage::on_filesystem_changed() {
	// EditorFileSystem emits this at the end of every scan and once per frame for a batch of
	// update_file calls, which covers a file created, deleted, renamed or moved however it
	// happened -- through the dock, or on disk behind the editor's back. FileSystemDock has the
	// per-path signals, but only for what the dock itself did, so it would miss the second case.
	//
	// A flag rather than a rebuild: this fires during a scan, and the walk belongs to whoever
	// next asks a question that needs it.
	invalidate_script_class_names();
	module_map_built = false;
}

const PackedStringArray &VerseScriptLanguage::mirrored_class_names() {
	static PackedStringArray names = []() {
		PackedStringArray built;
		built.resize((int64_t)std::size(verse_api::classes));
		for (size_t i = 0; i < std::size(verse_api::classes); i++) {
			built.set((int64_t)i, String(verse_api::classes[i].verse_name));
		}
		return built;
	}();
	return names;
}

Error VerseScriptLanguage::ensure_project_built() {
	if (project_built) {
		return project_build_status;
	}
	return build_project();
}

Error VerseScriptLanguage::build_project() {
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

	// Re-derived per build rather than trusted: a .vmodule added or removed since the last one
	// moves files between modules, and a build is the moment that is allowed to take effect. The
	// class-name list is re-enumerated with it, since a build walks res:// anyway and is the one
	// hook a game -- where there is no EditorFileSystem to signal -- still has.
	invalidate_script_class_names();
	refresh_module_map();

	// An exported game has nothing to build. vh_init loaded the generation the cooker published,
	// and every .verse under res:// is a one-byte stub (D10) -- compiling those would replace a
	// working project with an empty one, if there were a compiler to do it with. The two lines
	// above are the part that still has to happen, because which module a script is in and which
	// class names are declared are read off res:// rather than out of the host.
	if (!runtime->host_has_compiler()) {
		project_built = true;
		project_build_status = OK;
		return OK;
	}

	const PackedStringArray sources = find_verse_sources("res://");
	PackedStringArray globalized;
	PackedStringArray modules;
	ProjectSettings *settings = ProjectSettings::get_singleton();
	for (int64_t i = 0; i < sources.size(); i++) {
		globalized.push_back(settings->globalize_path(sources[i]));
		modules.push_back(module_for_script(sources[i]));
	}

	// The host reports against the absolute path it was handed; scripts are keyed by res:// path.
	path_by_globalized.clear();
	for (int64_t i = 0; i < sources.size(); i++) {
		path_by_globalized[globalized[i]] = sources[i];
	}

	Dictionary errors_by_globalized;
	const Error status = runtime->compile_project(globalized, modules, &errors_by_globalized);

	// The host loaded each of these from disk just now, so this is the text it holds. Seeding it
	// here is what makes the *first* validate of a file free rather than only the repeats -- and
	// record_diagnostics below measures a diagnostic's span against it, so it has to be filled
	// first.
	analyzed_source_by_path.clear();
	std::vector<std::string> texts;
	texts.reserve(sources.size());
	for (int64_t i = 0; i < sources.size(); i++) {
		const String text = FileAccess::get_file_as_string(sources[i]);
		const bool read = FileAccess::get_open_error() == OK;
		if (read) {
			analyzed_source_by_path[sources[i]] = verse_newline_normalized(text);
		}
		texts.push_back(read ? std::string(text.utf8().get_data()) : std::string());
	}

	record_diagnostics(errors_by_globalized);

	// Every source is in hand exactly once per build, which is the only affordable moment to ask
	// the three questions that are about the project rather than about a file.
	report_name_collisions(sources, texts);
	log_script_warnings(sources);

	const Array reported = errors_by_globalized.keys();
	for (int64_t i = 0; i < reported.size(); i++) {
		log_build_diagnostics(TypedArray<Dictionary>(errors_by_globalized[reported[i]]));
	}

	project_built = true;
	project_build_status = status;

	if (status != OK) {
		// Nothing was published, so whatever ran before this still runs (R-ITER-5). The
		// diagnostics above say what is wrong; this says what that costs.
		UtilityFunctions::push_warning(
				"Verse: the project did not build, so no new code was published. The editor's analysis -- "
				"diagnostics, completion and the shape of the exported properties -- is live either way; "
				"fix the errors and build again to replace what is running.");
		return status;
	}

	// A new generation means new classes, new method tables and new declared defaults. Every
	// script has to re-read them off the program that now exists, and the inspector has to be
	// told, which is R-ITER-3. Snapshotted because refreshing a script republishes its export
	// list, and Godot is free to drop a script while that runs.
	const std::vector<VerseScript *> scripts = live_scripts;
	for (VerseScript *script : scripts) {
		script->generation_published();
	}

	return status;
}

TypedArray<Dictionary> VerseScriptLanguage::check_buffer(const String &p_path, const String &p_source) const {
	VerseRuntime *runtime = get_runtime();
	if (!project_built || runtime == nullptr || !runtime->is_host_loaded()) {
		return diagnostics_for(p_path);
	}

	// Anything the host does not already hold needs a fresh analysis, which takes ~750 ms -- some
	// forty-five frames. Start it on the host's thread and answer from the last one: returning stale
	// diagnostics for a moment is a far smaller cost than freezing the editor on every keystroke.
	// _frame picks the result up, and Godot re-validates often enough that the fresh answer lands
	// on its own.
	//
	// Nothing an analysis finds is written to the output log, neither here nor when the result
	// lands. It describes what the file said a moment ago, which may be a mistake the author has
	// already undone, and the log has no way to retract a line; the script editor's own error list
	// can show it because Godot replaces that wholesale on the next validate. The log is the
	// build's (build_project): a build is something the author asked for, and a failed one
	// refuses the run.
	if (!analysis_is_current(p_path, p_source)) {
		queue_check(p_path, p_source);
	}

	// Analysis covers the whole project, so a broken file elsewhere reports against its own path;
	// the editor asked about this one.
	return diagnostics_for(p_path);
}

bool VerseScriptLanguage::analysis_is_current(const String &p_path, const String &p_source) const {
	VerseRuntime *runtime = get_runtime();
	if (!project_built || runtime == nullptr || !runtime->is_host_loaded()) {
		return true;
	}

	// An exported game has no analysis and never will: the snapshot came out of the sidecar and is
	// the only one there is. Answering false here left every script waiting for a check that
	// nothing could run, so `valid` stayed false and not one scene came up with its script
	// attached -- with no error anywhere, because waiting is not failing.
	if (!runtime->host_has_compiler()) {
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

void VerseScriptLanguage::request_check(const String &p_path, const String &p_normalized_source, bool p_is_completion) const {
	// Recorded ahead of the in-flight test below, because the analysis already running may be the
	// very one this is asking for -- the editor asks for the options and the argument hint about
	// one keystroke, and the second ask must not lose the first's claim on the result.
	if (p_is_completion) {
		completion_refresh_path = p_path;
		completion_refresh_source = p_normalized_source;
	}

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
	pending_check_is_completion = p_is_completion;
	has_pending_check = true;

	// Queued, not started. Nothing that describes a class joins the analysis thread any more --
	// since ABI v7 they answer from the snapshot the last one left -- but an analysis still blocks
	// the VM for its whole length, so one begun in the middle of the editor's work is the pump and
	// every `@tool` instance stopped for ~750 ms of it. _frame starts it once the frame's own work
	// is done, and _frame is also the only thing that polls, so a buffer superseded before the next
	// one costs nothing at all.
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
	in_flight_is_completion = pending_check_is_completion;
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
		// Recorded for a completion buffer too, and that is the point: the entry says which text
		// the host is describing, so `_validate` comparing the author's real buffer against it
		// queues the ordinary analysis that puts the diagnostics back, and a hover declines in the
		// meantime rather than trusting loci measured against a spliced-in placeholder.
		analyzed_source_by_path[in_flight_path] = in_flight_source;

		if (in_flight_is_completion) {
			// Everything below describes the author's file to the author. This analysis was of a
			// line they are halfway through typing -- the placeholder resolves to nothing, so its
			// diagnostics are an unknown identifier they did not write -- and drawing that would
			// be worse than drawing nothing. The answer it was asked for is the program it left
			// behind, which vh_complete_symbol reads on the way back through.
			completion_refresh_pending = completion_refresh_path == in_flight_path
					&& completion_refresh_source == in_flight_source;
		} else {
			editor_refresh_pending = record_diagnostics(errors_by_globalized) || editor_refresh_pending;
			refresh_script_warnings(in_flight_path);

			// Every script whose compile() declined to wait for this. Told one at a time rather
			// than only the analysed file's script, because a save can be waiting on a result its
			// own buffer did not start. Snapshotted: telling a script republishes its export list,
			// and Godot is free to drop a script while that runs.
			const std::vector<VerseScript *> scripts = live_scripts;
			for (VerseScript *script : scripts) {
				editor_refresh_pending = script->analysis_landed() || editor_refresh_pending;
			}
		}

		in_flight_path = String();
		in_flight_source = String();
		in_flight_is_completion = false;
	}
}

// Asks the editor for completion again now that the host describes the buffer the last answer
// declined on.
//
// `request_code_completion(true)` is the same call Ctrl+Space makes. Forced, so it skips both the
// prefix test and CodeTextEditor's 0.3 s debounce and emits `code_completion_requested` straight
// away, which lands back in _complete_code -- where the host now answers and the cache fills. An
// open popup is refreshed in place rather than closed and reopened: CodeEdit only declines a
// re-request while one is open when every option showing is a path or a signal, which no answer
// here ever is, and the selected index survives unless the head of the list actually changed.
void VerseScriptLanguage::refresh_completion_if_current() const {
#ifdef TOOLS_ENABLED
	if (completion_refresh_path.is_empty()) {
		return;
	}

	EditorInterface *editor_interface = verse_editor_interface();
	ScriptEditor *script_editor = editor_interface != nullptr ? editor_interface->get_script_editor() : nullptr;
	if (script_editor == nullptr) {
		return;
	}

	// Only the file the analysis was for. The author may have switched tabs while it ran, and
	// asking some other script to complete would open a popup nobody asked for.
	const Ref<Script> script = script_editor->get_current_script();
	if (script.is_null() || script->get_path() != completion_refresh_path) {
		return;
	}

	ScriptEditorBase *current = script_editor->get_current_editor();
	CodeEdit *code_edit = current != nullptr ? Object::cast_to<CodeEdit>(current->get_base_editor()) : nullptr;
	if (code_edit == nullptr) {
		return;
	}

	// The caret has to still be inside the identifier the question was about. Anywhere else and
	// the answer that just landed is not the answer to what is being typed now.
	if (completion_placeholder_buffer(code_edit) != completion_refresh_source) {
		return;
	}

	code_edit->request_code_completion(true);
#endif
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
			// Narrow since B19: a class with no registered Godot name is exported anyway, filtered
			// by its nearest mirrored ancestor. What is left here is the case with no such ancestor
			// either -- a class whose chain reaches `object` without passing a mirrored one -- so
			// there is nothing to filter a slot by at all.
			return name + String(" refers to ") + class_name
					+ String(", which is neither a node nor a resource, so the inspector has nothing ")
					+ String("to draw for it. Derive ") + class_name
					+ String(" from a Godot class the inspector can pick one of.");
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

// What a refused signal has to say for itself, at the line that declared it.
//
// Every one of these was a runtime surprise before it was a warning, and three of them were silent:
// the member compiled, the signal was absent from Godot, and the author found out at the first
// emission or never. So each sentence names the rule and the edit that satisfies it.
static String signal_rejection_message(const VerseSignalInfo &p_signal) {
	const String name = String(p_signal.name);
	switch (p_signal.reject) {
		case VH_SIGNAL_IS_VAR:
			return name + String(" is a `var`, and a signal is an identity rather than a value. Its ")
					+ String("binding is made once against the object the member was built on, so ")
					+ String("reassigning it leaves the name pointing at nothing. Drop the `var`.");
		case VH_SIGNAL_NOT_PUBLIC:
			return name + String(" is not `<public>`, so nothing outside the class can connect to it ")
					+ String("-- which is the only thing connecting ever is. Declare it `")
					+ name + String("<public>`.");
		case VH_SIGNAL_NO_GODOT_OWNER:
			return name + String(" is on a class that does not derive from `object`, so Godot never ")
					+ String("gives it an object to register the signal on. Unlike GDScript, where ")
					+ String("every class is an Object with a signal table of its own, a plain Verse ")
					+ String("class has no Godot counterpart at all.");
		case VH_SIGNAL_PAYLOAD_NESTED_STRUCT:
			return name + String(" has a payload whose field `") + p_signal.reject_detail
					+ String("` is itself a struct. A struct payload becomes one Godot argument per ")
					+ String("top-level field, and Godot has no argument shape for a struct, so there ")
					+ String("is no second level to flatten into. Flatten the field, or carry it as ")
					+ String("one of the mirrored math types.");
		case VH_SIGNAL_PAYLOAD_UNSUPPORTED:
			return name + String(" has a payload argument `") + p_signal.reject_detail
					+ String("` with no Godot type, so an emission would have nothing to carry it in.");
		default:
			return name + String(" cannot be registered with Godot, so nothing can connect to it.");
	}
}

// What a refused `@rpc` has to say for itself, at the line that declared the method.
//
// Every one of these is silent otherwise: the attribute compiles -- its constructor only has to
// typecheck -- and the method is simply not in the config Godot reads, so the author finds out at
// the first call that goes nowhere, or never. GDScript's own messages are the model, and the first
// of them lists the seven words because guessing which one was meant is not this bridge's job.
static String rpc_rejection_message(const VerseRpcInfo &p_rpc) {
	const String name = String(p_rpc.name);
	switch (p_rpc.reject) {
		case VH_RPC_UNKNOWN_ARGUMENT:
			return name + String(": `") + p_rpc.reject_detail
					+ String("` is not an @rpc word. It must be one of \"call_local\"/\"call_remote\" ")
					+ String("(local calls), \"any_peer\"/\"authority\" (permission), or ")
					+ String("\"reliable\"/\"unreliable\"/\"unreliable_ordered\" (transfer mode).");
		case VH_RPC_DUPLICATE_CATEGORY:
			return name + String(": ") + p_rpc.reject_detail
					+ String(" is given twice. Each of the three may be said no more than once.");
		default:
			return name + String(": @rpc wants ") + p_rpc.reject_detail
					+ String(" in this position.");
	}
}

static String rpc_rejection_code(int32_t p_reject) {
	switch (p_reject) {
		case VH_RPC_UNKNOWN_ARGUMENT:
			return String("RPC_UNKNOWN_ARGUMENT");
		case VH_RPC_DUPLICATE_CATEGORY:
			return String("RPC_DUPLICATE_CATEGORY");
		default:
			return String("RPC_BAD_ARGUMENT_TYPE");
	}
}

static String signal_rejection_code(int32_t p_reject) {
	switch (p_reject) {
		case VH_SIGNAL_IS_VAR:
			return String("SIGNAL_IS_VAR");
		case VH_SIGNAL_NOT_PUBLIC:
			return String("SIGNAL_NOT_PUBLIC");
		case VH_SIGNAL_NO_GODOT_OWNER:
			return String("SIGNAL_NO_GODOT_OWNER");
		case VH_SIGNAL_PAYLOAD_NESTED_STRUCT:
			return String("SIGNAL_PAYLOAD_NESTED_STRUCT");
		default:
			return String("SIGNAL_PAYLOAD_UNSUPPORTED");
	}
}

// Whether an `@export` that Godot *will* draw is one whose value cannot survive a save.
//
// B19 Stage C. The class a second-class member holds has no script -- only the class named after
// the file can be one -- so its Godot peer is a bare `Resource` carrying none of the Verse object's
// members, and `ResourceSaver` writes an empty sub-resource. The member reads back as the empty
// option. C1's rule is to keep the class in its own file; this is the rule said at the member
// rather than only in a document.
//
// The host cannot be asked directly: A1 overwrites HintString with the fallback class, so on the
// wire an unregistered script class and an ordinary mirrored member are the same three fields. What
// separates them is the member's *declared* type, which the member list spells as Verse source --
// `^?stowaway` -- and which `vh_has_class` then answers for, because it is true only of a class the
// project itself declares. Both reads come from the analysis snapshot, so this costs no build and
// never waits (see CLAUDE.md, "The editor's thread").
static bool export_value_cannot_persist(const VerseRuntime &p_runtime, const String &p_class_name,
		const Dictionary &p_entry, const TypedArray<Dictionary> &p_members) {
	// A registered script class keeps VH_EXPORT_HINT_SCRIPT_CLASS and is not this case; only the
	// fallback to a mirrored name is.
	if ((int64_t)p_entry["hint"] != VH_EXPORT_HINT_CLASS) {
		return false;
	}

	const String member_name = p_entry["name"];
	String declared;
	for (int64_t i = 0; i < p_members.size(); i++) {
		const Dictionary member = p_members[i];
		if (String(member["name"]) == member_name) {
			declared = member["type"];
			break;
		}
	}
	// `^?stowaway`, and both decorations are load-bearing to strip in that order: the member list
	// spells a type as Verse source, where a `var` member's type is a reference -- `^` -- around the
	// option that every exported class-typed member is. Measured rather than assumed; the `^` is
	// what this test missed on its first writing, and it silenced the warning entirely.
	declared = declared.strip_edges();
	if (declared.begins_with("^")) {
		declared = declared.substr(1).strip_edges();
	}
	if (declared.begins_with("?")) {
		declared = declared.substr(1).strip_edges();
	}
	if (declared.is_empty()) {
		return false;
	}

	// The member list spells a type the way the source does, so a class in the script's own module
	// arrives bare and has to be re-qualified before the host will recognise it (R-LANG-6; every
	// ClassNameUtf8 in the ABI is module-qualified).
	const int module_end = p_class_name.rfind("/");
	if (module_end >= 0 && p_runtime.has_class(p_class_name.substr(0, module_end + 1) + declared)) {
		return true;
	}
	return p_runtime.has_class(declared);
}

void VerseScriptLanguage::refresh_script_warnings(const String &p_path) const {
	VerseRuntime *runtime = get_runtime();
	if (runtime == nullptr || !runtime->is_host_loaded()) {
		return;
	}

	TypedArray<Dictionary> warnings;
	bool found = false;
	const TypedArray<Dictionary> exports = runtime->class_exports(qualified_class_name(p_path), &found);
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

	// The exports Godot draws happily but cannot persist. Separate from the loop above because the
	// reject code is VH_EXPORT_OK -- the slot is real, the picker is real, and it is only the *save*
	// that loses the value, which is why this reads as its own sentence rather than as a rejection.
	const TypedArray<Dictionary> members = found
			? runtime->class_members(qualified_class_name(p_path))
			: TypedArray<Dictionary>();
	for (int64_t i = 0; found && i < exports.size(); i++) {
		const Dictionary entry = exports[i];
		const int64_t line = entry["line"];
		if ((int64_t)entry["reject"] != VH_EXPORT_OK || line < 0) {
			continue;
		}
		if (!export_value_cannot_persist(*runtime, qualified_class_name(p_path), entry, members)) {
			continue;
		}

		const String name = entry["name"];
		Dictionary warning;
		warning["start_line"] = line + 1;
		warning["end_line"] = line + 1;
		warning["leftmost_column"] = (int64_t)entry["column"] + 1;
		warning["rightmost_column"] = (int64_t)entry["column"] + 1;
		warning["code"] = 0;
		warning["string_code"] = String("VERSE_EXPORT_NOT_PERSISTED");
		warning["message"] = name
				+ String(" can be assigned in the inspector but not saved. Its class is not the one ")
				+ String("named after its file, so it has no script -- and a value survives a save ")
				+ String("only through one. Saving writes an empty sub-resource and the member ")
				+ String("reloads empty. Move the class into a file of its own.");
		warnings.push_back(warning);
	}

	// The same pass over the signal list, which the host rejects for its own five reasons. Reported
	// here rather than at the emission that used to discover them, which is R-SIG-1's half of
	// "refused at the member" and the reason vh_signal_desc carries a Reject at all.
	const Vector<VerseSignalInfo> signals = runtime->class_signals(qualified_class_name(p_path));
	for (int64_t i = 0; i < signals.size(); i++) {
		const VerseSignalInfo &signal = signals[i];
		if (signal.reject == VH_SIGNAL_OK || signal.line < 0) {
			continue;
		}

		Dictionary warning;
		warning["start_line"] = (int64_t)signal.line + 1;
		warning["end_line"] = (int64_t)signal.line + 1;
		warning["leftmost_column"] = (int64_t)signal.column + 1;
		warning["rightmost_column"] = (int64_t)signal.column + 1;
		warning["code"] = (int64_t)signal.reject;
		warning["string_code"] = signal_rejection_code(signal.reject);
		warning["message"] = signal_rejection_message(signal);
		warnings.push_back(warning);
	}

	// And the same pass over the `@rpc` list, for the same reason: a configuration the bridge
	// refused is a method the author believes is remote-callable and Godot has never heard of.
	const Vector<VerseRpcInfo> rpcs = runtime->class_rpcs(qualified_class_name(p_path));
	for (int64_t i = 0; i < rpcs.size(); i++) {
		const VerseRpcInfo &rpc = rpcs[i];
		if (rpc.reject == VH_RPC_OK || rpc.line < 0) {
			continue;
		}

		Dictionary warning;
		warning["start_line"] = (int64_t)rpc.line + 1;
		warning["end_line"] = (int64_t)rpc.line + 1;
		warning["leftmost_column"] = (int64_t)rpc.column + 1;
		warning["rightmost_column"] = (int64_t)rpc.column + 1;
		warning["code"] = (int64_t)rpc.reject;
		warning["string_code"] = rpc_rejection_code(rpc.reject);
		warning["message"] = rpc_rejection_message(rpc);
		warnings.push_back(warning);
	}

	// Written even when empty: a member the author has just fixed has to lose its warning, and the
	// analysis that proves it is this one.
	script_warnings_by_path[p_path] = warnings;
}

namespace {

// What a skipped member's reason means to the author who just wrote its name.
//
// R-SCN-2's whole point: a method that is not there for a good reason and a method that is not
// there by accident look identical from the editor, and only one of them is worth working around.
// The reason strings are gen_verse_api.py's own, so a new one shows up as an unexplained skip here
// rather than being silently rendered as nothing.
String skipped_member_explanation(const verse_api::skipped_member &p_entry) {
	const String reason = String(p_entry.reason);
	const String detail = String(p_entry.detail);
	if (reason == "superseded_by_property") {
		return String("it is reachable as the property ") + detail + ".";
	}
	if (reason == "property_renamed") {
		return String("a Verse function already answers to that name, so it is the property ")
				+ detail + ".";
	}
	if (reason == "superseded_by_free_function") {
		return String("it is reachable as ") + detail + ", which is also what string interpolation uses.";
	}
	if (reason.begins_with("property_")) {
		return String("it cannot be a property, so Godot's own ") + detail + " carry it instead.";
	}
	// Since Phase 4 a virtual is *emitted* rather than skipped, so this reason no longer appears
	// for one Godot describes. What is still skipped is a virtual whose return type has no default
	// a script could write -- an object, a typed container -- because an unoverridden one has to
	// answer something and there is nothing to answer with.
	if (reason == "virtual_no_default") {
		return String("it is a Godot virtual returning ") + detail
				+ ", and an unoverridden virtual has to answer a value there is no way to write (R-NODE-7).";
	}
	if (reason == "static") {
		return "it is static, and a static call has no Verse spelling yet (R-NODE-4).";
	}
	if (reason == "vararg") {
		return "it takes a variable number of arguments, which the bridge cannot carry.";
	}
	if (reason == "unmarshallable_pointer") {
		return "it takes a raw C pointer, which no scripting language can pass.";
	}
	if (reason == "unsupported_type") {
		return String("nothing can carry its ") + detail + " across the boundary.";
	}
	if (reason == "shadow") {
		return "a name it shares with an inherited member won.";
	}
	// The math types are ordinary Verse rather than a mirror over an ABI (OQ-11), so a missing
	// method is not a bridge limitation -- it is a body nobody has written yet, and the fix is to
	// write it. Said plainly, because "unsupported" would be false and would stop someone who could
	// have added it in ten minutes.
	if (reason == "math_not_written") {
		return String("the math types are ordinary Verse rather than calls into Godot, and this one ")
				+ String("has not been written yet -- host/Verse/GodotMath.native.verse is where it goes.");
	}
	// R-AUD-2's rule made visible: where Verse and Godot differ only in spelling, Verse's wins, and
	// this is where an author who typed Godot's finds that out. 86 of the 114 utilities answer here.
	if (reason == "utility_has_verse_spelling") {
		return String("Verse spells it ") + detail + String(".");
	}
	// A `variant` is deliberately unspellable by a script (R-TYPE-7 keeps the packers module-scoped),
	// so these have no signature a script could call even if the bridge dispatched them.
	if (reason == "utility_variant_only") {
		return String("its parameter or result is a Variant, which a script cannot spell -- the ")
				+ String("packers are module-scoped by R-TYPE-7, so there is no signature for it to have.");
	}
	// Currently unreachable, and deliberately kept: the generator's three tables cover all 114 of
	// Godot's utilities today, so this is what a utility a *future* Godot adds would fall to until
	// someone classifies it. An unexplained skip is the failure mode R-SCN-2 exists to prevent.
	if (reason == "utility_not_dispatched") {
		return String("it has no Verse counterpart and is not dispatched to Godot yet.");
	}
	if (reason == "math_operator_not_written") {
		return String("this operator has not been written for those operands yet -- the math types are ")
				+ String("ordinary Verse, and host/Verse/GodotMath.native.verse is where it goes.");
	}
	return String("it was skipped: ") + reason + ".";
}

// The Godot member a script named that the mirror does not carry, searched up the class chain.
//
// Up the chain because the compiler names the class the member was *looked for* on, which is the
// most derived one -- `sprite2d` for a method Node declares. ClassDB is what knows the chain, and
// the two name tables are what cross between its spelling and Verse's.
const verse_api::skipped_member *skipped_member_for(const String &p_verse_class, const String &p_member) {
	const char *godot_name = verse_godot_class_for(p_verse_class);
	if (godot_name == nullptr) {
		// A math type. ClassDB has never heard of Vector2 -- it is a Variant type, not a class --
		// so there is no chain to walk and no parent to inherit from: a vector2's members are all
		// its own. Matched on the Verse name the skip row already carries.
		for (size_t i = 0; i < std::size(verse_api::skipped); i++) {
			const verse_api::skipped_member &entry = verse_api::skipped[i];
			if (p_member == entry.verse_name && p_verse_class == entry.verse_class) {
				return &entry;
			}
		}
		return nullptr;
	}
	for (String godot_class = String(godot_name); !godot_class.is_empty();
			godot_class = ClassDB::get_parent_class(godot_class)) {
		for (size_t i = 0; i < std::size(verse_api::skipped); i++) {
			const verse_api::skipped_member &entry = verse_api::skipped[i];
			if (p_member == entry.verse_name && godot_class == entry.godot_class) {
				return &entry;
			}
		}
	}
	return nullptr;
}

// The same member, when the compiler could not say which class it belongs to.
//
// A call written inside the class body has no receiver -- `GetPosition()` rather than
// `Node.GetPosition()` -- and Verse reports it as an unknown *identifier*, which names no class at
// all. That is the common spelling and the one an author is most likely to reach for, so it cannot
// be the one case that goes unexplained.
//
// Answered only when every class that skips a member of this name skips it for the same reason and
// under the same Godot name. Where they disagree the honest answer is silence: the qualified form
// still explains itself, and a confident wrong class is worse than no sentence.
const verse_api::skipped_member *unambiguous_skipped_member(const String &p_member) {
	const verse_api::skipped_member *found = nullptr;
	for (size_t i = 0; i < std::size(verse_api::skipped); i++) {
		const verse_api::skipped_member &entry = verse_api::skipped[i];
		if (p_member != entry.verse_name) {
			continue;
		}
		if (found == nullptr) {
			found = &entry;
			continue;
		}
		if (String(found->godot_name) != String(entry.godot_name)
				|| String(found->reason) != String(entry.reason)
				|| String(found->detail) != String(entry.detail)) {
			return nullptr;
		}
	}
	return found;
}

} // namespace

// The Verse compiler's code for both "Unknown identifier %s." and "Unknown member %s in %s." --
// uLang's ErrSemantic_UnknownIdentifier, from Glitch.h, which is one code for the two wordings.
// Matched on the code rather than on the message, which is English and is not ours.
static constexpr int64_t UNKNOWN_IDENTIFIER_CODE = 3506;

// Verse identifiers are ASCII, so a byte test is the same test as a character test and can be made
// against the utf8 the compiler's columns count.
static bool is_identifier_byte(char p_c) {
	return std::isalnum(static_cast<unsigned char>(p_c)) || p_c == '_';
}

// The identifier a diagnostic's span begins at, taken out of the source the analysis read.
//
// The span's *end* is only used to reject a span that cannot be naming one identifier: uLang
// documents its locus as one-indexed and says nothing about whether the end is inclusive, and an
// identifier carries its own boundary, so the run of identifier bytes from the first byte is both
// simpler than trusting the end and exact. Columns are utf8 bytes, which is how the compiler counts
// and is not how Godot counts -- so the slice is taken out of the line's bytes and decoded back.
static String identifier_at_span(const String &p_source, const Dictionary &p_diagnostic) {
	const int64_t line = p_diagnostic.get("line", 0);
	const int64_t column = p_diagnostic.get("column", 0);
	const int64_t end_line = p_diagnostic.get("end_line", 0);
	if (line < 1 || column < 1 || (end_line > 0 && end_line != line)) {
		return String();
	}
	const PackedStringArray lines = p_source.split("\n");
	if (line > lines.size()) {
		return String();
	}
	const CharString utf8 = lines[line - 1].utf8();
	int64_t end = column - 1;
	if (end >= utf8.length()) {
		return String();
	}
	while (end < utf8.length() && is_identifier_byte(utf8[end])) {
		end++;
	}
	return String::utf8(utf8.get_data() + column - 1, (int)(end - column + 1));
}

// Appends to any diagnostic that named a member the mirror deliberately does not carry (R-SCN-2).
//
// Where the author meets the problem, which is the only place it prevents the failure mode: a
// coverage report in the repository is read by whoever wrote the generator and by nobody else.
void VerseScriptLanguage::explain_skipped_members(const String &p_path, const TypedArray<Dictionary> &p_errors) const {
	// The text the analysis read, which is what the compiler's spans are measured against. A file
	// no analysis has seen has none, and nothing here can be said about it.
	const String source = analyzed_source_by_path.has(p_path) ? String(analyzed_source_by_path[p_path]) : String();
	for (int64_t i = 0; i < p_errors.size(); i++) {
		Dictionary error = p_errors[i];
		if ((int64_t)error.get("code", 0) != UNKNOWN_IDENTIFIER_CODE) {
			continue;
		}
		// The name out of the buffer and the class off the diagnostic, where both used to be split
		// out of the message's backticks. uLang raises the member and the bare-identifier wordings
		// under one code, so an empty subject type is what says which of the two this is.
		const String member = identifier_at_span(source, error);
		if (member.is_empty()) {
			continue;
		}
		const String verse_class = error.get("subject_type", String());
		const verse_api::skipped_member *entry = verse_class.is_empty()
				? unambiguous_skipped_member(member)
				: skipped_member_for(verse_class, member);
		if (entry == nullptr) {
			continue;
		}
		// The class is named only where the diagnostic named one. For an unqualified call it is not
		// known which class was meant, and every class that skips this name skips it alike.
		const String owner = verse_class.is_empty() ? String() : String(entry->godot_class) + ".";
		error["message"] = String(error["message"]) + " Godot has " + owner
				+ String(entry->godot_name) + ", but " + skipped_member_explanation(*entry);
	}
}

void VerseScriptLanguage::note_missing_imports(const String &p_path, const TypedArray<Dictionary> &p_errors) const {
	VerseRuntime *runtime = get_runtime();
	if (runtime == nullptr || !runtime->is_host_loaded()) {
		return;
	}

	const String source = analyzed_source_by_path.has(p_path) ? String(analyzed_source_by_path[p_path]) : String();

	for (int64_t i = 0; i < p_errors.size(); i++) {
		Dictionary error = p_errors[i];
		if (!error.has("code") || (int64_t)error["code"] != UNKNOWN_IDENTIFIER_CODE) {
			continue;
		}
		const String name = identifier_at_span(source, error);
		if (name.is_empty()) {
			continue;
		}

		const PackedStringArray modules = runtime->modules_declaring(name);
		if (modules.is_empty()) {
			continue;
		}

		// Said in the diagnostic whatever happens next, and said first. The insertion below only
		// reaches a file the script editor happens to be showing; the sentence reaches the author
		// wherever they are, and is the whole feature if the buffer turns out to be unreachable.
		String listed;
		for (int64_t m = 0; m < modules.size(); m++) {
			listed += (m == 0 ? String() : String(" or ")) + String("`using { /user@localhost/") + modules[m] + String(" }`");
		}
		error["message"] = String(error["message"]) + String("\nIt is declared in ")
				+ (modules.size() == 1 ? String("a module this file does not import; add ")
									   : String("more than one module, so which was meant is yours to say; add "))
				+ listed + String(" at the top of the file.");

		// One insertion waits for _frame at a time; the next analysis reports whatever is left.
		if (modules.size() != 1 || !pending_import_path.is_empty()) {
			continue;
		}

		const std::string key = std::string(p_path.utf8().get_data()) + "|" + std::string(modules[0].utf8().get_data());
		if (offered_imports.find(key) != offered_imports.end()) {
			continue;
		}
		offered_imports[key] = true;
		pending_import_path = p_path;
		pending_import_module = modules[0];
	}
}

void VerseScriptLanguage::insert_pending_import() const {
	const String path = pending_import_path;
	const String module = pending_import_module;
	pending_import_path = String();
	pending_import_module = String();
	if (path.is_empty()) {
		return;
	}

#ifdef TOOLS_ENABLED
	EditorInterface *editor_interface = verse_editor_interface();
	ScriptEditor *script_editor = editor_interface != nullptr ? editor_interface->get_script_editor() : nullptr;
	if (script_editor == nullptr) {
		return;
	}
	// Only the file on screen. The author moved on if it is not, and writing into a buffer nobody
	// is looking at is how an edit surprises someone later.
	const Ref<Script> current = script_editor->get_current_script();
	if (current.is_null() || current->get_path() != path) {
		return;
	}
	ScriptEditorBase *editor = script_editor->get_current_editor();
	CodeEdit *code = editor != nullptr ? Object::cast_to<CodeEdit>(editor->get_base_editor()) : nullptr;
	if (code == nullptr) {
		return;
	}

	const String line = String("using { /user@localhost/") + module + String(" }");
	if (code->get_text().contains(line)) {
		return;
	}

	// After the last `using` at the top of the file, or at the very top when there is none. The
	// scan stops at the first line that is neither an import, a comment nor blank, so a `using`
	// written further down -- which Verse allows inside a scope -- is not what this appends to.
	int64_t insert_at = 0;
	for (int64_t i = 0; i < code->get_line_count(); i++) {
		const String text = code->get_line(i).strip_edges();
		if (text.begins_with("using")) {
			insert_at = i + 1;
			continue;
		}
		if (text.is_empty() || text.begins_with("#")) {
			continue;
		}
		break;
	}

	// Godot restores the caret itself after a text edit, but not across an inserted line above it.
	const int64_t caret_line = code->get_caret_line();
	const int64_t caret_column = code->get_caret_column();
	code->insert_line_at(insert_at, line);
	if (caret_line >= insert_at) {
		code->set_caret_line(caret_line + 1);
		code->set_caret_column(caret_column);
	}
#endif
}

bool VerseScriptLanguage::record_diagnostics(const Dictionary &p_diagnostics_by_globalized) const {
	PackedStringArray previous = flattened_diagnostics(diagnostics_by_path);
	previous.append_array(flattened_diagnostics(compiler_warnings_by_path));

	diagnostics_by_path.clear();
	compiler_warnings_by_path.clear();

	const Array reported = p_diagnostics_by_globalized.keys();
	for (int64_t i = 0; i < reported.size(); i++) {
		const String globalized = reported[i];
		const String path = path_by_globalized.has(globalized) ? String(path_by_globalized[globalized]) : globalized;
		const TypedArray<Dictionary> filed = p_diagnostics_by_globalized[globalized];

		// The host reports the absolute path it was handed, but the script editor compares an
		// error's path against the *script's* -- `res://scripts/mover.verse` -- and moves every
		// error that does not match into its depended-errors list. Those are listed but never
		// marked: the line highlight and the error bar both read the list this filters.
		//
		// The same dictionaries, not copies: the build logs what it filed after this has run,
		// which is how the log carries the path and the explanations added below.
		TypedArray<Dictionary> errors;
		TypedArray<Dictionary> warnings;
		for (int64_t e = 0; e < filed.size(); e++) {
			Dictionary entry = filed[e];
			entry["path"] = path;
			const int64_t severity = entry["severity"];
			if (severity == VH_SEVERITY_ERROR) {
				errors.push_back(entry);
			} else if (severity == VH_SEVERITY_WARNING) {
				warnings.push_back(entry);
			}
			// An info has no row in the editor; the build's log is where it is read.
		}
		explain_skipped_members(path, errors);
		note_missing_imports(path, errors);
		diagnostics_by_path[path] = errors;
		if (!warnings.is_empty()) {
			compiler_warnings_by_path[path] = warnings;
		}
	}

	PackedStringArray current = flattened_diagnostics(diagnostics_by_path);
	current.append_array(flattened_diagnostics(compiler_warnings_by_path));
	return current != previous;
}

// The build is the one thing that writes a diagnostic to the output log, and it writes every one
// it filed, every time: a build is something the author asked for, and the answer to a second
// build with the same errors is those errors again. An analysis writes nothing -- the script
// editor shows what it found and replaces it on the next validate, which the log cannot do.
void VerseScriptLanguage::log_build_diagnostics(const TypedArray<Dictionary> &p_diagnostics) const {
	for (int64_t i = 0; i < p_diagnostics.size(); i++) {
		const Dictionary entry = p_diagnostics[i];
		const String formatted = formatted_diagnostic(entry);
		const int64_t severity = entry["severity"];
		if (severity == VH_SEVERITY_ERROR) {
			UtilityFunctions::push_error(formatted);
		} else if (severity == VH_SEVERITY_WARNING) {
			UtilityFunctions::push_warning(formatted);
		} else {
			UtilityFunctions::print(formatted);
		}
	}
}

TypedArray<Dictionary> VerseScriptLanguage::diagnostics_for(const String &p_path) const {
	if (!diagnostics_by_path.has(p_path)) {
		return TypedArray<Dictionary>();
	}
	return TypedArray<Dictionary>(diagnostics_by_path[p_path]);
}

TypedArray<Dictionary> VerseScriptLanguage::compiler_warnings_for(const String &p_path) const {
	if (!compiler_warnings_by_path.has(p_path)) {
		return TypedArray<Dictionary>();
	}
	return TypedArray<Dictionary>(compiler_warnings_by_path[p_path]);
}
