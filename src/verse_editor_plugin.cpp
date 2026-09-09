#include "verse_editor_plugin.h"

#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/script_editor.hpp>

using namespace godot;

void VerseEditorPlugin::_bind_methods() {
}

void VerseEditorPlugin::_enter_tree() {
	highlighter.instantiate();
	EditorInterface::get_singleton()->get_script_editor()->register_syntax_highlighter(highlighter);
}

void VerseEditorPlugin::_exit_tree() {
	EditorInterface::get_singleton()->get_script_editor()->unregister_syntax_highlighter(highlighter);
	highlighter.unref();
}
