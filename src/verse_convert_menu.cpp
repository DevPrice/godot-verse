#include "verse_convert_menu.h"

#include "verse_bindings.h"
#include "verse_gd_convert.h"

#include <godot_cpp/classes/accept_dialog.hpp>
#include <godot_cpp/classes/button.hpp>
#include <godot_cpp/classes/class_db_singleton.hpp>
#include <godot_cpp/classes/confirmation_dialog.hpp>
#include <godot_cpp/classes/control.hpp>
#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/editor_file_system.hpp>
#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/editor_undo_redo_manager.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/file_system_dock.hpp>
#include <godot_cpp/classes/label.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/resource_uid.hpp>
#include <godot_cpp/classes/script.hpp>
#include <godot_cpp/classes/script_editor.hpp>
#include <godot_cpp/classes/text_edit.hpp>
#include <godot_cpp/classes/v_box_container.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <set>

using namespace godot;

namespace {

// The key in an `apply_files` dictionary that is not a file: which script replaced which, so a
// script open in the editor can be swapped for its replacement in either direction.
const char *PAIRS_KEY = "__pairs__";

std::string utf8(const String &p_text) {
	return std::string(p_text.utf8().get_data());
}

String text(const std::string &p_text) {
	return String::utf8(p_text.c_str());
}

// Every file under `p_dir` the conversion reads or may rewrite. `.godot` is the import cache and
// `addons/` the extensions' own files, which is the rule `find_project_files` already follows.
void walk(const String &p_dir, PackedStringArray &r_out) {
	Ref<DirAccess> dir = DirAccess::open(p_dir);
	if (dir.is_null()) {
		return;
	}
	const PackedStringArray files = dir->get_files();
	for (int64_t i = 0; i < files.size(); i++) {
		const String ext = files[i].get_extension().to_lower();
		if (ext == "gd" || ext == "tscn" || ext == "tres" || ext == "scn" || ext == "res") {
			r_out.push_back(p_dir.path_join(files[i]));
		}
	}
	const PackedStringArray subdirs = dir->get_directories();
	for (int64_t i = 0; i < subdirs.size(); i++) {
		if (subdirs[i].begins_with(".") || (p_dir == "res://" && subdirs[i] == "addons")) {
			continue;
		}
		walk(p_dir.path_join(subdirs[i]), r_out);
	}
}

struct file_cache {
	std::map<std::string, std::string> texts;
	const std::string &read(const std::string &p_path) {
		auto found = texts.find(p_path);
		if (found != texts.end()) {
			return found->second;
		}
		const String path = text(p_path);
		std::string content;
		if (FileAccess::file_exists(path)) {
			content = utf8(FileAccess::get_file_as_string(path));
		}
		return texts.emplace(p_path, content).first->second;
	}
};

Control *base_control() {
	return EditorInterface::get_singleton()->get_base_control();
}

TextEdit *make_list(const String &p_text) {
	TextEdit *list = memnew(TextEdit);
	list->set_editable(false);
	list->set_text(p_text);
	list->set_custom_minimum_size(Vector2(760, 320));
	list->set_v_size_flags(Control::SIZE_EXPAND_FILL);
	return list;
}

} // namespace

void VerseConvertMenu::_bind_methods() {
	ClassDB::bind_method(D_METHOD("convert_selection", "selection"), &VerseConvertMenu::convert_selection);
	ClassDB::bind_method(D_METHOD("apply_files", "files"), &VerseConvertMenu::apply_files);
	ClassDB::bind_method(D_METHOD("update_callers"), &VerseConvertMenu::update_callers);
	ClassDB::bind_method(D_METHOD("show_next_dialog"), &VerseConvertMenu::show_next_dialog);
}

void VerseConvertMenu::queue_dialog(AcceptDialog *p_dialog) {
	base_control()->add_child(p_dialog);
	p_dialog->connect("confirmed", callable_mp(this, &VerseConvertMenu::dialog_closed).bind(p_dialog));
	p_dialog->connect("canceled", callable_mp(this, &VerseConvertMenu::dialog_closed).bind(p_dialog));
	dialogs.push_back(p_dialog);
	if (!dialog_open) {
		show_next_dialog();
	}
}

void VerseConvertMenu::show_next_dialog() {
	if (dialogs.empty()) {
		dialog_open = false;
		return;
	}
	AcceptDialog *next = dialogs.front();
	dialogs.erase(dialogs.begin());
	dialog_open = true;
	next->popup_centered();
}

void VerseConvertMenu::dialog_closed(AcceptDialog *p_dialog) {
	p_dialog->queue_free();
	dialog_open = false;
	// Deferred: the closing dialog still holds the exclusive slot until this frame ends.
	callable_mp(this, &VerseConvertMenu::show_next_dialog).call_deferred();
}

void VerseConvertMenu::_popup_menu(const PackedStringArray &p_paths) {
	if (p_paths.is_empty()) {
		return;
	}
	for (int64_t i = 0; i < p_paths.size(); i++) {
		if (p_paths[i].get_extension().to_lower() != "gd") {
			return;
		}
	}
	add_context_menu_item("Convert to Verse", callable_mp(this, &VerseConvertMenu::convert_selection));
}

void VerseConvertMenu::convert_selection(const Variant &p_selection) {
	PackedStringArray paths;
	if (p_selection.get_type() == Variant::OBJECT) {
		// The script editor hands over the Script it was opened on.
		const Script *script = Object::cast_to<Script>(p_selection.operator Object *());
		if (script != nullptr) {
			paths.push_back(script->get_path());
		}
	} else if (p_selection.get_type() == Variant::PACKED_STRING_ARRAY) {
		paths = p_selection;
	} else if (p_selection.get_type() == Variant::ARRAY) {
		const Array selection = p_selection;
		for (int64_t i = 0; i < selection.size(); i++) {
			paths.push_back(selection[i]);
		}
	}
	convert_paths(paths);
}

void VerseConvertMenu::report(const String &p_title, const String &p_text) {
	AcceptDialog *dialog = memnew(AcceptDialog);
	dialog->set_title(p_title);
	dialog->add_child(make_list(p_text));
	queue_dialog(dialog);
}

void VerseConvertMenu::convert_paths(const PackedStringArray &p_paths) {
	std::vector<std::string> selected;
	for (int64_t i = 0; i < p_paths.size(); i++) {
		if (p_paths[i].get_extension().to_lower() == "gd") {
			selected.push_back(utf8(p_paths[i]));
		}
	}
	// A selection is a set: the order it was clicked in is not an input.
	std::sort(selected.begin(), selected.end());
	selected.erase(std::unique(selected.begin(), selected.end()), selected.end());
	if (selected.empty()) {
		return;
	}

	// What is on disk is what is converted, and what the scenes are rewritten from -- so an unsaved
	// edit is saved first, the way Play saves before a run.
	EditorInterface *editor = EditorInterface::get_singleton();
	editor->save_all_scenes();
	if (ScriptEditor *scripts = editor->get_script_editor()) {
		scripts->save_all_scripts();
	}

	PackedStringArray project_files;
	walk("res://", project_files);
	file_cache files;
	const VerseGdResourceReader read = [&files](const std::string &p_path) { return files.read(p_path); };

	// Every script class the project declares, with the Godot class it finally extends.
	const TypedArray<Dictionary> globals = ProjectSettings::get_singleton()->get_global_class_list();
	std::map<std::string, std::pair<std::string, std::string>> global_classes; // name -> (path, declared base)
	for (int64_t i = 0; i < globals.size(); i++) {
		const Dictionary entry = globals[i];
		global_classes[utf8(entry.get("class", String()))] = { utf8(entry.get("path", String())), utf8(entry.get("base", String())) };
	}
	ClassDBSingleton *db = ClassDBSingleton::get_singleton();
	auto native_base = [&](std::string p_base) {
		for (int depth = 0; depth < 32 && !p_base.empty(); depth++) {
			if (db->class_exists(text(p_base))) {
				return p_base;
			}
			auto found = global_classes.find(p_base);
			if (found == global_classes.end()) {
				break;
			}
			p_base = found->second.second;
		}
		return std::string("Node");
	};

	// The batch, and where each file is going.
	std::map<std::string, std::string> new_paths;
	std::map<std::string, std::string> class_names;
	std::vector<std::string> problems;
	std::set<std::string> targets;
	for (const std::string &path : selected) {
		const std::string source = files.read(path);
		const std::string class_name = verse_gd_scan_class_name(source);
		const std::string stem = verse_gd_stem(utf8(text(path).get_file().get_basename()), class_name);
		const std::string target = utf8(text(path).get_base_dir().path_join(text(stem) + ".verse"));
		if (FileAccess::file_exists(text(target))) {
			problems.push_back(path + ": " + target + " already exists, and converting would overwrite it");
		}
		if (!targets.insert(target).second) {
			problems.push_back(path + ": another selected script also converts to " + target);
		}
		new_paths[path] = target;
		class_names[path] = class_name;
	}

	std::map<std::string, VerseGdScriptClass> script_classes;
	for (const auto &kv : global_classes) {
		const std::string &name = kv.first;
		const std::string &path = kv.second.first;
		if (name.empty()) {
			continue;
		}
		VerseGdScriptClass c;
		c.path = path;
		c.godot_base = native_base(kv.second.second);
		auto converting = new_paths.find(path);
		if (converting != new_paths.end()) {
			c.verse_name = utf8(text(converting->second).get_file().get_basename());
		} else if (text(path).get_extension().to_lower() == "gd") {
			c.verse_name = verse_binding_class_name(name);
			c.is_binding = true;
		} else {
			c.verse_name = utf8(text(path).get_file().get_basename());
		}
		script_classes[name] = c;
	}

	std::vector<VerseGdConvertInput> inputs;
	for (const std::string &path : selected) {
		VerseGdConvertInput input;
		input.source = files.read(path);
		input.file_stem = utf8(text(path).get_file().get_basename());
		input.script_classes = script_classes;
		input.script_classes.erase(class_names[path]);
		// What `$Child` is, from every scene that puts this script on a node. Two scenes that
		// disagree about a path meet at the nearest class they share.
		for (int64_t i = 0; i < project_files.size(); i++) {
			if (project_files[i].get_extension().to_lower() != "tscn") {
				continue;
			}
			const std::map<std::string, std::string> types = verse_gd_scene_node_types(files.read(utf8(project_files[i])), path, read);
			for (const auto &type : types) {
				auto existing = input.node_types.find(type.first);
				input.node_types[type.first] = existing == input.node_types.end() ? type.second : verse_gd_common_class(existing->second, type.second);
			}
		}
		inputs.push_back(input);
	}

	const std::vector<VerseGdConvertResult> results = verse_gd_convert_batch(inputs);
	for (size_t i = 0; i < results.size(); i++) {
		if (!results[i].ok) {
			problems.push_back(selected[i] + ":" + std::to_string(results[i].error_line) + ": does not parse -- " + results[i].error);
		}
	}
	if (!problems.empty()) {
		String message = "Nothing was converted:\n\n";
		for (const std::string &p : problems) {
			message += text(p) + "\n";
		}
		report("Convert to Verse", message);
		return;
	}

	// The whole change as files before and after, which is what makes it one undo.
	Dictionary before;
	Dictionary after;
	Dictionary forward_pairs;
	Dictionary backward_pairs;
	std::vector<VerseGdScriptMove> moves;
	for (size_t i = 0; i < selected.size(); i++) {
		const std::string &old_path = selected[i];
		const std::string &new_path = new_paths[old_path];
		before[text(old_path)] = text(files.read(old_path));
		after[text(old_path)] = Variant();
		before[text(new_path)] = Variant();
		after[text(new_path)] = text(results[i].verse);
		// The UID moves with the script, so every `uid://` that named the GDScript names the Verse.
		const String old_uid = text(old_path) + ".uid";
		if (FileAccess::file_exists(old_uid)) {
			const String uid_text = FileAccess::get_file_as_string(old_uid);
			before[old_uid] = uid_text;
			after[old_uid] = Variant();
			before[text(new_path) + ".uid"] = Variant();
			after[text(new_path) + ".uid"] = uid_text;
		}
		forward_pairs[text(old_path)] = text(new_path);
		backward_pairs[text(new_path)] = text(old_path);

		VerseGdScriptMove move;
		move.old_path = old_path;
		move.new_path = new_path;
		move.old_class_name = results[i].gd_class_name;
		move.new_class_name = results[i].global_name;
		move.renames = results[i].renames;
		moves.push_back(move);
	}
	int binary_resources = 0;
	for (int64_t i = 0; i < project_files.size(); i++) {
		const String ext = project_files[i].get_extension().to_lower();
		if (ext == "scn" || ext == "res") {
			binary_resources++;
			continue;
		}
		if (ext != "tscn" && ext != "tres") {
			continue;
		}
		const std::string path = utf8(project_files[i]);
		const std::string &old_text = files.read(path);
		const std::string new_text = verse_gd_rewrite_resource(old_text, moves, read);
		if (new_text != old_text) {
			before[project_files[i]] = text(old_text);
			after[project_files[i]] = text(new_text);
		}
	}
	after[PAIRS_KEY] = forward_pairs;
	before[PAIRS_KEY] = backward_pairs;

	prepare_caller_updates(moves, selected, project_files, read);

	EditorUndoRedoManager *undo = editor->get_editor_undo_redo();
	undo->create_action(selected.size() == 1 ? "Convert to Verse" : "Convert " + itos(int64_t(selected.size())) + " Scripts to Verse");
	undo->add_do_method(this, "apply_files", after);
	undo->add_undo_method(this, "apply_files", before);
	undo->commit_action();

	// What the author has to look at, in the Output panel and -- when there is anything to do --
	// in a dialog too.
	String summary;
	int todos = 0;
	for (size_t i = 0; i < selected.size(); i++) {
		const VerseGdConvertResult &r = results[i];
		todos += r.todo_count();
		summary += text(selected[i]) + " -> " + text(new_paths[selected[i]]);
		if (!r.gd_class_name.empty() && r.global_name != r.gd_class_name) {
			summary += "  (registered as " + text(r.global_name) + ")";
		}
		summary += "\n";
		for (const VerseGdNote &note : r.notes) {
			summary += String("    ") + (note.line ? "line " + itos(note.line) + ": " : String()) + (note.todo ? "TODO: " : "") + text(note.message) + "\n";
		}
	}
	int touched_resources = 0;
	const Array keys = after.keys();
	for (int64_t i = 0; i < keys.size(); i++) {
		const String key = keys[i];
		touched_resources += key.ends_with(".tscn") || key.ends_with(".tres") ? 1 : 0;
	}
	summary += "\n" + itos(touched_resources) + " scene and resource file(s) updated for the new names.";
	if (binary_resources > 0) {
		summary += "\n" + itos(binary_resources) + " binary .scn/.res file(s) could not be checked; if one uses a converted script, re-save it as text.";
	}
	summary += "\nUndo with Edit > Undo.";
	UtilityFunctions::print(String("Convert to Verse:\n") + summary);
	if (todos > 0) {
		report("Converted to Verse, with " + itos(todos) + " TODO(s)", summary);
	}
	if (pending_caller_dialog != nullptr) {
		queue_dialog(pending_caller_dialog);
		pending_caller_dialog = nullptr;
	}
}

void VerseConvertMenu::prepare_caller_updates(const std::vector<VerseGdScriptMove> &p_moves, const std::vector<std::string> &p_converted,
		const PackedStringArray &p_project_files, const std::function<std::string(const std::string &)> &p_read) {
	const PackedStringArray &project_files = p_project_files;
	const VerseGdResourceReader &read = p_read;
	pending_caller_dialog = nullptr;
	pending_caller_before.clear();
	pending_caller_after.clear();
	String listing;
	int rewritten = 0;
	int unresolved = 0;
	for (int64_t i = 0; i < project_files.size(); i++) {
		if (project_files[i].get_extension().to_lower() != "gd") {
			continue;
		}
		const std::string path = utf8(project_files[i]);
		if (std::find(p_converted.begin(), p_converted.end(), path) != p_converted.end()) {
			continue;
		}
		std::map<std::string, std::string> node_types;
		for (int64_t k = 0; k < project_files.size(); k++) {
			if (project_files[k].get_extension().to_lower() == "tscn") {
				const std::map<std::string, std::string> types = verse_gd_scene_node_types(read(utf8(project_files[k])), path, read);
				for (const auto &type : types) {
					auto existing = node_types.find(type.first);
					node_types[type.first] = existing == node_types.end() ? type.second : verse_gd_common_class(existing->second, type.second);
				}
			}
		}
		const std::string source = read(path);
		const VerseGdCallerRewrite rewrite = verse_gd_rewrite_callers(source, p_moves, node_types);
		for (const VerseGdCallerSite &site : rewrite.sites) {
			listing += text(path) + ":" + itos(site.line) + "  " + text(site.source) + "\n";
			if (site.rewritten) {
				listing += "    will be updated\n";
				rewritten++;
			} else {
				listing += "    needs a look: " + text(site.reason) + "\n";
				unresolved++;
			}
		}
		if (rewrite.changed) {
			pending_caller_before[project_files[i]] = text(source);
			pending_caller_after[project_files[i]] = text(rewrite.text);
		}
	}
	if (rewritten + unresolved == 0) {
		return;
	}
	UtilityFunctions::print(String("Convert to Verse: GDScript that uses what was converted:\n") + listing);

	const String header = itos(rewritten) + " reference(s) in other GDScript files can be updated, because the receiver's type is known there. "
			+ itos(unresolved) + " more may refer to what was converted and are left for you to check.";
	if (rewritten == 0) {
		AcceptDialog *dialog = memnew(AcceptDialog);
		dialog->set_title("GDScript That May Use the Converted Scripts");
		dialog->add_child(make_list(header + String("\n\n") + listing));
		pending_caller_dialog = dialog;
		return;
	}
	ConfirmationDialog *dialog = memnew(ConfirmationDialog);
	dialog->set_title("Update References to the Converted Scripts?");
	VBoxContainer *box = memnew(VBoxContainer);
	Label *label = memnew(Label);
	label->set_text(header);
	label->set_autowrap_mode(TextServer::AUTOWRAP_WORD_SMART);
	label->set_custom_minimum_size(Vector2(760, 0));
	box->add_child(label);
	box->add_child(make_list(listing));
	dialog->add_child(box);
	dialog->get_ok_button()->set_text("Update " + itos(rewritten) + " Reference(s)");
	dialog->get_cancel_button()->set_text("Leave Them");
	dialog->connect("confirmed", callable_mp(this, &VerseConvertMenu::update_callers));
	pending_caller_dialog = dialog;
}

void VerseConvertMenu::update_callers() {
	if (pending_caller_after.is_empty()) {
		return;
	}
	EditorUndoRedoManager *undo = EditorInterface::get_singleton()->get_editor_undo_redo();
	undo->create_action("Update References to Converted Scripts");
	undo->add_do_method(this, "apply_files", pending_caller_after.duplicate());
	undo->add_undo_method(this, "apply_files", pending_caller_before.duplicate());
	undo->commit_action();
	pending_caller_before.clear();
	pending_caller_after.clear();
}

void VerseConvertMenu::apply_files(const Dictionary &p_files) {
	EditorInterface *editor = EditorInterface::get_singleton();
	ScriptEditor *script_editor = editor->get_script_editor();
	std::set<std::string> open_scripts;
	if (script_editor != nullptr) {
		const TypedArray<Script> open = script_editor->get_open_scripts();
		for (int64_t i = 0; i < open.size(); i++) {
			const Ref<Script> script = open[i];
			if (script.is_valid()) {
				open_scripts.insert(utf8(script->get_path()));
			}
		}
	}

	PackedStringArray written;
	PackedStringArray removed;
	const Array keys = p_files.keys();
	for (int64_t i = 0; i < keys.size(); i++) {
		const String path = keys[i];
		if (path == PAIRS_KEY) {
			continue;
		}
		const Variant content = p_files[path];
		if (content.get_type() == Variant::NIL) {
			if (FileAccess::file_exists(path)) {
				DirAccess::remove_absolute(path);
				removed.push_back(path);
			}
			continue;
		}
		Ref<FileAccess> file = FileAccess::open(path, FileAccess::WRITE);
		if (file.is_null()) {
			UtilityFunctions::push_error("Convert to Verse: could not write " + path + ".");
			continue;
		}
		file->store_string(content);
		file->close();
		written.push_back(path);
	}

	// A `.uid` file moved: point the UID at the file it now sits beside.
	ResourceUID *uids = ResourceUID::get_singleton();
	for (int64_t i = 0; i < written.size(); i++) {
		if (!written[i].ends_with(".uid")) {
			continue;
		}
		const int64_t id = uids->text_to_id(String(p_files[written[i]]).strip_edges());
		const String target = written[i].trim_suffix(".uid");
		if (id != ResourceUID::INVALID_ID) {
			if (uids->has_id(id)) {
				uids->set_id(id, target);
			} else {
				uids->add_id(id, target);
			}
		}
	}

	EditorFileSystem *filesystem = editor->get_resource_filesystem();
	FileSystemDock *dock = editor->get_file_system_dock();
	for (int64_t i = 0; i < removed.size(); i++) {
		filesystem->update_file(removed[i]);
		// The dock's own signal, which is how the script editor learns to close a tab whose file
		// has gone -- the same one a delete from the dock sends.
		if (dock != nullptr) {
			dock->emit_signal("file_removed", removed[i]);
		}
	}
	for (int64_t i = 0; i < written.size(); i++) {
		filesystem->update_file(written[i]);
	}
	filesystem->scan();

	// A resource already loaded keeps the old text until it is loaded again, and an open scene
	// keeps it until it is reloaded.
	ResourceLoader *loader = ResourceLoader::get_singleton();
	const PackedStringArray open_scenes = editor->get_open_scenes();
	for (int64_t i = 0; i < written.size(); i++) {
		const String &path = written[i];
		if (path.ends_with(".uid")) {
			continue;
		}
		if (loader->has_cached(path)) {
			loader->load(path, "", ResourceLoader::CACHE_MODE_REPLACE);
		}
		if (open_scenes.has(path)) {
			editor->reload_scene_from_path(path);
		}
	}

	// A script that was open is replaced by the one that replaced it.
	const Dictionary pairs = p_files.get(PAIRS_KEY, Dictionary());
	const Array from = pairs.keys();
	for (int64_t i = 0; i < from.size(); i++) {
		if (!open_scripts.count(utf8(from[i]))) {
			continue;
		}
		const Ref<Resource> replacement = loader->load(pairs[from[i]]);
		if (replacement.is_valid()) {
			editor->edit_resource(replacement);
		}
	}
}
