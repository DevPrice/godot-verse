#include "verse_syntax_highlighter.h"

#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/editor_settings.hpp>
#include <godot_cpp/classes/text_edit.hpp>
#include <godot_cpp/variant/char_string.hpp>
#include <godot_cpp/variant/variant.hpp>

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
			return keyword_color;
		case VerseTokenKind::Symbol:
			return symbol_color;
		case VerseTokenKind::Function:
			return function_color;
		case VerseTokenKind::Member:
			return member_color;
		case VerseTokenKind::Text:
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

	for (const VerseToken &token : tokens) {
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
		entry["color"] = color_for(token.kind);
		result[column] = entry;
	}
	return result;
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
	member_color = read_color(settings, "text_editor/theme/highlighting/member_variable_color", member_color);
	text_color = read_color(settings, "text_editor/theme/highlighting/text_color", text_color);
}
