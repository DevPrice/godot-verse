#include "verse_syntax_highlighter.h"

#include "verse_api_classes.h"
#include "verse_script.h"
#include "verse_script_language.h"

#include <godot_cpp/classes/class_db_singleton.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/editor_settings.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/text_edit.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/callable.hpp>
#include <godot_cpp/variant/char_string.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/variant.hpp>

#include <cctype>
#include <iterator>
#include <utility>

using namespace godot;

namespace {

VerseRuntime *get_runtime() {
	return Object::cast_to<VerseRuntime>(Engine::get_singleton()->get_singleton("VerseRuntime"));
}

// The Verse class one of Godot's stands for, or null when that class was not mirrored -- which a
// chain walk meets whenever gen_verse_api.py was run with --classes-file.
const char *mirrored_class(const StringName &p_godot_class) {
	for (size_t i = 0; i < std::size(verse_api::classes); i++) {
		if (p_godot_class == StringName(verse_api::classes[i].godot_name)) {
			return verse_api::classes[i].verse_name;
		}
	}
	return nullptr;
}

// Data members only: a method reaches the highlighter as a Function or FunctionDefinition token
// from its parameter list and never as an Identifier, so the mirror's 9597 method names would
// colour nothing and cost a lookup per word. A signal accessor is a data member -- which is why
// `Hidden` cannot be redeclared on a node2d -- so this is properties and signals both.
void collect_data_members(const VerseRuntime &p_runtime, const String &p_class, std::unordered_set<std::string> &r_names) {
	const TypedArray<Dictionary> members = p_runtime.class_members(p_class);
	for (int64_t i = 0; i < members.size(); i++) {
		const Dictionary member = members[i];
		if ((int64_t)member["kind"] == VH_LOOKUP_DATA) {
			r_names.insert(String(member["name"]).utf8().get_data());
		}
	}
}

Color read_color(const Ref<EditorSettings> &p_settings, const String &p_name, const Color &p_fallback) {
	if (p_settings.is_null() || !p_settings->has_setting(p_name)) {
		return p_fallback;
	}
	const Variant value = p_settings->get_setting(p_name);
	if (value.get_type() != Variant::COLOR) {
		return p_fallback;
	}
	return value;
}

// EditorSettings splits a marker list the same way GDScript's highlighter reads it
// (String::split(",", false), which drops empty entries from a trailing or doubled comma).
PackedStringArray read_marker_list(const Ref<EditorSettings> &p_settings, const String &p_name, const char *p_fallback) {
	String value = p_fallback;
	if (!p_settings.is_null() && p_settings->has_setting(p_name)) {
		const Variant setting = p_settings->get_setting(p_name);
		if (setting.get_type() == Variant::STRING) {
			value = setting;
		}
	}
	return value.split(",", false);
}

// The identifier-character rule GDScript's own marker scan uses (is_unicode_identifier_continue)
// narrowed to the ASCII this lexer already classifies identifiers by -- a marker word is matched
// exactly, so a wider or narrower rule here would just mean a run like "TODO2" silently fails to
// match "TODO" instead of the two staying in agreement with the rest of this file's tokens.
bool is_marker_word_char(char p_c) {
	return std::isalnum(static_cast<unsigned char>(p_c)) || p_c == '_';
}

// The type names no generated table stands behind, in two groups.
//
// The three natives the ABI is built out of: gen_verse_api.py leaves them out of the type table
// on purpose, because completion must never offer a script `vh_object` or `godot_ref`. A script
// still *reads* them -- in a diagnostic, in the mirror's own source, in a `_Get` it is writing --
// and a name the editor draws as plain text reads as a name it does not know.
//
// Then Verse's own type names, which the reserved-word list does not carry: `char8` is a reserved
// word and `char` is not, though `char` is what the compiler prints and `string` is `[]char`. The
// same holds for the concurrency vocabulary a signal declaration is written in. Which names those
// are was measured rather than reasoned about -- tests/verse_probe/stdlib_types_probe.verse is the
// file, and it records that `awaitable` and `task` need `using { /Verse.org/Concurrency }` while
// the rest resolve with the Godot package alone.
//
// Everything else the package exports is a mirrored class, a mirrored enum or one of the other
// exported types, and all three tables are read below.
constexpr const char *native_type_names[] = {
	"vh_object",
	"vh_signal",
	"godot_ref",

	"awaitable",
	"cancelable",
	"char",
	"comparable",
	"error",
	"event",
	"listenable",
	"signalable",
	"subscribable",
	"task",
	"weak_map",
};

// The mirror's own classes, split once per process into the two colours they are not all of.
// `verse_api::classes` carries the sixteen math types and `rid` beside the 1036 engine classes
// and ClassDB has heard of none of those (by-hand-findings.md B28) -- which is exactly the line
// GDScript draws between a Variant type and an engine class, so the same test serves here.
const std::unordered_map<std::string, VerseTypeKind> &mirror_type_kinds() {
	static const std::unordered_map<std::string, VerseTypeKind> kinds = []() {
		std::unordered_map<std::string, VerseTypeKind> built;
		ClassDBSingleton *db = ClassDBSingleton::get_singleton();
		for (size_t i = 0; i < std::size(verse_api::classes); i++) {
			const bool engine = db != nullptr && db->class_exists(verse_api::classes[i].godot_name);
			built[verse_api::classes[i].verse_name] = engine ? VerseTypeKind::Engine : VerseTypeKind::Base;
		}
		return built;
	}();
	return kinds;
}

std::string word_at(const CharString &p_utf8, int p_begin, int p_end) {
	if (p_end <= p_begin || p_end > p_utf8.length()) {
		return std::string();
	}
	return std::string(p_utf8.get_data() + p_begin, (size_t)(p_end - p_begin));
}

// Whitespace lexes as its own Text token -- verse_lex_line emits one per run so nested comments
// and strings can resume correctly -- and one sits between every pair of words a declaration scan
// cares about, not just before the first: `var Speed`, `name := enum`. The index this returns is
// always into p_tokens, so a token's own end is still p_tokens[index + 1].column, never
// p_tokens[significant[index + 1]].column -- the run this skipped past would be counted otherwise.
std::vector<size_t> significant_tokens(const std::vector<VerseToken> &p_tokens) {
	std::vector<size_t> indices;
	for (size_t i = 0; i < p_tokens.size(); i++) {
		if (p_tokens[i].kind != VerseTokenKind::Text) {
			indices.push_back(i);
		}
	}
	return indices;
}

int token_text_end(const std::vector<VerseToken> &p_tokens, size_t p_index, int p_utf8_length) {
	return p_index + 1 < p_tokens.size() ? p_tokens[p_index + 1].column : p_utf8_length;
}

// A field declaration is the one thing a class body has at one level of indent besides a method:
// `var Speed<public>:...` or the bare `Greeting<public>:...` non-var form. A method's name lexes
// as Function/FunctionDefinition instead of Identifier (it is followed by a parameter list), which
// is what tells the two apart here without re-parsing the type after the name.
//
// "One level of indent" is not one byte: Verse permits either tabs or spaces (only mixing them is
// forbidden), and most of this repo's own .verse files are space-indented. What one level means is
// therefore a whole-file question -- answered by rebuild_name_caches, which has seen every line --
// so this only reports a candidate's own indent column via r_indent_column and leaves the accept/
// reject decision to the caller.
bool collect_member_name(const CharString &p_utf8, const std::vector<VerseToken> &p_tokens, std::string &r_name, int &r_indent_column) {
	const std::vector<size_t> sig = significant_tokens(p_tokens);
	if (sig.empty()) {
		return false;
	}
	size_t pos = 0;
	if (p_tokens[sig[0]].kind == VerseTokenKind::Keyword &&
			word_at(p_utf8, p_tokens[sig[0]].column, token_text_end(p_tokens, sig[0], p_utf8.length())) == "var") {
		pos = 1;
	}
	if (pos >= sig.size() || p_tokens[sig[pos]].kind != VerseTokenKind::Identifier) {
		return false;
	}
	const size_t name_index = sig[pos];
	const std::string name = word_at(p_utf8, p_tokens[name_index].column, token_text_end(p_tokens, name_index, p_utf8.length()));
	if (name.empty()) {
		return false;
	}
	r_name = name;
	r_indent_column = p_tokens[sig[0]].column;
	return true;
}

// A type the open file declares at top level: `name := class(...)`, and the same shape for
// `struct`, `interface` and `enum`.
//
// `verse_scan_class_decl` tracks the class a file is **named after** and nothing else, because that
// is the only one that can go on a node -- and `script_class_names` is built from it, so the one
// kind of type name nothing coloured was a *second* class in a file. That is ordinary Verse and is
// what a file writes whenever it declares a helper beside its node class:
//
//     test := class(main_script):
//     mover := class(node2d):
//
// Verse's flat project scope means every one of these is nameable from any other script too, but
// this only sees the file open in this editor -- the same trade type_names already accepts.
void collect_type_name(const CharString &p_utf8, const std::vector<VerseToken> &p_tokens,
		std::unordered_map<std::string, VerseTypeKind> &r_names) {
	const std::vector<size_t> sig = significant_tokens(p_tokens);
	if (sig.empty() || p_tokens[sig[0]].column != 0 || p_tokens[sig[0]].kind != VerseTokenKind::Identifier) {
		return;
	}
	// Not necessarily the next token: an access specifier may sit between the name and the `:=`,
	// which `widget<public> := class...` is the ordinary spelling of. Column 0 plus an identifier
	// has already established that this is a top-level definition, so the first `:=` on the line
	// is this definition's.
	size_t assign = 0;
	for (size_t i = 1; i < sig.size(); i++) {
		if (p_tokens[sig[i]].kind == VerseTokenKind::Symbol &&
				word_at(p_utf8, p_tokens[sig[i]].column, token_text_end(p_tokens, sig[i], p_utf8.length())) == ":=") {
			assign = i;
			break;
		}
	}
	if (assign == 0 || assign + 1 >= sig.size() || p_tokens[sig[assign + 1]].kind != VerseTokenKind::Keyword) {
		return;
	}
	const std::string keyword = word_at(p_utf8, p_tokens[sig[assign + 1]].column,
			token_text_end(p_tokens, sig[assign + 1], p_utf8.length()));
	if (keyword != "class" && keyword != "struct" && keyword != "interface" && keyword != "enum") {
		return;
	}
	const std::string name = word_at(p_utf8, p_tokens[sig[0]].column, token_text_end(p_tokens, sig[0], p_utf8.length()));
	if (!name.empty()) {
		r_names[name] = VerseTypeKind::User;
	}
}

} // namespace

void VerseSyntaxHighlighter::_bind_methods() {
	ClassDB::bind_method(D_METHOD("_on_lines_edited_from", "from_line", "to_line"), &VerseSyntaxHighlighter::_on_lines_edited_from);
}

String VerseSyntaxHighlighter::_get_name() const {
	return "Verse";
}

PackedStringArray VerseSyntaxHighlighter::_get_supported_languages() const {
	PackedStringArray languages;
	languages.push_back("Verse");
	return languages;
}

Ref<EditorSyntaxHighlighter> VerseSyntaxHighlighter::_create() const {
	Ref<VerseSyntaxHighlighter> highlighter;
	highlighter.instantiate();
	return highlighter;
}

Color VerseSyntaxHighlighter::color_for(VerseTokenKind p_kind) const {
	switch (p_kind) {
		case VerseTokenKind::Comment:
			return comment_color;
		case VerseTokenKind::String:
			return string_color;
		case VerseTokenKind::Escape:
			return symbol_color;
		case VerseTokenKind::Interpolation:
			return symbol_color;
		case VerseTokenKind::Number:
			return number_color;
		case VerseTokenKind::Keyword:
			return keyword_color;
		case VerseTokenKind::ControlKeyword:
			return control_flow_keyword_color;
		case VerseTokenKind::Attribute:
			return annotation_color;
		case VerseTokenKind::Symbol:
			return symbol_color;
		case VerseTokenKind::Function:
			return function_color;
		case VerseTokenKind::FunctionDefinition:
			return function_definition_color;
		case VerseTokenKind::Member:
			return member_color;
		case VerseTokenKind::Text:
		case VerseTokenKind::Identifier:
		default:
			return text_color;
	}
}

Dictionary VerseSyntaxHighlighter::_get_line_syntax_highlighting(int32_t p_line) const {
	Dictionary result;

	TextEdit *text_edit = get_text_edit();
	if (text_edit == nullptr || p_line < 0) {
		return result;
	}

	if (names_dirty) {
		rebuild_name_caches();
	}

	if (line_start_state.empty()) {
		line_start_state.push_back(VerseLexState());
	}

	// A gap this large would mean re-lexing potentially tens of thousands of lines just to
	// answer one query for a freshly scrolled-to line; restarting from p_line with the default
	// state trades a possibly wrong colouring of that region for not stalling the editor. Grown
	// with resize rather than reassigned, so the states already computed for lines before the gap
	// -- known good -- survive; only the newly reachable indices up to p_line get the default.
	constexpr int MAX_CATCH_UP_LINES = 2000;
	if (p_line - ((int32_t)line_start_state.size() - 1) > MAX_CATCH_UP_LINES) {
		line_start_state.resize((size_t)p_line + 1, VerseLexState());
	}

	while ((int32_t)line_start_state.size() <= p_line) {
		const int32_t line_index = (int32_t)line_start_state.size() - 1;
		VerseLexState state = line_start_state[line_index];
		std::vector<VerseToken> tokens;
		verse_lex_line(text_edit->get_line(line_index).utf8().get_data(), state, tokens);
		line_start_state.push_back(state);
	}

	VerseLexState line_state = line_start_state[p_line];
	const String line = text_edit->get_line(p_line);
	const CharString utf8 = line.utf8();
	std::vector<VerseToken> tokens;
	verse_lex_line(utf8.get_data(), line_state, tokens);

	// The lexer counts bytes and Godot indexes this dictionary by character; the two agree only
	// while the line is ASCII. One non-ASCII character would otherwise shift every colour
	// boundary after it. Tokens come out in increasing column order, so one walk converts them
	// all: the character index of a byte offset is the number of non-continuation bytes before it.
	const bool byte_offsets_differ = utf8.length() != line.length();
	int byte_cursor = 0;
	int char_cursor = 0;

	for (size_t i = 0; i < tokens.size(); i++) {
		const VerseToken &token = tokens[i];
		const int token_end = (int)(i + 1 < tokens.size() ? tokens[i + 1].column : utf8.length());
		const int column = to_char_column(utf8, byte_offsets_differ, token.column, byte_cursor, char_cursor);

		Dictionary entry;
		entry["color"] = token.kind == VerseTokenKind::Identifier
				? color_for_identifier(utf8, token.column, token_end)
				: color_for(token.kind);
		result[column] = entry;

		// A comment token already spans the whole run the lexer coalesced -- delimiters (#, <#,
		// #>) included, which is harmless here since none of them is a marker word character --
		// so the marker scan needs no separate notion of where the comment "really" starts.
		if (token.kind == VerseTokenKind::Comment) {
			highlight_comment_markers(utf8, token.column, token_end, byte_offsets_differ, byte_cursor, char_cursor, result);
		}
	}
	return result;
}

// A run of one kind ends where the next begins, and an identifier is never adjacent to another
// identifier -- there is always a symbol or a space between them, and those are other kinds. So
// the token's own extent falls out of the next token's column without the lexer carrying a length.
godot::Color VerseSyntaxHighlighter::color_for_identifier(const CharString &p_utf8, int p_begin, int p_end) const {
	if (p_end <= p_begin || p_end > p_utf8.length()) {
		return text_color;
	}
	const std::string word(p_utf8.get_data() + p_begin, (size_t)(p_end - p_begin));
	const std::unordered_map<std::string, VerseTypeKind>::const_iterator type = type_names.find(word);
	if (type != type_names.end()) {
		switch (type->second) {
			case VerseTypeKind::Engine:
				return engine_type_color;
			case VerseTypeKind::User:
				return user_type_color;
			default:
				return base_type_color;
		}
	}
	return member_names.count(word) > 0 ? member_color : text_color;
}

int VerseSyntaxHighlighter::to_char_column(const CharString &p_utf8, bool p_byte_offsets_differ, int p_byte_column, int &r_byte_cursor, int &r_char_cursor) {
	if (!p_byte_offsets_differ) {
		return p_byte_column;
	}
	while (r_byte_cursor < p_byte_column && r_byte_cursor < p_utf8.length()) {
		if ((static_cast<unsigned char>(p_utf8[r_byte_cursor]) & 0xC0) != 0x80) {
			r_char_cursor++;
		}
		r_byte_cursor++;
	}
	return r_char_cursor;
}

// Same boundary GDScript's highlighter uses inside its own comment regions: a maximal run of
// identifier characters, looked up whole rather than matched as a substring, so "TODO2" and
// "TODOING" are not "TODO". r_byte_cursor/r_char_cursor are the caller's running conversion --
// threaded through rather than restarted here, since marker positions still have to come out in
// the same increasing byte order the rest of the line's tokens already rely on.
void VerseSyntaxHighlighter::highlight_comment_markers(const CharString &p_utf8, int p_begin, int p_end, bool p_byte_offsets_differ, int &r_byte_cursor, int &r_char_cursor, Dictionary &r_result) const {
	int word_start = -1;
	for (int i = p_begin; i <= p_end; i++) {
		if (i < p_end && is_marker_word_char(p_utf8[i])) {
			if (word_start < 0) {
				word_start = i;
			}
			continue;
		}
		if (word_start < 0) {
			continue;
		}
		const std::string word(p_utf8.get_data() + word_start, (size_t)(i - word_start));
		const std::unordered_map<std::string, CommentMarkerLevel>::const_iterator found = comment_markers.find(word);
		if (found != comment_markers.end()) {
			Dictionary marker_entry;
			marker_entry["color"] = comment_marker_colors[(int)found->second];
			r_result[to_char_column(p_utf8, p_byte_offsets_differ, word_start, r_byte_cursor, r_char_cursor)] = marker_entry;

			Dictionary restore_entry;
			restore_entry["color"] = comment_color;
			r_result[to_char_column(p_utf8, p_byte_offsets_differ, i, r_byte_cursor, r_char_cursor)] = restore_entry;
		}
		word_start = -1;
	}
}

void VerseSyntaxHighlighter::_clear_highlighting_cache() {
	line_start_state.assign(1, VerseLexState());
}

// TextEdit's own "lines_edited_from" signal, not one of ours: SyntaxHighlighter::set_text_edit
// (scene/resources/syntax_highlighter.cpp) already connects it to erase highlighting_cache and
// then calls update_cache(), which is where this connection is (re)established. Comparing by
// instance id, not by pointer, is what makes a reassignment idempotent -- the same TextEdit
// triggers _update_cache on every save and every theme change, and reconnecting each time would
// stack one dead connection per call.
void VerseSyntaxHighlighter::_on_lines_edited_from(int32_t p_from_line, int32_t p_to_line) {
	// Godot's own handler (SyntaxHighlighter::_lines_edited_from) takes the same MIN(...) - 1:
	// the line the edit starts on inherits its start state from the line above, so that line's
	// state -- not the edited line's -- is the last one still known good.
	int32_t edit_start = p_from_line < p_to_line ? p_from_line : p_to_line;
	edit_start -= 1;
	if (edit_start < 0) {
		edit_start = 0;
	}
	if ((int32_t)line_start_state.size() > edit_start + 1) {
		line_start_state.resize((size_t)edit_start + 1);
	}
	names_dirty = true;
}

void VerseSyntaxHighlighter::_update_cache() {
	line_start_state.assign(1, VerseLexState());

	if (TextEdit *text_edit = get_text_edit()) {
		const uint64_t text_edit_id = text_edit->get_instance_id();
		if (text_edit_id != connected_text_edit_id) {
			if (connected_text_edit_id != 0) {
				// The old TextEdit may already be gone -- ObjectDB::get_instance is how Godot's own
				// set_text_edit checks the same thing before touching text_edit_instance_id.
				if (Object *old_object = ObjectDB::get_instance(connected_text_edit_id)) {
					if (TextEdit *old_text_edit = Object::cast_to<TextEdit>(old_object)) {
						old_text_edit->disconnect("lines_edited_from", Callable(this, "_on_lines_edited_from"));
					}
				}
			}
			text_edit->connect("lines_edited_from", Callable(this, "_on_lines_edited_from"));
			connected_text_edit_id = text_edit_id;
		}
	}

	const Ref<EditorSettings> settings = EditorInterface::get_singleton()->get_editor_settings();
	comment_color = read_color(settings, "text_editor/theme/highlighting/comment_color", comment_color);
	string_color = read_color(settings, "text_editor/theme/highlighting/string_color", string_color);
	keyword_color = read_color(settings, "text_editor/theme/highlighting/keyword_color", keyword_color);
	control_flow_keyword_color = read_color(settings, "text_editor/theme/highlighting/control_flow_keyword_color", control_flow_keyword_color);
	number_color = read_color(settings, "text_editor/theme/highlighting/number_color", number_color);
	symbol_color = read_color(settings, "text_editor/theme/highlighting/symbol_color", symbol_color);
	function_color = read_color(settings, "text_editor/theme/highlighting/function_color", function_color);
	// Verse has no theme keys of its own, and matching GDScript is the point: a Verse file open
	// beside a .gd one should not colour the same idea two different ways.
	function_definition_color = read_color(settings, "text_editor/theme/highlighting/gdscript/function_definition_color", function_definition_color);
	annotation_color = read_color(settings, "text_editor/theme/highlighting/gdscript/annotation_color", annotation_color);
	member_color = read_color(settings, "text_editor/theme/highlighting/member_variable_color", member_color);
	text_color = read_color(settings, "text_editor/theme/highlighting/text_color", text_color);
	base_type_color = read_color(settings, "text_editor/theme/highlighting/base_type_color", base_type_color);
	engine_type_color = read_color(settings, "text_editor/theme/highlighting/engine_type_color", engine_type_color);
	user_type_color = read_color(settings, "text_editor/theme/highlighting/user_type_color", user_type_color);

	comment_marker_colors[(int)CommentMarkerLevel::Critical] = read_color(settings, "text_editor/theme/highlighting/comment_markers/critical_color", comment_marker_colors[(int)CommentMarkerLevel::Critical]);
	comment_marker_colors[(int)CommentMarkerLevel::Warning] = read_color(settings, "text_editor/theme/highlighting/comment_markers/warning_color", comment_marker_colors[(int)CommentMarkerLevel::Warning]);
	comment_marker_colors[(int)CommentMarkerLevel::Notice] = read_color(settings, "text_editor/theme/highlighting/comment_markers/notice_color", comment_marker_colors[(int)CommentMarkerLevel::Notice]);

	comment_markers.clear();
	const PackedStringArray critical_list = read_marker_list(settings, "text_editor/theme/highlighting/comment_markers/critical_list", "ALERT,ATTENTION,CAUTION,CRITICAL,DANGER,SECURITY");
	for (int64_t i = 0; i < critical_list.size(); i++) {
		comment_markers[critical_list[i].utf8().get_data()] = CommentMarkerLevel::Critical;
	}
	const PackedStringArray warning_list = read_marker_list(settings, "text_editor/theme/highlighting/comment_markers/warning_list", "BUG,DEPRECATED,FIXME,HACK,TASK,TBD,TODO,WARNING");
	for (int64_t i = 0; i < warning_list.size(); i++) {
		comment_markers[warning_list[i].utf8().get_data()] = CommentMarkerLevel::Warning;
	}
	const PackedStringArray notice_list = read_marker_list(settings, "text_editor/theme/highlighting/comment_markers/notice_list", "INFO,NOTE,NOTICE,TEST,TESTING");
	for (int64_t i = 0; i < notice_list.size(); i++) {
		comment_markers[notice_list[i].utf8().get_data()] = CommentMarkerLevel::Notice;
	}

	rebuild_name_caches();
}

// The member set as the last analysis described it, or false when there is none to read.
//
// The scan below is a heuristic about indentation, which is all a file that has never been built
// leaves to read; once a build has happened the program itself says what the class declares. The
// half no scan of one file could reach is the inherited one: `Position` and `Name` are members of
// the script exactly the way its own `Speed` is, and colouring one and not the other is the
// difference the author sees.
//
// As stale as every other snapshot read -- it describes the last analysis, and Godot refreshes a
// highlighter's cache after each successful validation, which is the same hook GDScript rebuilds
// its own member set on.
bool VerseSyntaxHighlighter::collect_analysed_members() const {
	VerseScriptLanguage *language = VerseScriptLanguage::singleton();
	VerseRuntime *runtime = get_runtime();
	if (language == nullptr || runtime == nullptr || !runtime->is_host_loaded()) {
		return false;
	}

	// EditorSyntaxHighlighter::_get_edited_resource is bound in ClassDB but absent from
	// extension_api.json, so godot-cpp generated no wrapper for it and a dynamic call is the only
	// way in. It is also the only way to the script at all: SyntaxHighlighter hands a highlighter
	// its TextEdit and nothing else, and a TextEdit does not know what it is editing.
	VerseSyntaxHighlighter *self = const_cast<VerseSyntaxHighlighter *>(this);
	if (!self->has_method("_get_edited_resource")) {
		return false;
	}
	Object *edited = self->call("_get_edited_resource");
	VerseScript *script = Object::cast_to<VerseScript>(edited);
	if (script == nullptr || script->get_path().is_empty()) {
		return false;
	}

	// A class the analysis has never seen answers an empty member list, which is indistinguishable
	// from a class that declares nothing -- and falling back is right for the first and wrong for
	// the second, so the question is asked separately.
	const String own_class = language->qualified_class_name(script->get_path());
	if (!runtime->has_class(own_class)) {
		return false;
	}
	collect_data_members(*runtime, own_class, member_names);

	// A base that is another script declares members of its own, and the chain of base scripts is
	// the only place they are written down: _get_instance_base_type walks past every one of them to
	// the Godot class underneath.
	Ref<Script> base = script->_get_base_script();
	for (int depth = 0; depth < 32 && base.is_valid(); depth++) {
		VerseScript *verse_base = Object::cast_to<VerseScript>(base.ptr());
		if (verse_base == nullptr || verse_base->get_path().is_empty()) {
			break;
		}
		collect_data_members(*runtime, language->qualified_class_name(verse_base->get_path()), member_names);
		base = verse_base->_get_base_script();
	}

	// The rest of the chain is Godot's, because every Verse class in it is the mirror of one, and
	// ClassDB is where that hierarchy is written down -- the mirror's own bases are not something
	// the ABI answers for.
	for (StringName godot_class = script->_get_instance_base_type(); !String(godot_class).is_empty();
			godot_class = ClassDB::get_parent_class(godot_class)) {
		if (const char *verse_class = mirrored_class(godot_class)) {
			collect_data_members(*runtime, String(verse_class), member_names);
		}
	}
	return true;
}

void VerseSyntaxHighlighter::rebuild_name_caches() const {
	member_names.clear();
	type_names.clear();
	const bool analysed = collect_analysed_members();
	if (TextEdit *text_edit = get_text_edit()) {
		VerseLexState state;
		std::vector<VerseToken> tokens;
		const int line_count = text_edit->get_line_count();

		// Verse forbids mixing tabs and spaces within one file, so every code line's leading run
		// is one indent character repeated; the smallest nonzero width among lines that open with
		// code rather than a comment is what "one level under the class header" means here -- one
		// tab in Godot's own script editor, or however many spaces this project's own files use
		// (tests/integration/scripts, tests/host_smoke, tests/coverage_diagnostic, host/Verse/*).
		// A comment-first line is excluded because a comment can be indented to whatever column a
		// human found readable, which says nothing about the file's structural indent step.
		int indent_unit = 0;
		std::vector<std::pair<int, std::string>> member_candidates;

		for (int i = 0; i < line_count; i++) {
			tokens.clear();
			const CharString utf8 = text_edit->get_line(i).utf8();
			verse_lex_line(utf8.get_data(), state, tokens);

			const std::vector<size_t> sig = significant_tokens(tokens);
			if (!sig.empty() && tokens[sig[0]].column > 0 && tokens[sig[0]].kind != VerseTokenKind::Comment) {
				if (indent_unit == 0 || tokens[sig[0]].column < indent_unit) {
					indent_unit = tokens[sig[0]].column;
				}
			}

			std::string member_name;
			int member_column = 0;
			if (!analysed && collect_member_name(utf8, tokens, member_name, member_column)) {
				member_candidates.emplace_back(member_column, std::move(member_name));
			}
			collect_type_name(utf8, tokens, type_names);
		}

		// indent_unit == 0 means the file has no indented code at all (a library file of only
		// top-level definitions), in which case nothing is a class member -- never fall back to
		// accepting column 0, which is where a top-level `name := ...` sits.
		if (!analysed && indent_unit > 0) {
			for (const std::pair<int, std::string> &candidate : member_candidates) {
				if (candidate.first == indent_unit) {
					member_names.insert(candidate.second);
				}
			}
		}
	}

	const std::unordered_map<std::string, VerseTypeKind> &mirror = mirror_type_kinds();
	type_names.insert(mirror.begin(), mirror.end());
	// The 793 mirrored enums are types a script writes as often as it writes a class -- every
	// `node_internal_mode` in a declaration -- and the editor already sends a hover on one to
	// Godot's own documentation, so drawing it as plain text was the one surface that disagreed.
	for (size_t i = 0; i < std::size(verse_api::enums); i++) {
		type_names[verse_api::enums[i].verse_enum] = VerseTypeKind::Engine;
	}
	// `variant`, the containers, `callable` and the two signal types: the package's exported types
	// that stand for no Godot class, so the table above cannot carry them. The sixteen value types
	// and `rid` are in that one, and three of them used to be -- `vector2` coloured and `vector2i`
	// beside it did not.
	for (size_t i = 0; i < std::size(verse_api::types); i++) {
		type_names[verse_api::types[i].verse_name] = VerseTypeKind::Base;
	}
	for (size_t i = 0; i < std::size(native_type_names); i++) {
		type_names[native_type_names[i]] = VerseTypeKind::Base;
	}
	if (VerseScriptLanguage *language = VerseScriptLanguage::singleton()) {
		// The cached list, shared with completion: this used to walk the whole of res:// with
		// DirAccess every time the theme changed or a highlighter was reassigned.
		const PackedStringArray &names = language->script_class_names();
		for (int64_t i = 0; i < names.size(); i++) {
			type_names[names[i].utf8().get_data()] = VerseTypeKind::User;
		}
		// R-INT-7's generated bindings, on whichever side of the same line they belong: a class a
		// GDExtension registered is an engine type, and one a script declares with `class_name` is
		// the project's own. Neither was in any of the tables above, so both drew as plain text --
		// which is how a name the editor does not know reads.
		for (const KeyValue<String, VerseScriptLanguage::BindingInfo> &binding : language->bindings()) {
			type_names[binding.key.utf8().get_data()] = binding.value.script_class.is_empty()
					? VerseTypeKind::Engine
					: VerseTypeKind::User;
		}
	}
	names_dirty = false;
}
