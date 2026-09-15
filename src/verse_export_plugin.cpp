#include "verse_export_plugin.h"

#include "verse_export_paths.h"
#include "verse_host_paths.h"
#include "verse_script_language.h"

#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/editor_export_platform.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

using namespace godot;

namespace {

// The platforms this bridge does not reach. Mobile is deferred with no design and web is Phase
// 7.5; both fail here rather than producing a game that cannot load its own scripts (R-PLAT-4).
const char *UNREACHABLE_PLATFORMS[] = { "android", "ios", "web" };

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

	PackedStringArray args;
	args.push_back(manifest);
	args.push_back(work);

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

	// A directory, copied recursively beside the executable after the PCK
	// (editor_export_platform_pc.cpp:230-256). On macOS it goes inside the bundle instead, which
	// is where godotsharp_dirs.cpp looks for .NET's.
	add_shared_object(work, PackedStringArray(), platform_tag == "macos" ? String("Contents/Resources") : String());
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

	// A `.vmodule` is deliberately *not* touched here. The markers decide which module each script
	// is in and so half of every class's name, so they have to ship -- but what ships them is the
	// preset's `include_filter="*.vmodule"`, which carries them as plain files. Re-adding one with
	// add_file and then skip() takes it back out again: measured, and it cost the export layer two
	// missing markers.
}

void VerseExportPlugin::_export_end() {
	if (!temp_dir.is_empty()) {
		DirAccess::remove_absolute(temp_dir);
		temp_dir = String();
	}
	refused = false;
}
