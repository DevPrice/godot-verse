#include "verse_export_plugin.h"

#include "verse_export_paths.h"
#include "verse_bindings_gen.h"
#include "verse_host_paths.h"
#include "verse_script_language.h"

#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/editor_export_platform.hpp>
#include <godot_cpp/classes/editor_export_plugin.hpp>
#include <godot_cpp/classes/editor_export_preset.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

using namespace godot;

namespace {

// The platforms this bridge does not reach. Mobile is deferred with no design; web left this list
// in Phase 7.5 (docs/phase-7.5-design.md §9), and exports only on the interpreter.
const char *UNREACHABLE_PLATFORMS[] = { "android", "ios" };

// gdextension.py writes [dependencies] last (RUNTIME_HOST_FILES, scanned from bin/ at build
// time), so cutting the text off at its heading drops exactly the runtime host and tbbmalloc.dll
// and nothing else the file declares. If a future dependency belongs there whatever the backend,
// this needs to parse the section rather than truncate it.
const char *DEPENDENCIES_MARKER = "[dependencies]";

String without_gdextension_dependencies(const String &p_text) {
	const int index = p_text.find(DEPENDENCIES_MARKER);
	return index < 0 ? p_text : p_text.substr(0, index);
}

// Web has no "beside the executable" for add_shared_object to copy a directory into -- there is no
// filesystem there at all until the .pck is mounted -- so verse_data ships inside the .pck instead,
// at the res:// path VerseRuntime::load_host and verse_paths::data_dir_for_this_build already agree
// on (design §9). add_file is the one lever an EditorExportPlugin has into the pack; recursing by
// hand is the price of using it for a whole directory instead of one resource.
void add_directory_to_pack(EditorExportPlugin *p_plugin, const String &p_abs_dir, const String &p_res_dir) {
	Ref<DirAccess> dir = DirAccess::open(p_abs_dir);
	if (dir.is_null()) {
		return;
	}
	const PackedStringArray files = dir->get_files();
	for (int64_t i = 0; i < files.size(); i++) {
		Ref<FileAccess> reader = FileAccess::open(p_abs_dir.path_join(files[i]), FileAccess::READ);
		if (reader.is_null()) {
			continue;
		}
		p_plugin->add_file(p_res_dir.path_join(files[i]), reader->get_buffer(reader->get_length()), false);
	}
	const PackedStringArray subdirs = dir->get_directories();
	for (int64_t i = 0; i < subdirs.size(); i++) {
		add_directory_to_pack(p_plugin, p_abs_dir.path_join(subdirs[i]), p_res_dir.path_join(subdirs[i]));
	}
}

// Matches the way Godot applies a preset's include filter: comma-separated globs, each tried against
// the res:// path and against the file name.
bool include_filter_matches(const String &p_filter, const String &p_path) {
	const PackedStringArray patterns = p_filter.split(",", false);
	for (int64_t i = 0; i < patterns.size(); i++) {
		const String pattern = patterns[i].strip_edges();
		if (!pattern.is_empty() && (p_path.matchn(pattern) || p_path.get_file().matchn(pattern))) {
			return true;
		}
	}
	return false;
}

void collect_vmodules(const String &p_dir, PackedStringArray &r_paths) {
	Ref<DirAccess> dir = DirAccess::open(p_dir);
	if (dir.is_null()) {
		return;
	}
	const PackedStringArray files = dir->get_files();
	for (int64_t i = 0; i < files.size(); i++) {
		if (files[i].ends_with(".vmodule")) {
			r_paths.push_back(p_dir.path_join(files[i]));
		}
	}
	const PackedStringArray subdirs = dir->get_directories();
	for (int64_t i = 0; i < subdirs.size(); i++) {
		if (!subdirs[i].begins_with(".")) {
			collect_vmodules(p_dir.path_join(subdirs[i]), r_paths);
		}
	}
}

} // namespace

void VerseExportPlugin::_bind_methods() {}

String VerseExportPlugin::_get_name() const {
	return "Verse";
}

void VerseExportPlugin::say(int p_message_type, const String &p_message) {
	Ref<EditorExportPlatform> platform = get_export_platform();
	if (platform.is_valid()) {
		platform->add_message((EditorExportPlatform::ExportMessageType)p_message_type, "Verse", p_message);
	} else {
		UtilityFunctions::push_error(String("Verse: ") + p_message);
	}
}

void VerseExportPlugin::_export_begin(const PackedStringArray &p_features, bool p_is_debug, const String &p_path, uint32_t p_flags) {
	refused = false;
	temp_dir = String();

	VerseScriptLanguage *language = VerseScriptLanguage::singleton();
	if (language == nullptr) {
		return; // Nothing registered the language, so this project has no Verse in it.
	}

	const PackedStringArray sources = language->find_verse_sources("res://");
	if (sources.is_empty()) {
		return; // A project with no Verse exports exactly as it did before this plugin existed.
	}

	// The vm backend boots no host DLL to load either of these beside (phase-7.5-design.md §9,
	// T5.4), so an export on it withholds both -- but the .gdextension's [dependencies] section
	// that puts them there is generated from what is staged in bin/, not from this project's
	// setting, and Godot reads that section itself rather than asking this plugin. Rewriting the
	// file for the length of this export, and putting it back in _export_end, is the only lever an
	// EditorExportPlugin has over a dependency Godot itself declared.
	// The preset's reading, not ProjectSettings', so the `.web` override VerseRuntime registers (vm)
	// applies to a Web export made from a Windows editor.
	const Ref<EditorExportPreset> preset = get_export_preset();
	const String backend = String(preset.is_valid()
					? preset->get_project_setting("verse/runtime/backend")
					: ProjectSettings::get_singleton()->get_setting("verse/runtime/backend", String("host")))
								   .strip_edges();
	if (p_features.has("web") && backend != "vm") {
		refused = true;
		say(EditorExportPlatform::EXPORT_MESSAGE_ERROR,
				String("Verse needs the vm backend on Web, and this preset reads verse/runtime/backend as \"") + backend +
						String("\": the UE host is a native DLL a browser cannot load. Set verse/runtime/backend.web to \"vm\" in Project Settings, or remove the override that changed it."));
		return;
	}
	if (backend == "vm") {
		const String gdextension_path = String("res://addons/godot-verse/godot-verse.gdextension");
		Ref<FileAccess> reader = FileAccess::open(gdextension_path, FileAccess::READ);
		if (reader.is_valid()) {
			const String original = reader->get_as_text();
			reader.unref();
			const String stripped = without_gdextension_dependencies(original);
			if (stripped.length() != original.length()) {
				Ref<FileAccess> writer = FileAccess::open(gdextension_path, FileAccess::WRITE);
				if (writer.is_valid()) {
					writer->store_string(stripped);
					writer.unref();
					rewritten_gdextension_path = gdextension_path;
					rewritten_gdextension_original = original;
				} else {
					say(EditorExportPlatform::EXPORT_MESSAGE_WARNING,
							String("Could not rewrite ") + gdextension_path + String(" to drop the host DLL for the vm backend; the export will carry it anyway."));
				}
			}
		}
	}

	for (const char *platform : UNREACHABLE_PLATFORMS) {
		if (p_features.has(String(platform))) {
			refused = true;
			say(EditorExportPlatform::EXPORT_MESSAGE_ERROR,
					String("Verse does not export to ") + String(platform) +
							String(" yet, and this project has Verse scripts in it. "
								   "See docs/phase-7-design.md §14."));
			return;
		}
	}

	const String platform_tag = verse_paths::platform_tag(p_features);
	if (platform_tag.is_empty()) {
		refused = true;
		say(EditorExportPlatform::EXPORT_MESSAGE_ERROR,
				"Verse cannot tell which platform this export is for; no platform feature tag was set.");
		return;
	}

	// The editor's own build first, so a project that does not compile fails with diagnostics the
	// author has already seen in the script editor, against res:// paths, rather than with the
	// cooker's copy of them. The cook then compiles a second time, on purpose (§14).
	if (language->build_project() != OK) {
		refused = true;
		say(EditorExportPlatform::EXPORT_MESSAGE_ERROR,
				"The Verse project did not compile, so nothing was cooked. Fix the errors in the "
				"Output panel and export again.");
		return;
	}

	const String cooker = verse_host_paths::cooker_exe();
	if (cooker.is_empty() || !FileAccess::file_exists(cooker)) {
		refused = true;
		say(EditorExportPlatform::EXPORT_MESSAGE_ERROR,
				String("The Verse cooker is not at ") + (cooker.is_empty() ? String("<unset>") : cooker) +
						String(". Build it with `python tools/build_host.py --target VerseHostCooker`, "
							   "or set Editor Settings > Verse > Host > Cooker Path to it."));
		return;
	}

	const String app = ProjectSettings::get_singleton()->get_setting("application/config/name", String("godot"));
	const String cache_name = verse_paths::cache_dir_name(app, platform_tag, verse_paths::arch_tag(p_features));

	// The qualified name is the *parent* and `verse_data` the leaf, because add_shared_object ships
	// a directory under its own name and takes no rename (editor_export_platform_pc.cpp:244). That
	// keeps one cook per project per platform in the cache and a plain `verse_data` in the game.
	const String work = OS::get_singleton()->get_cache_dir().path_join("verse_cook")
								.path_join(cache_name)
								.path_join(verse_paths::DATA_DIR_NAME);
	DirAccess::make_dir_recursive_absolute(work);
	temp_dir = work;

	// One source per line: absolute path, module path, res:// path. Absolute because the ABI
	// carries only absolute paths (phase-0-spikes.md §S-3); the res:// path is what turns a
	// cooker diagnostic back into something the author recognises.
	ProjectSettings *settings = ProjectSettings::get_singleton();
	PackedStringArray lines;
	for (int64_t i = 0; i < sources.size(); i++) {
		lines.push_back(settings->globalize_path(sources[i]) + String("\t") +
				language->module_for_script(sources[i]) + String("\t") + sources[i]);
	}
	// Beside the work directory and not in it: the plugin ships that directory whole, and this
	// file is a list of absolute paths on the author's machine (R-DIST-11).
	const String manifest = work + String(".sources.txt");
	{
		Ref<FileAccess> file = FileAccess::open(manifest, FileAccess::WRITE);
		if (file.is_null()) {
			refused = true;
			temp_dir = String();
			say(EditorExportPlatform::EXPORT_MESSAGE_ERROR, String("Could not write ") + manifest);
			return;
		}
		file->store_string(String("\n").join(lines) + String("\n"));
	}

	// The bindings package (R-INT-11). The cooker cannot generate it -- its classes come from
	// ClassDB and from Godot's global class list, and `verse_cook.exe` has no Godot -- so what the
	// editor generated is written out and named on the command line. Beside the work directory for
	// the manifest's reason: the plugin ships that directory whole and this is not part of a game.
	const VerseBindings bindings = verse_generate_bindings();
	String bindings_path;
	if (!bindings.source.empty()) {
		bindings_path = work + String(".bindings.verse");
		Ref<FileAccess> file = FileAccess::open(bindings_path, FileAccess::WRITE);
		if (file.is_null()) {
			refused = true;
			temp_dir = String();
			say(EditorExportPlatform::EXPORT_MESSAGE_ERROR, String("Could not write ") + bindings_path);
			return;
		}
		file->store_string(String(bindings.source.c_str()));
	}

	// And the table that keys it, which is not recoverable from the Verse: a binding's class name
	// says nothing about whether it stands for a ClassDB class or for a script's `class_name`, and
	// those are matched against two different callbacks. The cooker writes these rows into the
	// sidecar, which is how an exported game keys a crossing handle on a binding (R-INT-11).
	//
	// One row per line, `verse<TAB>godot<TAB>script`, with exactly one of the last two filled: a
	// format with no quoting rules, over three identifiers that can carry none of the characters
	// that would need them.
	String table_path;
	if (!bindings.classes.empty()) {
		PackedStringArray rows;
		for (const VerseBindingClass &binding : bindings.classes) {
			rows.push_back(String(binding.verse_class.c_str()) + String("\t") +
					String(binding.godot_class.c_str()) + String("\t") +
					String(binding.script_class.c_str()));
		}
		table_path = work + String(".bindings.tsv");
		Ref<FileAccess> file = FileAccess::open(table_path, FileAccess::WRITE);
		if (file.is_null()) {
			refused = true;
			temp_dir = String();
			say(EditorExportPlatform::EXPORT_MESSAGE_ERROR, String("Could not write ") + table_path);
			return;
		}
		file->store_string(String("\n").join(rows) + String("\n"));
	}

	PackedStringArray args;
	args.push_back(manifest);
	args.push_back(work);
	if (!bindings_path.is_empty()) {
		args.push_back(String("-bindings=") + bindings_path);
	}
	if (!table_path.is_empty()) {
		args.push_back(String("-binding-classes=") + table_path);
	}

	Array output;
	const int32_t status = OS::get_singleton()->execute(cooker, args, output, /*read_stderr*/ true);

	// Relayed line by line, and by what the line says about itself: the cooker prints
	// `<res path>:<line>:<col>: error: ...`, which is the shape every compiler prints and the
	// shape the export dialog's own list reads best.
	for (int64_t i = 0; i < output.size(); i++) {
		const PackedStringArray printed = String(output[i]).split("\n", false);
		for (int64_t j = 0; j < printed.size(); j++) {
			const String line = printed[j].strip_edges();
			if (line.is_empty()) {
				continue;
			}
			int type = EditorExportPlatform::EXPORT_MESSAGE_INFO;
			if (line.contains("error:")) {
				type = EditorExportPlatform::EXPORT_MESSAGE_ERROR;
			} else if (line.contains("warning:")) {
				type = EditorExportPlatform::EXPORT_MESSAGE_WARNING;
			}
			say(type, line);
		}
	}

	if (status != 0) {
		refused = true;
		temp_dir = String();
		say(EditorExportPlatform::EXPORT_MESSAGE_ERROR,
				String("verse_cook exited ") + String::num_int64(status) +
						String("; the export carries no Verse. To see the engine's own log, run it by hand: \"") +
						cooker + String("\" \"") + manifest + String("\" \"") + work + String("\" --verbose"));
		return;
	}

	// The markers name half of every class, and the cook just compiled the project with them, so a
	// pack without them looks every script up under the wrong module and attaches none -- silently.
	// A preset whose non-resource filter lists `*.vmodule` already carries them, and adding one
	// twice would ship it twice, so only the rest are added.
	const String include_filter = preset.is_valid() ? preset->get_include_filter() : String();
	PackedStringArray markers;
	collect_vmodules("res://", markers);
	for (int64_t i = 0; i < markers.size(); i++) {
		if (!include_filter_matches(include_filter, markers[i])) {
			add_file(markers[i], FileAccess::get_file_as_bytes(markers[i]), false);
		}
	}

	if (platform_tag == "web") {
		add_directory_to_pack(this, work, String("res://").path_join(verse_paths::DATA_DIR_NAME));
	} else {
		// A directory, copied recursively beside the executable after the PCK
		// (editor_export_platform_pc.cpp:230-256). On macOS it goes inside the bundle instead, which
		// is where godotsharp_dirs.cpp looks for .NET's.
		add_shared_object(work, PackedStringArray(), platform_tag == "macos" ? String("Contents/Resources") : String());
	}
}

void VerseExportPlugin::_export_file(const String &p_path, const String &p_type, const PackedStringArray &p_features) {
	// Unconditional, including on an export _export_begin refused: add_message(EXPORT_MESSAGE_ERROR)
	// reports but does not abort (measured, §2 S-7 -- the export finishes, exit code 0, "completed
	// with warnings"), so a refused export still produces a game, and one that leaks its sources
	// because the cook failed would be the worse of the two outcomes. What a refusal withholds is
	// the data directory, which is what makes the game say so at load rather than run wrong.
	if (p_path.ends_with(".verse")) {
		// A one-byte stub, so `ext_resource type="Script" path="res://player.verse"` still
		// resolves and the source does not ship (R-DIST-11). The runtime host has the class
		// already; VerseScript reads the file only to find out that it is a script.
		PackedByteArray stub;
		stub.push_back('\n');
		add_file(p_path, stub, false);
		skip();
		return;
	}

	// A `.vmodule` is deliberately *not* touched here: _export_begin adds the ones the preset's filter
	// does not carry. Re-adding one here with add_file and then skip() takes it back out again:
	// measured, and it cost the export layer two missing markers.
}

void VerseExportPlugin::_export_end() {
	if (!temp_dir.is_empty()) {
		DirAccess::remove_absolute(temp_dir);
		temp_dir = String();
	}
	if (!rewritten_gdextension_path.is_empty()) {
		Ref<FileAccess> writer = FileAccess::open(rewritten_gdextension_path, FileAccess::WRITE);
		if (writer.is_valid()) {
			writer->store_string(rewritten_gdextension_original);
		}
		rewritten_gdextension_path = String();
		rewritten_gdextension_original = String();
	}
	refused = false;
}
