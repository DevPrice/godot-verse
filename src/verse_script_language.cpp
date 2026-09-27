#include "verse_script_language.h"

#include "verse_api_classes.h"
#include "verse_api_lookup.h"
#include "verse_bindings_gen.h"
#include "verse_api_skipped.h"
#include "verse_completion.h"
#include "verse_diagnostic_prose.h"
#include "verse_hover.h"
#include "verse_class_decl.h"
#include "verse_doc_markup.h"
#include "verse_keywords.h"
#include "verse_module_map.h"
#include "verse_resource_format.h"
#include "verse_runtime.h"
#include "verse_script.h"

#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/memory.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#ifdef TOOLS_ENABLED
#include <godot_cpp/classes/code_edit.hpp>
#include <godot_cpp/classes/editor_file_system.hpp>
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

// The comment block immediately above p_line, as prose. The rules are verse_doc_markup's, where
// they are unit-tested; this is the godot::String face of them.
//
// Read out of the source rather than asked of the compiler. The parser does keep comments, but it
// hangs one off whichever node begins the construct, and for a member behind four lines of
// `@editable` and friends that is the attribute clause rather than the member -- so recovering
// the association costs more Vst archaeology than re-reading the text. The host's DocOf reads
// those nodes by the same rules, as the fallback for a file this side cannot open.
String verse_doc_comment_above(const String &p_source, int64_t p_line) {
	const CharString utf8 = p_source.utf8();
	const std::string source(utf8.get_data(), (size_t)utf8.length());
	return String::utf8(verse_doc_comment_above(source, (int)p_line).c_str());
}

String verse_doc_bbcode(const String &p_doc) {
	return String::utf8(verse_doc_to_bbcode(p_doc.utf8().get_data()).c_str());
}

#ifdef TOOLS_ENABLED
EditorInterface *verse_editor_interface() {
	return Engine::get_singleton()->is_editor_hint() ? EditorInterface::get_singleton() : nullptr;
}
#endif

namespace {

VerseRuntime *get_runtime() {
	return Object::cast_to<VerseRuntime>(Engine::get_singleton()->get_singleton("VerseRuntime"));
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
	ClassDB::bind_method(D_METHOD("on_script_classes_updated"), &VerseScriptLanguage::on_script_classes_updated);

	// The hover harness' one seam. `_lookup_code` is a virtual, and a virtual is metadata in
	// ClassDB rather than a MethodBind (class_db.cpp's add_virtual_method fills virtual_methods
	// and nothing else), so no script can call it however it reaches the language object. This is
	// the method that can be called, and it exists because every tooltip the script editor draws
	// is otherwise testable only by hand.
	ClassDB::bind_method(D_METHOD("probe_hover", "path"), &VerseScriptLanguage::probe_hover);
	ClassDB::bind_method(D_METHOD("probe_complete", "path", "positions"),
		&VerseScriptLanguage::probe_complete);
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

// R-INT-10. One sentence, written twice: as an error the editor draws at the class's own line,
// and as a warning `log_script_warnings` pushes so a headless run can assert it. That is the rule
// `inert_global_class_message` below is the other worked example of -- a `_validate` error reaches
// the gutter and no log, so a diagnostic that has to be both seen and tested needs two reporters.
static String script_binding_base_message(const String &p_class_name, const String &p_base) {
	return String("`") + p_class_name + String("` extends `") + p_base
			+ String("`, which is the generated binding for a class a *script* declares. That cannot work: ")
			+ String("Godot gives an object exactly one script instance, so the inherited methods would ")
			+ String("forward to a `") + p_base + String("` that is not there -- `") + p_class_name
			+ String("`'s own script is the only one the node has (R-INT-6, R-INT-10). Extend the ")
			+ String("binding's own Godot base instead and hold the other node, or move the shared code ")
			+ String("into Verse.");
}

static Dictionary script_binding_base_error(const String &p_message, int64_t p_line) {
	Dictionary error;
	error["line"] = p_line;
	error["column"] = 1;
	error["message"] = p_message;
	return error;
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
	const Array other_paths = project_state.all_diagnostics().keys();
	for (int64_t i = 0; i < other_paths.size(); i++) {
		const String other_path = other_paths[i];
		if (other_path == p_path) {
			continue;
		}
		const TypedArray<Dictionary> filed = TypedArray<Dictionary>(project_state.all_diagnostics()[other_path]);
		if (filed.is_empty()) {
			continue;
		}
		broken_elsewhere.push_back(other_path);
		// Fitted against that file's own last-analyzed source, never p_script's -- a depended error
		// is only listed and clicked through here, but the column still has to sit inside a real
		// line before the editor displays it (see diagnostics_fitted_to above). No entry means no
		// analysis has read that file yet; pass its diagnostics through rather than fit them to the
		// wrong buffer.
		if (project_state.analyzed_sources().has(other_path)) {
			elsewhere.append_array(diagnostics_fitted_to(filed, String(project_state.analyzed_sources()[other_path])));
		} else {
			elsewhere.append_array(filed);
		}
	}

	// R-INT-10, read off the buffer rather than the last analysis so it appears and clears as the
	// base is typed. The compiler cannot diagnose it: `player := class(mob)` is a perfectly ordinary
	// Verse subclass and every method on it resolves. What makes it wrong is Godot's one-script-per-
	// object rule, which no part of Verse knows about.
	{
		const VerseClassDecl base_decl = verse_scan_class_decl(p_script.utf8().get_data(),
				p_path.get_file().get_basename().utf8().get_data());
		if (!base_decl.base.empty() && is_script_binding(String(base_decl.base.c_str()))) {
			errors.push_back(script_binding_base_error(
					script_binding_base_message(String(base_decl.name.c_str()), String(base_decl.base.c_str())),
					// The scanner counts rows from zero; Godot's error lines start at one.
					base_decl.line + 1));
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
	// The same reverse lookup member_bearing_chain and base_types_for use below, over
	// verse_api::classes; see verse_api_lookup.h.
	if (const char *mirrored = mirrored_class(p_godot_type)) {
		return String(mirrored);
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

// Completion is answered by the compiler wherever it can be: verse_completion.cpp is the whole
// body, kept there rather than here for the same reason the debugger's and the profiler's are.
Dictionary VerseScriptLanguage::_complete_code(const String &p_code, const String &p_path, Object *p_owner) const {
	return completion.complete_code(p_code, p_path, p_owner);
}

// Ctrl+click, the ctrl-hover underline and the documentation tooltip are all this one call.
// verse_hover.cpp is the whole body, kept there for the same reason completion's is.
Dictionary VerseScriptLanguage::_lookup_code(const String &p_code, const String &p_symbol, const String &p_path, Object *p_owner) const {
	return hover.lookup_code(p_code, p_symbol, p_path, p_owner);
}

// tools/probe_hover.py's seam; VerseHover::probe is the whole body, kept there with
// _lookup_code's rather than here, since ClassDB only needs the pointer-to-member.
TypedArray<Dictionary> VerseScriptLanguage::probe_hover(const String &p_path) {
	return hover.probe(p_path);
}

// tools/probe_complete.py's seam; VerseCompletion::probe is the whole body, kept there with
// _complete_code's rather than here, since ClassDB only needs the pointer-to-member.
TypedArray<Dictionary> VerseScriptLanguage::probe_complete(
		const String &p_path, const PackedInt32Array &p_positions) {
	return completion.probe(p_path, p_positions);
}

// Every _debug_* virtual is a one-line delegation to VerseDebugger, which owns the state and the
// behaviour: godot-cpp binds a virtual with Godot only when the subclass declares it, so the
// declarations have to stay here even though the bodies do not.
String VerseScriptLanguage::_debug_get_error() const {
	return debugger.get_error();
}

int32_t VerseScriptLanguage::_debug_get_stack_level_count() const {
	return debugger.get_stack_level_count();
}

int32_t VerseScriptLanguage::_debug_get_stack_level_line(int32_t p_level) const {
	return debugger.get_stack_level_line(p_level);
}

String VerseScriptLanguage::_debug_get_stack_level_function(int32_t p_level) const {
	return debugger.get_stack_level_function(p_level);
}

String VerseScriptLanguage::_debug_get_stack_level_source(int32_t p_level) const {
	return debugger.get_stack_level_source(p_level);
}

Dictionary VerseScriptLanguage::_debug_get_stack_level_locals(int32_t p_level, int32_t p_max_subitems, int32_t p_max_depth) {
	return debugger.get_stack_level_locals(p_level, p_max_subitems, p_max_depth);
}

Dictionary VerseScriptLanguage::_debug_get_stack_level_members(int32_t p_level, int32_t p_max_subitems, int32_t p_max_depth) {
	return debugger.get_stack_level_members(p_level, p_max_subitems, p_max_depth);
}

void *VerseScriptLanguage::_debug_get_stack_level_instance(int32_t p_level) {
	return debugger.get_stack_level_instance(p_level);
}

Dictionary VerseScriptLanguage::_debug_get_globals(int32_t p_max_subitems, int32_t p_max_depth) {
	return debugger.get_globals(p_max_subitems, p_max_depth);
}

TypedArray<Dictionary> VerseScriptLanguage::_debug_get_current_stack_info() {
	return debugger.get_current_stack_info();
}

// Every `_profiling_*` virtual is a one-line delegation to VerseProfiler, for the same reason the
// debugger's are: godot-cpp binds a virtual only when the subclass declares it.
void VerseScriptLanguage::_profiling_start() {
	profiler.start();
}

void VerseScriptLanguage::_profiling_stop() {
	profiler.stop();
}

void VerseScriptLanguage::_profiling_set_save_native_calls(bool p_enable) {
	profiler.set_save_native_calls(p_enable);
}

int32_t VerseScriptLanguage::_profiling_get_accumulated_data(ScriptLanguageExtensionProfilingInfo *p_info_array, int32_t p_info_max) {
	return profiler.get_accumulated_data(p_info_array, p_info_max);
}

int32_t VerseScriptLanguage::_profiling_get_frame_data(ScriptLanguageExtensionProfilingInfo *p_info_array, int32_t p_info_max) {
	return profiler.get_frame_data(p_info_array, p_info_max);
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
		project_state.poll_check();

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

		// The roster hook, beside it and for the same reason -- there is no EditorFileSystem to
		// connect to until the editor's singletons exist. `script_classes_updated` is
		// language-agnostic, so a C# `class_name` re-arms this with no code of its own; OQ-17 is
		// still that nothing here has ever run C#.
		if (!bindings_hook_connected) {
			EditorInterface *editor_interface = verse_editor_interface();
			EditorFileSystem *filesystem = editor_interface != nullptr ? editor_interface->get_resource_filesystem() : nullptr;
			if (filesystem != nullptr) {
				filesystem->connect("script_classes_updated", Callable(this, "on_script_classes_updated"));
				bindings_hook_connected = true;
			}
		}

		// Regenerated on the frame after the roster moved rather than inside the signal, because
		// the generator loads every `class_name` script and the signal is emitted from the middle
		// of the editor's own scan.
		// An incomplete roster re-arms this once: a class whose script could not be described is
		// asked about again on the next frame, by which time the build has usually made it
		// loadable. It clears itself when a generation describes everything.
		if ((bindings_refresh_pending || project_state.binding_roster_incomplete()) && refresh_bindings()) {
			bindings_refresh_pending = false;
		}

		// Directly after that refresh, which is what makes this build's roster the complete one.
		// The build being corrected was made from inside a resource load with scripts held back, so
		// it described their bindings as bare types and refused every Verse file that called one
		// (B30); its verdict was withheld rather than logged, and this is the build that produces a
		// real one. Fired whether or not the roster actually completed: a script that can never be
		// described would otherwise leave the withheld verdict unreported for the session.
		if (project_state.take_corrective_build()) {
			build_project();
		}

		// The first ask, and every one a change to the program has re-armed. The poll above is
		// what clears docs_refresh_attempted, so this costs one republish per analysis rather
		// than one per frame for a class that still cannot be described.
		if (project_state.is_built() && !docs_refresh_attempted && script_docs_deferred.exchange(false)) {
			docs_refresh_attempted = true;
			docs_refresh_pending = true;
		}

		// Before the error list, because republishing a doc is what the list's own script needs
		// to have happened already. Same reason as below for not doing it inside the poll.
		if (docs_refresh_pending) {
			docs_refresh_pending = false;
			republish_script_docs();
		}

		// Deliberately here rather than in poll_check: re-entering the script editor from inside
		// the poll would rebuild its error list while it is drawing a popup.
		if (editor_refresh_pending) {
			editor_refresh_pending = false;
			refresh_current_script_editor();
		}

		// After the error list and before the import, for the same reason: this asks the editor to
		// run _complete_code again, and that has to see the buffer everything else has settled on.
		if (project_state.take_completion_refresh()) {
			completion.refresh_if_current();
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
		project_state.start_pending_check();
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

		// R-INT-10's second reporter. The error itself goes to `_validate`, which reaches the
		// gutter and no log -- so nothing headless could assert it without this line.
		if (!decl.base.empty() && is_script_binding(String(decl.base.c_str()))) {
			UtilityFunctions::push_error(path + String(": ")
					+ script_binding_base_message(String(decl.name.c_str()), String(decl.base.c_str())));
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
			script_class_names_cache.push_back(qualified_class_name(sources[i]));
		}
		script_class_names_built = true;
	}
	return script_class_names_cache;
}

void VerseScriptLanguage::invalidate_script_class_names() const {
	script_class_names_built = false;
}

void VerseScriptLanguage::on_script_classes_updated() {
	bindings_refresh_pending = true;
}

const VerseScriptLanguage::BindingInfo *VerseScriptLanguage::binding_for(const String &p_verse_class) const {
	const HashMap<String, BindingInfo>::ConstIterator found = bindings_by_verse_class.find(p_verse_class);
	return found != bindings_by_verse_class.end() ? &found->value : nullptr;
}

bool VerseScriptLanguage::is_script_binding(const String &p_verse_class) const {
	const BindingInfo *binding = binding_for(p_verse_class);
	return binding != nullptr && !binding->script_class.is_empty();
}

void VerseScriptLanguage::warn_incomplete_roster() const {
	String named;
	for (const std::string &verse_class : project_state.incomplete_binding_classes()) {
		const String name = String(verse_class.c_str());
		const BindingInfo *info = binding_for(name);
		named += named.is_empty() ? String("`") : String(", `");
		named += name + String("`");
		if (info != nullptr && !info->script_path.is_empty()) {
			named += String(" (") + info->script_path + String(")");
		}
	}
	UtilityFunctions::push_warning(String(
			"Verse: this build ran against an incomplete binding roster, so its errors are withheld "
			"for one pass. ") + named + String(" is declared with no members, because the script it "
			"stands for names a Verse class -- loading it to describe it while a .verse is loading "
			"would be a cyclic load, so it is held back instead. A Verse file that names one of its "
			"*methods* does not compile on this pass, and a node that loses its script over it is "
			"reported by GDScript as a null value with nothing said about Verse. The next build "
			"describes it in full; what no build can repair is a node the scene has already "
			"finished instantiating. Reach the method through `Call`/`Callv` (R-INT-2) rather than "
			"naming it, and the cycle is broken."));
}

bool VerseScriptLanguage::refresh_bindings() {
#ifdef TOOLS_ENABLED
	VerseRuntime *runtime = get_runtime();
	if (runtime == nullptr || !runtime->is_host_loaded() || !runtime->host_has_compiler()) {
		// The host loads lazily, so the first frames of a session have nothing to hand a package
		// to. Answering false keeps the request armed rather than dropping it.
		return false;
	}

	// The generator loads every `class_name` script, and a GDScript naming a Verse class reaches
	// this from inside that script's own load, where asking for it again is a cyclic load. Godot
	// refuses one silently, so the only thing said is the asking side's `Error loading resource` --
	// about the wrong script (B30). On that stack the generator holds back the scripts that can
	// close the loop and no others: each is still declared from the class list, and
	// an incomplete roster asks again on the next frame for its members.
	const VerseBindings bindings = verse_generate_bindings(
			VerseResourceFormatLoader::is_loading(), &last_binding_classes);
	runtime->set_bindings(bindings);
	last_binding_classes = bindings.classes;

	// A class emitted as a bare type because its script would not load. That is the ordinary state
	// of a GDScript naming a Verse class *before the first build*, so it is not worth a warning --
	// what it is worth is asking again, because the build this generation feeds is exactly what
	// makes the script loadable. The next ask fills the members in.
	project_state.set_incomplete_binding_classes(bindings.incomplete);

	bindings_by_verse_class.clear();
	for (const VerseBindingClass &binding : bindings.classes) {
		BindingInfo info;
		info.godot_class = String(binding.godot_class.c_str());
		info.script_class = String(binding.script_class.c_str());
		info.script_path = String(binding.script_path.c_str());
		// The same function the emitter names the member with, rather than a field beside it:
		// two spellings of one name are two things that can disagree.
		for (const VerseBindingMethod &method : binding.methods) {
			info.methods.insert(String(verse_binding_member_name(method.godot_name).c_str()),
					String(method.godot_name.c_str()));
		}
		for (const VerseBindingSignal &signal : binding.signals) {
			info.signals.insert(String(verse_binding_member_name(signal.godot_name).c_str()),
					String(signal.godot_name.c_str()));
		}
		bindings_by_verse_class.insert(String(binding.verse_class.c_str()), info);
	}

	// Written as well as handed over, because a generated file nobody can read is a generated file
	// nobody can debug: the Verse the host compiled is exactly these bytes, so a diagnostic against
	// `GodotBindings.verse` has somewhere to point. Under `.godot/`, which is never committed and
	// which `find_project_files` already skips along with every other dot-directory -- so it is not
	// project source and is never compiled twice.
	const String generated = "res://.godot/verse/bindings.verse";
	if (bindings.source.empty()) {
		if (FileAccess::file_exists(generated)) {
			DirAccess::remove_absolute(generated);
		}
		return true;
	}
	if (DirAccess::make_dir_recursive_absolute("res://.godot/verse") == OK) {
		Ref<FileAccess> file = FileAccess::open(generated, FileAccess::WRITE);
		if (file.is_valid()) {
			file->store_string(String(bindings.source.c_str()));
		}
	}
	return true;
#else
	return true;
#endif
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

// Every class name a script can write, for the completion list Godot asks for when the host has
// offered nothing: the mirrored classes, and the package's other exported types beside them.
// `variant` and `signal` are as nameable as `node2d` is -- a script writes them in `_Get`'s
// signature and in every declared signal -- and were offered by nothing.
const PackedStringArray &VerseScriptLanguage::mirrored_class_names() {
	static PackedStringArray names = []() {
		PackedStringArray built;
		built.resize((int64_t)(std::size(verse_api::classes) + std::size(verse_api::types)));
		for (size_t i = 0; i < std::size(verse_api::classes); i++) {
			built.set((int64_t)i, String(verse_api::classes[i].verse_name));
		}
		for (size_t i = 0; i < std::size(verse_api::types); i++) {
			built.set((int64_t)(std::size(verse_api::classes) + i), String(verse_api::types[i].verse_name));
		}
		return built;
	}();
	return names;
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

void VerseScriptLanguage::note_script_docs_deferred() const {
	script_docs_deferred.store(true);
}

// Every loaded script's documentation, re-registered now that the host can describe a class.
//
// Godot builds a project's script documentation exactly once per session and does it on a loader
// thread of its own -- EditorHelp::_regen_script_doc_thread, which loads every script and asks it
// to describe itself -- and after that republishes a script's only when it is *saved*. That one
// pass runs before the first analysis has published a snapshot, so every class described itself as
// having no members at all, and a hover on `Speed` found the class' documentation with no row for
// it: the label, the name, and an empty box where the comment above the declaration should be.
//
// It used to work because vh_class_members waited: ~22 ABI reads began with a join on the analysis
// thread, and this pass was one of the callers that paid the 1.7 s and got real members back
// (commit dcd517e, which took the waits out and is what this replaces them with).
//
// Not limited to the open script the way refresh_current_script_editor is. The documentation of a
// class is read by a hover in *any* file, and nothing else will ask for it again.
void VerseScriptLanguage::republish_script_docs() const {
#ifdef TOOLS_ENABLED
	EditorInterface *editor_interface = verse_editor_interface();
	ScriptEditor *script_editor = editor_interface != nullptr ? editor_interface->get_script_editor() : nullptr;
	if (script_editor == nullptr) {
		return;
	}

	// Snapshotted: describing a script reaches _get_documentation, and Godot is free to drop one
	// while that runs.
	const std::vector<VerseScript *> scripts = live_scripts;
	for (VerseScript *script : scripts) {
		const Ref<Script> ref = Ref<Script>(script);
		if (ref.is_null() || ref->get_path().is_empty()) {
			continue;
		}
		script_editor->clear_docs_from_script(ref);
		script_editor->update_docs_from_script(ref);
	}
#endif
}

// The republish above cannot be relied on to have happened by the time a tooltip needs the doc,
// and B38 is the record of how it was missed. It runs once per re-arm over the scripts alive at
// that moment; EditorHelp queues a doc added before its own regeneration has finished and
// *discards* the queue when that regeneration starts (editor_help.cpp, _regen_script_doc_thread);
// and a script the regeneration loaded and dropped is in nobody's list when the pass runs. A
// hover that names the class is the one moment the doc is certainly wanted, and the script knows
// whether what Godot holds was described from the text the host now describes.
void VerseScriptLanguage::ensure_script_doc_published(const String &p_class_name) const {
#ifdef TOOLS_ENABLED
	EditorInterface *editor_interface = verse_editor_interface();
	ScriptEditor *script_editor = editor_interface != nullptr ? editor_interface->get_script_editor() : nullptr;
	if (script_editor == nullptr) {
		return;
	}

	const std::vector<VerseScript *> scripts = live_scripts;
	for (VerseScript *script : scripts) {
		if (script->verse_class_name() != p_class_name) {
			continue;
		}
		if (script->doc_is_current()) {
			return;
		}
		const Ref<Script> ref = Ref<Script>(script);
		if (ref.is_null() || ref->get_path().is_empty()) {
			return;
		}
		script_editor->clear_docs_from_script(ref);
		script_editor->update_docs_from_script(ref);
		return;
	}
#endif
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
		warning["message"] = export_rejection_message((vh_export_reject)reject, (vh_export_hint)(int64_t)entry["hint"],
				entry["name"], entry["hint_string"], entry["native_class"]);
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
		warning["message"] = signal_rejection_message(String(signal.name), signal.reject, signal.reject_detail);
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
		warning["message"] = rpc_rejection_message(String(rpc.name), rpc.reject, rpc.reject_detail);
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
	// The mirror emits the method under another name, so the author who typed Godot's is one word
	// from working code rather than looking at a gap. Only the *lost* spelling has a row: the name
	// the rename took resolves, so a row keyed on it could answer nothing but itself.
	if (reason == "method_renamed") {
		return String("it is reachable as ") + detail + ".";
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
	const char *godot_name = godot_classdb_class_for(p_verse_class);
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
	const String source = project_state.analyzed_sources().has(p_path) ? String(project_state.analyzed_sources()[p_path]) : String();
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

	const String source = project_state.analyzed_sources().has(p_path) ? String(project_state.analyzed_sources()[p_path]) : String();

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

// The build is the one thing that writes a diagnostic to the output log, and it writes every one
// it filed, every time: a build is something the author asked for, and the answer to a second
// build with the same errors is those errors again. An analysis writes nothing -- the script
// editor shows what it found and replaces it on the next validate, which the log cannot do.
void VerseScriptLanguage::log_build_diagnostics(const TypedArray<Dictionary> &p_diagnostics) const {
	for (int64_t i = 0; i < p_diagnostics.size(); i++) {
		const Dictionary entry = p_diagnostics[i];
		const String formatted = verse_formatted_diagnostic(entry);
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
