#pragma once

#include "verse_lexer.h"

#include <godot_cpp/classes/editor_syntax_highlighter.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Colours Verse source through the lexer in verse_lexer.h/.cpp, which is what makes nested
// comments and comments-inside-strings come out right where Godot's stock CodeHighlighter
// (fed from VerseScriptLanguage's delimiter lists) cannot express them.
// Which of the editor theme's three type colours a name draws in. GDScript's own split,
// gdscript_highlighter.cpp:776-818: an exposed ClassDB class and a global enum are engine
// types, a global script class is a user type, and the Variant type names are base types.
enum class VerseTypeKind {
	Base,
	Engine,
	User,
};

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

	// p_begin and p_end are byte offsets into p_utf8 bounding one identifier.
	godot::Color color_for_identifier(const godot::CharString &p_utf8, int p_begin, int p_end) const;

	// Godot's own three-tier comment-marker convention (critical/warning/notice), keyed the way
	// EditorSettings' comment_marker_colors array is: index order must match CommentMarkerLevel.
	enum class CommentMarkerLevel {
		Critical,
		Warning,
		Notice,
	};

	// The running byte->char conversion _get_line_syntax_highlighting already does across a
	// line's tokens (see the comment there on why the two counting conventions can disagree),
	// factored out so highlight_comment_markers can extend the same monotonic walk across marker
	// boundaries that fall inside one comment token's span.
	static int to_char_column(const godot::CharString &p_utf8, bool p_byte_offsets_differ, int p_byte_column, int &r_byte_cursor, int &r_char_cursor);

	// p_begin/p_end bound one Comment token in byte offsets. Colours each marker word GDScript's
	// three lists recognise -- an exact, case-sensitive match of a whole run of identifier
	// characters, the same boundary GDScript's own highlighter uses -- and restores comment_color
	// immediately after, so a marker reads as a highlight inside the comment rather than a new
	// colour that keeps going.
	void highlight_comment_markers(const godot::CharString &p_utf8, int p_begin, int p_end, bool p_byte_offsets_differ, int &r_byte_cursor, int &r_char_cursor, godot::Dictionary &r_result) const;

	// Bound so it can be reached by name through Callable(this, "..."), which is what a signal
	// connection needs. Godot's own SyntaxHighlighter::_lines_edited_from (scene/resources/
	// syntax_highlighter.cpp) only erases the base class's highlighting_cache -- it never calls
	// our _clear_highlighting_cache override -- so line_start_state and the name sets below are
	// otherwise never told that anything below the edited line is now unreliable.
	void _on_lines_edited_from(int32_t p_from_line, int32_t p_to_line);

	// Shared by _update_cache (always, since a new theme or a reload invalidates everything) and
	// by _get_line_syntax_highlighting (lazily, when names_dirty says an edit touched the file).
	// const because the lazy caller is const; the sets it fills are mutable for the same reason.
	void rebuild_name_caches() const;

	// Fills member_names from the analysis snapshot -- what the edited script's class declares,
	// plus the properties and signal accessors it inherits. False when there is nothing to read
	// (no host, nothing built yet, or a file no analysis has seen), which is when the lexical scan
	// in rebuild_name_caches is the only source there is.
	bool collect_analysed_members() const;

	// line_start_state[i] is the lexer state line i begins in; index 0 is always the default
	// state. _get_line_syntax_highlighting is const (Godot's contract), so the lazily-filled
	// cache has to be mutable.
	mutable std::vector<VerseLexState> line_start_state;

	// The TextEdit _on_lines_edited_from is currently connected to, by instance id rather than a
	// raw pointer: _update_cache runs again for a *different* TextEdit only after Godot has
	// already repointed SyntaxHighlighter's own text_edit, so this is the only remaining chance to
	// disconnect from the old one, and by then it may already be freed -- ObjectDB::get_instance
	// answers null rather than a dangling pointer when it is.
	uint64_t connected_text_edit_id = 0;

	// Set by _on_lines_edited_from, cleared by rebuild_name_caches. A rebuild scans every line of
	// the file, so it happens once per query that needs it rather than once per keystroke or once
	// per line of a pasted block.
	mutable bool names_dirty = true;

	// Every class name a script could name: the mirrored Godot API, the native package's
	// hand-written `object`, plus the class each .verse file in the project defines. A set of
	// names rather than source positions on purpose -- a
	// name does not move when a line is inserted above it, so colouring stays put while the
	// author types, which is the one thing per-keystroke recolouring cannot tolerate.
	mutable std::unordered_map<std::string, VerseTypeKind> type_names;

	// Every field the currently open script can reach bare (Verse code writes no `self.`), so
	// unlike a dotted access these need their name on record to colour. Read out of the analysis
	// where there is one -- the class's own members and everything it inherits from the mirror --
	// and otherwise scanned out of the buffer, one level of indent under the class header, the way
	// `mover.verse` indents `Speed` and `Direction`. Rebuilt whenever the file changes, unlike
	// type_names: a field is only a field in the file that declares it.
	mutable std::unordered_set<std::string> member_names;

	// Read once per _update_cache, not per line: text_editor/theme/highlighting/comment_markers/
	// {critical,warning,notice}_list, split and mapped to the level that colours it. Case-
	// sensitive, matching the HashMap<String, ...> GDScript's own highlighter keys the same lists
	// by, so the same word list behaves the same in both languages.
	std::unordered_map<std::string, CommentMarkerLevel> comment_markers;
	godot::Color comment_marker_colors[3] = {
		godot::Color(0.77f, 0.35f, 0.35f),
		godot::Color(0.72f, 0.61f, 0.48f),
		godot::Color(0.56f, 0.67f, 0.51f),
	};

	godot::Color comment_color = godot::Color(0.4f, 0.6f, 0.4f);
	godot::Color string_color = godot::Color(0.94f, 0.83f, 0.53f);
	godot::Color keyword_color = godot::Color(1.0f, 0.44f, 0.52f);
	godot::Color control_flow_keyword_color = godot::Color(1.0f, 0.55f, 0.8f);
	godot::Color number_color = godot::Color(0.63f, 0.82f, 0.99f);
	godot::Color symbol_color = godot::Color(0.67f, 0.79f, 1.0f);
	godot::Color function_color = godot::Color(0.34f, 0.7f, 1.0f);
	godot::Color function_definition_color = godot::Color(0.4f, 0.9f, 1.0f);
	godot::Color annotation_color = godot::Color(1.0f, 0.7f, 0.45f);
	godot::Color base_type_color = godot::Color(0.64f, 1.0f, 0.83f);
	godot::Color engine_type_color = godot::Color(0.51f, 0.83f, 1.0f);
	godot::Color user_type_color = godot::Color(0.42f, 0.67f, 0.93f);
	godot::Color member_color = godot::Color(0.74f, 0.48f, 0.95f);
	godot::Color text_color = godot::Color(0.85f, 0.85f, 0.85f);
};
