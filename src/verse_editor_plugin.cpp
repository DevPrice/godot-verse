#include "verse_editor_plugin.h"

#include "verse_script_language.h"

#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/script_editor.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

using namespace godot;

// What the Project > Tools entry is called. Held as a constant because removing it wants the same
// string that added it.
static const char *BUILD_MENU_ITEM = "Build Verse";

void VerseEditorPlugin::_bind_methods() {
	ClassDB::bind_method(D_METHOD("build_from_menu"), &VerseEditorPlugin::build_from_menu);
}

void VerseEditorPlugin::_enter_tree() {
	highlighter.instantiate();
	EditorInterface::get_singleton()->get_script_editor()->register_syntax_highlighter(highlighter);

	// Play builds on its own; this is for everything else that wants the running code to catch up
	// without a run -- a `@tool` script, or an `@export` default whose value only a build produces.
	add_tool_menu_item(BUILD_MENU_ITEM, callable_mp(this, &VerseEditorPlugin::build_from_menu));
}

void VerseEditorPlugin::_exit_tree() {
	remove_tool_menu_item(BUILD_MENU_ITEM);
	EditorInterface::get_singleton()->get_script_editor()->unregister_syntax_highlighter(highlighter);
	highlighter.unref();
}

bool VerseEditorPlugin::_build() {
	VerseScriptLanguage *language = VerseScriptLanguage::singleton();
	if (language == nullptr) {
		return true; // Nothing registered the language, so there is nothing of ours to build.
	}
	return language->build_project() == OK;
}

void VerseEditorPlugin::build_from_menu() {
	VerseScriptLanguage *language = VerseScriptLanguage::singleton();
	if (language == nullptr) {
		return;
	}
	// Said out loud only on success: a failure has already printed the compiler's diagnostics and
	// the warning that says what they cost, and a second line adds nothing.
	if (language->build_project() == OK) {
		UtilityFunctions::print("Verse: built.");
	}
}
