#pragma once

#include <godot_cpp/classes/editor_context_menu_plugin.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>

// "Make Verse Module" in the FileSystem dock's directory context menu.
//
// Without it the phase's headline feature would need the author to leave the editor: Godot's dock
// can create a script, a scene or a resource, and nothing else -- there is no "new empty file" --
// so a `.vmodule` marker could not be made from inside it at all.
class VerseModuleMenu : public godot::EditorContextMenuPlugin {
	GDCLASS(VerseModuleMenu, godot::EditorContextMenuPlugin)

protected:
	static void _bind_methods();

public:
	// Offered only when the selection is a single directory, because a module is a property of one.
	void _popup_menu(const godot::PackedStringArray &p_paths) override;

private:
	void make_module(const godot::Variant &p_paths);
};
