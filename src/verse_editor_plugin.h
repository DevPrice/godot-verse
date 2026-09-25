#pragma once

#ifdef TOOLS_ENABLED
#include "verse_convert_menu.h"
#endif
#include "verse_export_plugin.h"
#include "verse_module_menu.h"
#include "verse_syntax_highlighter.h"

#include <godot_cpp/classes/editor_plugin.hpp>

// The editor-only half of the bridge: syntax highlighting, and the two places a build is asked
// for.
class VerseEditorPlugin : public godot::EditorPlugin {
	GDCLASS(VerseEditorPlugin, godot::EditorPlugin)

protected:
	static void _bind_methods();

public:
	void _enter_tree() override;
	void _exit_tree() override;

	// Called by EditorNode::call_build() before a run, on every plugin in registration order,
	// and the run is abandoned if any of them returns false. This is both the build trigger and
	// R-ITER-5's reporting path: a project that does not compile refuses to launch rather than
	// launching the previous generation and looking like the edit did nothing.
	//
	// The same hook C# uses (GodotSharpEditor -> BuildManager.EditorBuildCallback), for the same
	// reason: its compilation unit is the whole project too, so building on save would pay for
	// the project on every Ctrl+S.
	bool _build() override;

	// Watches for a Play session ending, so a fatal error the game's host recorded before it died is
	// shown here: the game cannot report it, and this is the window the author is looking at.
	void _process(double p_delta) override;

private:
	bool was_playing = false;

	void build_from_menu();

	// Godot raises the completion popup on its own from a table of trigger characters, and
	// the table is the editor's rather than the language's. Two positions this bridge
	// answers for are not in it; this puts them there, on the Verse script's own CodeEdit.
	void widen_completion_prefixes();

	godot::Ref<VerseSyntaxHighlighter> highlighter;
	godot::Ref<VerseModuleMenu> module_menu;
	// Two registrations of one plugin: the FileSystem dock's context menu and the script editor's
	// script list, which is where a GDScript is right-clicked (docs/gdscript-conversion.md).
#ifdef TOOLS_ENABLED
	godot::Ref<VerseConvertMenu> convert_menu;
	godot::Ref<VerseConvertMenu> convert_menu_script_editor;
#endif
	godot::Ref<VerseExportPlugin> export_plugin;
};
