#pragma once

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

private:
	void build_from_menu();

	godot::Ref<VerseSyntaxHighlighter> highlighter;
};
