#pragma once

#include "verse_syntax_highlighter.h"

#include <godot_cpp/classes/editor_plugin.hpp>

// Registers VerseSyntaxHighlighter with the script editor for the life of the plugin.
class VerseEditorPlugin : public godot::EditorPlugin {
	GDCLASS(VerseEditorPlugin, godot::EditorPlugin)

protected:
	static void _bind_methods();

public:
	void _enter_tree() override;
	void _exit_tree() override;

private:
	godot::Ref<VerseSyntaxHighlighter> highlighter;
};
