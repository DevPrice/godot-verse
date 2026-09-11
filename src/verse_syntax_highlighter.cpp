#include "verse_syntax_highlighter.h"

#include "verse_api_classes.h"
#include "verse_script_language.h"

#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/editor_settings.hpp>
#include <godot_cpp/classes/text_edit.hpp>
#include <godot_cpp/variant/char_string.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/variant.hpp>

#include <iterator>

using namespace godot;

namespace {

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

// The one type /Godot.org/Godot exports that no generated entry stands behind. It is hand-written
// rather than mirrored precisely because Godot's Object is the single class gen_verse_api.py
// skips, so it is absent from the table it belongs in -- and a script names it in its own class
// header, which is the commonest place a type name appears at all. Everything else the package
// exports to a script is either a mirrored class, which the table already carries, or a call,
// which colours from its position. The `variant` tuple is not on either list because it is not
// exported: a script cannot name it.
constexpr const char *native_type_names[] = {
	"object",
};

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

// A field declaration is the one thing a class body has at one tab of indent besides a method:
// `var Speed<public>:...` or the bare `Greeting<public>:...` non-var form. A method's name lexes
// as Function/FunctionDefinition instead of Identifier (it is followed by a parameter list), which
// is what tells the two apart here without re-parsing the type after the name.
void collect_member_name(const CharString &p_utf8, const std::vector<VerseToken> &p_tokens, std::unordered_set<std::string> &r_names) {
	const std::vector<size_t> sig = significant_tokens(p_tokens);
	if (sig.empty() || p_tokens[sig[0]].column != 1) {
		return;
	}
	size_t pos = 0;
	if (p_tokens[sig[0]].kind == VerseTokenKind::Keyword &&
			word_at(p_utf8, p_tokens[sig[0]].column, token_text_end(p_tokens, sig[0], p_utf8.length())) == "var") {
		pos = 1;
	}
	if (pos >= sig.size() || p_tokens[sig[pos]].kind != VerseTokenKind::Identifier) {
		return;
	}
	const size_t name_index = sig[pos];
	const std::string name = word_at(p_utf8, p_tokens[name_index].column, token_text_end(p_tokens, name_index, p_utf8.length()));
	if (!name.empty()) {
		r_names.insert(name);
	}
}

// The same `name := enum{...}` shape verse_scan_class_decl looks for in `name := class(...):`,
// for the one top-level keyword that scanner does not track. Verse's flat project scope means an
// enum declared here is nameable from any other script too, but this only sees the file open in
// this editor -- the type_names set already accepts that trade for script_class_names the same way.
void collect_enum_name(const CharString &p_utf8, const std::vector<VerseToken> &p_tokens, std::unordered_set<std::string> &r_names) {
	const std::vector<size_t> sig = significant_tokens(p_tokens);
	if (sig.size() < 3 || p_tokens[sig[0]].column != 0 || p_tokens[sig[0]].kind != VerseTokenKind::Identifier) {
		return;
	}
	if (p_tokens[sig[1]].kind != VerseTokenKind::Symbol ||
			word_at(p_utf8, p_tokens[sig[1]].column, token_text_end(p_tokens, sig[1], p_utf8.length())) != ":=") {
		return;
	}
	if (p_tokens[sig[2]].kind != VerseTokenKind::Keyword ||
			word_at(p_utf8, p_tokens[sig[2]].column, token_text_end(p_tokens, sig[2], p_utf8.length())) != "enum") {
		return;
	}
	const std::string name = word_at(p_utf8, p_tokens[sig[0]].column, token_text_end(p_tokens, sig[0], p_utf8.length()));
	if (!name.empty()) {
		r_names.insert(name);
	}
}

} // namespace

void VerseSyntaxHighlighter::_bind_methods() {
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

	if (line_start_state.empty()) {
		line_start_state.push_back(VerseLexState());
	}

	// A gap this large would mean re-lexing potentially tens of thousands of lines just to
	// answer one query for a freshly scrolled-to line; restarting from p_line with the default
	// state trades a possibly wrong colouring of that region for not stalling the editor.
	constexpr int MAX_CATCH_UP_LINES = 2000;
	if (p_line - ((int32_t)line_start_state.size() - 1) > MAX_CATCH_UP_LINES) {
		line_start_state.assign((size_t)p_line + 1, VerseLexState());
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
		int column = token.column;
		if (byte_offsets_differ) {
			while (byte_cursor < token.column && byte_cursor < utf8.length()) {
				if ((static_cast<unsigned char>(utf8[byte_cursor]) & 0xC0) != 0x80) {
					char_cursor++;
				}
				byte_cursor++;
			}
			column = char_cursor;
		}

		Dictionary entry;
		entry["color"] = token.kind == VerseTokenKind::Identifier
				? color_for_identifier(utf8, token.column, (int)(i + 1 < tokens.size() ? tokens[i + 1].column : utf8.length()))
				: color_for(token.kind);
		result[column] = entry;
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
	if (type_names.count(word) > 0) {
		return type_color;
	}
	return member_names.count(word) > 0 ? member_color : text_color;
}

void VerseSyntaxHighlighter::_clear_highlighting_cache() {
	line_start_state.assign(1, VerseLexState());
}

void VerseSyntaxHighlighter::_update_cache() {
	line_start_state.assign(1, VerseLexState());

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
	type_color = read_color(settings, "text_editor/theme/highlighting/base_type_color", type_color);

	member_names.clear();
	type_names.clear();
	if (TextEdit *text_edit = get_text_edit()) {
		VerseLexState state;
		std::vector<VerseToken> tokens;
		const int line_count = text_edit->get_line_count();
		for (int i = 0; i < line_count; i++) {
			tokens.clear();
			const CharString utf8 = text_edit->get_line(i).utf8();
			verse_lex_line(utf8.get_data(), state, tokens);
			collect_member_name(utf8, tokens, member_names);
			collect_enum_name(utf8, tokens, type_names);
		}
	}

	for (size_t i = 0; i < std::size(verse_api::classes); i++) {
		type_names.insert(verse_api::classes[i].verse_name);
	}
	for (size_t i = 0; i < std::size(native_type_names); i++) {
		type_names.insert(native_type_names[i]);
	}
	if (VerseScriptLanguage *language = VerseScriptLanguage::singleton()) {
		const PackedStringArray names = language->script_class_names();
		for (int64_t i = 0; i < names.size(); i++) {
			type_names.insert(names[i].utf8().get_data());
		}
	}
}
