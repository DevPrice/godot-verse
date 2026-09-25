#include "verse_editor_plugin.h"

#include "verse_host_paths.h"
#include "verse_script.h"
#include "verse_script_language.h"

#include <godot_cpp/classes/code_edit.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/script_editor.hpp>
#include <godot_cpp/classes/script_editor_base.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

using namespace godot;

// What the Project > Tools entry is called. Held as a constant because removing it wants the same
// string that added it.
static const char *BUILD_MENU_ITEM = "Build Verse";

void VerseEditorPlugin::_bind_methods() {
	ClassDB::bind_method(D_METHOD("build_from_menu"), &VerseEditorPlugin::build_from_menu);
	ClassDB::bind_method(D_METHOD("widen_completion_prefixes"),
		&VerseEditorPlugin::widen_completion_prefixes);
}

void VerseEditorPlugin::_enter_tree() {
	// The host paths live in EditorSettings now, and a GDExtension has no hook earlier than this
	// to declare them from -- reading them works without it, but they would not appear in the
	// Editor Settings dialog for anyone to set (R-DIST-12).
	verse_host_paths::register_editor_settings();
	verse_host_paths::sync_environment_for_play();

	highlighter.instantiate();
	EditorInterface::get_singleton()->get_script_editor()->register_syntax_highlighter(highlighter);

	// Play builds on its own; this is for everything else that wants the running code to catch up
	// without a run -- a `@tool` script, or an `@export` default whose value only a build produces.
	add_tool_menu_item(BUILD_MENU_ITEM, callable_mp(this, &VerseEditorPlugin::build_from_menu));

	// Godot's FileSystem dock can create a script, a scene or a resource and nothing else, so
	// without this a `.vmodule` marker could not be made from inside the editor at all.
	module_menu.instantiate();
	add_context_menu_plugin(EditorContextMenuPlugin::CONTEXT_SLOT_FILESYSTEM, module_menu);

#ifdef TOOLS_ENABLED
	// "Convert to Verse" on a `.gd`, in the dock and on the script editor's list. One instance per
	// slot, because a plugin is registered against one.
	convert_menu.instantiate();
	add_context_menu_plugin(EditorContextMenuPlugin::CONTEXT_SLOT_FILESYSTEM, convert_menu);
	convert_menu_script_editor.instantiate();
	add_context_menu_plugin(EditorContextMenuPlugin::CONTEXT_SLOT_SCRIPT_EDITOR, convert_menu_script_editor);
#endif

	// An export cooks the project and strips the sources; without this one an exported game ships
	// .verse files it has no compiler to read (R-DIST-9 .. R-DIST-11).
	export_plugin.instantiate();
	add_export_plugin(export_plugin);

	// Fires on every tab switch and on every script opened, after the tab is current
	// (ScriptEditor::_go_to_tab and ::edit both call notify_script_changed at the end), which
	// is what widen_completion_prefixes needs: it reads the editor that is on screen.
	EditorInterface::get_singleton()->get_script_editor()->connect("editor_script_changed",
		callable_mp(this, &VerseEditorPlugin::widen_completion_prefixes).unbind(1));
}

void VerseEditorPlugin::_exit_tree() {
	EditorInterface::get_singleton()->get_script_editor()->disconnect("editor_script_changed",
		callable_mp(this, &VerseEditorPlugin::widen_completion_prefixes).unbind(1));
	remove_export_plugin(export_plugin);
	export_plugin.unref();
#ifdef TOOLS_ENABLED
	remove_context_menu_plugin(convert_menu_script_editor);
	convert_menu_script_editor.unref();
	remove_context_menu_plugin(convert_menu);
	convert_menu.unref();
#endif
	remove_context_menu_plugin(module_menu);
	module_menu.unref();
	remove_tool_menu_item(BUILD_MENU_ITEM);
	EditorInterface::get_singleton()->get_script_editor()->unregister_syntax_highlighter(highlighter);
	highlighter.unref();
}

bool VerseEditorPlugin::_build() {
	// EditorNode::call_build() runs this right before Play spawns its own process, which is the
	// last point in the editor's process that a Play session's environment can still be changed.
	verse_host_paths::sync_environment_for_play();

	VerseScriptLanguage *language = VerseScriptLanguage::singleton();
	if (language == nullptr) {
		return true; // Nothing registered the language, so there is nothing of ours to build.
	}
	return language->build_project() == OK;
}

// Godot decides for itself whether to raise the completion popup, and the test is a table of
// trigger characters CodeTextEditor hard-codes -- `.`, `,`, `(`, `=`, `$`, `@`, `"` and `'`
// (editor/gui/code_editor.cpp). A caret with nothing typed after a character outside that table is
// cancelled outright by CodeEdit::_filter_code_completion_candidates, and `force` does not exempt
// it: the only branch `code_completion_forced` reaches is the one for `(`.
//
// Two of the positions _complete_code narrows on sit behind a character Godot has never needed.
// `vector2{` offers the archetype's fields and `Foo(?` offers the callee's named parameters, and
// at each of them every other name in scope is *refused* by the compiler rather than merely
// unlikely -- so the popup was answering correctly and being closed before it drew. A second field
// already worked, because `,` is in Godot's table; only the first one was unreachable.
//
// `[` is in the table for a second reason, and it is the argument hint rather than the popup.
// `confirm_code_completion` re-asks for completion once it has inserted an option, and only when
// the inserted text's last character is in this table (scene/gui/code_edit.cpp) -- which is the
// whole of how the hint appears the moment a call is completed. Verse spells a `<decides>` call
// with brackets, so completion_option_for inserts `GetNode[`, and every failable call in the
// mirror -- every object-returning method and all 568 predicates -- landed on a character that
// asked for nothing. The hint was already correct when something asked: call_opened_with_bracket
// exists to spell it with the brackets the author wrote. Nothing asked until the first argument
// character, which reaches _complete_code through the `!is_symbol` arm instead.
//
// The other narrowed positions are deliberately absent. A type after `:` and a specifier after `<`
// decline an empty prefix in _complete_code itself, so putting them here would raise a popup with
// nothing to show.
//
// Set on the script's own CodeEdit rather than globally, so a GDScript tab keeps Godot's set.
void VerseEditorPlugin::widen_completion_prefixes() {
	ScriptEditor *script_editor = EditorInterface::get_singleton()->get_script_editor();
	if (script_editor == nullptr) {
		return;
	}
	const Ref<Script> current = script_editor->get_current_script();
	if (Object::cast_to<VerseScript>(current.ptr()) == nullptr) {
		return;
	}
	ScriptEditorBase *editor = script_editor->get_current_editor();
	CodeEdit *code = editor != nullptr ? Object::cast_to<CodeEdit>(editor->get_base_editor()) : nullptr;
	if (code == nullptr) {
		return;
	}

	TypedArray<String> prefixes = code->get_code_completion_prefixes();
	for (const char *wanted : { "{", "?", "[" }) {
		if (!prefixes.has(String(wanted))) {
			prefixes.push_back(String(wanted));
		}
	}
	code->set_code_completion_prefixes(prefixes);
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
