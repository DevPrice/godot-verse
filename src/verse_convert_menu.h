#pragma once

#include <godot_cpp/classes/editor_context_menu_plugin.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>

#include <functional>
#include <map>
#include <string>
#include <vector>

struct VerseGdScriptMove;

namespace godot {
class AcceptDialog;
}

// "Convert to Verse" in the FileSystem dock and on the script editor's script list
// (docs/gdscript-conversion.md).
//
// The conversion itself is `verse_gd_convert_batch`, which is pure and tested in the units layer;
// this is everything around it that needs the editor: reading the project (the scenes that say what
// `$Child` is, the global class list), writing the result as one undoable action, and the second,
// separately undoable action that updates the GDScripts that call into what was converted.
//
// A conversion is a set of whole-file writes, so its undo is too: every file it touches is kept as
// it was -- or as absent -- and undo writes them back. That is exact for the files, and the editor's
// own state is refreshed from them both ways: the filesystem is rescanned, a UID is pointed back at
// the file it names, and an open scene or script is reloaded.
class VerseConvertMenu : public godot::EditorContextMenuPlugin {
	GDCLASS(VerseConvertMenu, godot::EditorContextMenuPlugin)

protected:
	static void _bind_methods();

public:
	// Offered when every selected path is a `.gd`: several at once are one batch and one undo. The
	// same object serves both slots, because the only difference is what the callback is handed --
	// the dock the selected paths, the script editor the Script itself.
	void _popup_menu(const godot::PackedStringArray &p_paths) override;

	// Undo and redo land here: `p_files` maps a `res://` path to its text, or to null for a file
	// that did not exist (or no longer does).
	void apply_files(const godot::Dictionary &p_files);

private:
	void convert_selection(const godot::Variant &p_selection);
	void convert_paths(const godot::PackedStringArray &p_paths);
	// Read against the project as it was *before* the conversion: afterwards `$Player` resolves
	// through `player.verse`, which declares no GDScript `class_name`, and would no longer be
	// recognised as the class whose callers these are.
	void prepare_caller_updates(const std::vector<VerseGdScriptMove> &p_moves, const std::vector<std::string> &p_converted,
			const godot::PackedStringArray &p_project_files, const std::function<std::string(const std::string &)> &p_read);
	void update_callers();
	void report(const godot::String &p_title, const godot::String &p_text);

	// Godot refuses a second exclusive dialog while one is open, so they are shown in turn.
	void queue_dialog(godot::AcceptDialog *p_dialog);
	void show_next_dialog();
	void dialog_closed(godot::AcceptDialog *p_dialog);
	std::vector<godot::AcceptDialog *> dialogs;
	bool dialog_open = false;

	// The caller rewrite the dialog is asking about, held until the author answers it.
	godot::Dictionary pending_caller_before;
	godot::Dictionary pending_caller_after;
	godot::AcceptDialog *pending_caller_dialog = nullptr;
};
