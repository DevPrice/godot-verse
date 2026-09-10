#pragma once

#include "verse_lexer.h"

#include <godot_cpp/classes/editor_syntax_highlighter.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>

#include <vector>

// Colours Verse source through the lexer in verse_lexer.h/.cpp, which is what makes nested
// comments and comments-inside-strings come out right where Godot's stock CodeHighlighter
// (fed from VerseScriptLanguage's delimiter lists) cannot express them.
class VerseSyntaxHighlighter : public godot::EditorSyntaxHighlighter {
	GDCLASS(VerseSyntaxHighlighter, godot::EditorSyntaxHighlighter)

protected:
	static void _bind_methods();

public:
	godot::String _get_name() const override;
	godot::PackedStringArray _get_supported_languages() const override;
	godot::Ref<godot::EditorSyntaxHighlighter> _create() const override;

	godot::Dictionary _get_line_syntax_highlighting(int32_t p_line) const override;
	void _clear_highlighting_cache() override;
	void _update_cache() override;

private:
	godot::Color color_for(VerseTokenKind p_kind) const;

	// line_start_state[i] is the lexer state line i begins in; index 0 is always the default
	// state. _get_line_syntax_highlighting is const (Godot's contract), so the lazily-filled
	// cache has to be mutable.
	mutable std::vector<VerseLexState> line_start_state;

	godot::Color comment_color = godot::Color(0.4f, 0.6f, 0.4f);
	godot::Color string_color = godot::Color(0.94f, 0.83f, 0.53f);
	godot::Color keyword_color = godot::Color(1.0f, 0.44f, 0.52f);
	godot::Color control_flow_keyword_color = godot::Color(1.0f, 0.55f, 0.8f);
	godot::Color number_color = godot::Color(0.63f, 0.82f, 0.99f);
	godot::Color symbol_color = godot::Color(0.67f, 0.79f, 1.0f);
	godot::Color function_color = godot::Color(0.34f, 0.7f, 1.0f);
	godot::Color member_color = godot::Color(0.74f, 0.48f, 0.95f);
	godot::Color text_color = godot::Color(0.85f, 0.85f, 0.85f);
};
