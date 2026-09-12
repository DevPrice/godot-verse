#include "verse_module_menu.h"

#include "verse_module_map.h"

#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/editor_file_system.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/file_system_dock.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

using namespace godot;

void VerseModuleMenu::_bind_methods() {
	ClassDB::bind_method(D_METHOD("make_module", "paths"), &VerseModuleMenu::make_module);
}

void VerseModuleMenu::_popup_menu(const PackedStringArray &p_paths) {
	if (p_paths.size() != 1 || !p_paths[0].ends_with("/")) {
		return;
	}
	add_context_menu_item("Make Verse Module", callable_mp(this, &VerseModuleMenu::make_module));
}

void VerseModuleMenu::make_module(const Variant &p_paths) {
	const PackedStringArray paths = p_paths;
	if (paths.is_empty()) {
		return;
	}

	// Godot hands a directory back with its trailing slash; the marker's name comes from the
	// directory's, which is only a starting point -- the whole reason the marker names the module
	// is that a Godot asset directory is not obliged to be a Verse identifier. The author renames
	// the file if it is not one, and the diagnostic on the next build says so.
	const String directory = paths[0].trim_suffix("/");
	const String suggested = directory.get_file();
	const String marker = directory.path_join(suggested + String(".vmodule"));

	if (FileAccess::file_exists(marker)) {
		UtilityFunctions::push_warning(marker + String(" already exists, so this directory is already a Verse module."));
		return;
	}

	Ref<FileAccess> file = FileAccess::open(marker, FileAccess::WRITE);
	if (file.is_null()) {
		UtilityFunctions::push_error(String("Could not create ") + marker + ".");
		return;
	}
	// Empty on purpose: everything the marker says, it says with its name.
	file->close();

	if (!verse_is_valid_module_name(std::string(suggested.utf8().get_data()))) {
		UtilityFunctions::push_warning(marker + String(" is named after its directory, and `") + suggested
				+ String("` is not a Verse module name. Rename the file -- a letter or underscore followed ")
				+ String("by letters, digits or underscores -- and the directory can keep the name it has."));
	}

	EditorInterface::get_singleton()->get_resource_filesystem()->scan();
}
