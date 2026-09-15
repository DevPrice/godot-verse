#include "verse_host_paths.h"

#include <godot_cpp/classes/editor_interface.hpp>
#include <godot_cpp/classes/editor_settings.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

using namespace godot;

namespace {

const char *ENGINE_SETTING = "verse/host/engine_dir";
const char *DLL_SETTING = "verse/host/dll_path";
const char *COOKER_SETTING = "verse/host/cooker_path";

#if defined(_WIN32)
const char *BINARIES_SUBDIR = "Engine/Binaries/Win64";
const char *HOST_DLL_NAME = "verse_host.dll";
const char *COOKER_NAME = "verse_cook.exe";
#elif defined(__APPLE__)
// Neither of the two below has ever been produced: Linux and macOS are out of Phase 7b by
// decision, and the host has only ever been built on Windows. They are here so that the Windows
// assumption is one line to correct rather than a path concatenated at three call sites
// (R-PLAT-5).
const char *BINARIES_SUBDIR = "Engine/Binaries/Mac";
const char *HOST_DLL_NAME = "libverse_host.dylib";
const char *COOKER_NAME = "verse_cook";
#else
const char *BINARIES_SUBDIR = "Engine/Binaries/Linux";
const char *HOST_DLL_NAME = "libverse_host.so";
const char *COOKER_NAME = "verse_cook";
#endif

String from_environment(const char *p_name) {
	OS *os = OS::get_singleton();
	if (os == nullptr || !os->has_environment(p_name)) {
		return String();
	}
	// Forward slashes, the way every other path in Godot is spelled: an environment variable set
	// from a shell carries whatever separator that shell used, and these get path_join'd.
	return os->get_environment(p_name).strip_edges().replace("\\", "/");
}

Ref<EditorSettings> editor_settings() {
	// is_editor_hint, not has_singleton: Engine registers the *name* EditorInterface whatever the
	// binary is doing and refuses the object outside the editor ("Can't retrieve singleton
	// 'EditorInterface' outside of editor", engine.cpp:344), so has_singleton answers true and
	// godot-cpp's ERR_FAIL_NULL_V then prints two errors per call. The editor binary running a
	// `-s` script is exactly that case and it is ordinary, not a fault.
	Engine *engine = Engine::get_singleton();
	if (engine == nullptr || !engine->is_editor_hint()) {
		return Ref<EditorSettings>();
	}
	EditorInterface *interface = EditorInterface::get_singleton();
	return interface == nullptr ? Ref<EditorSettings>() : interface->get_editor_settings();
}

String from_editor_settings(const char *p_name) {
	Ref<EditorSettings> settings = editor_settings();
	if (settings.is_null() || !settings->has_setting(p_name)) {
		return String();
	}
	return String(settings->get_setting(p_name)).strip_edges();
}

// The one store that can travel in a commit, which is why reading it says so. Warned once per
// process rather than per read: the three settings are asked for several times per build.
String from_project_settings(const char *p_name) {
	ProjectSettings *settings = ProjectSettings::get_singleton();
	if (settings == nullptr || !settings->has_setting(p_name)) {
		return String();
	}
	const String value = String(settings->get_setting(p_name)).strip_edges();
	if (value.is_empty()) {
		return String();
	}

	static bool warned = false;
	if (!warned) {
		warned = true;
		UtilityFunctions::push_warning(String("Verse: ") + p_name +
				String(" is set in project.godot, which commits one machine's paths to everyone "
					   "who clones this project. Move it to Editor Settings > Verse > Host, or set "
					   "UE_ROOT, and delete the [verse] section. It is still read for now."));
	}
	return settings->globalize_path(value);
}

// env, then the user's editor, then the project. The first non-empty answer wins, and nothing
// falls back past an answer that is set but wrong -- a path that does not exist is a diagnostic
// the caller gives, not a reason to go looking somewhere else.
String resolve(const char *p_env, const char *p_setting) {
	const String from_env = from_environment(p_env);
	if (!from_env.is_empty()) {
		return from_env;
	}
	const String from_editor = from_editor_settings(p_setting);
	if (!from_editor.is_empty()) {
		return from_editor;
	}
	return from_project_settings(p_setting);
}

void declare(const Ref<EditorSettings> &p_settings, const char *p_name, int64_t p_hint, const String &p_hint_string) {
	if (!p_settings->has_setting(p_name)) {
		p_settings->set_setting(p_name, String());
	}
	p_settings->set_initial_value(p_name, String(), false);
	Dictionary info;
	info["name"] = String(p_name);
	info["type"] = (int64_t)Variant::STRING;
	info["hint"] = p_hint;
	info["hint_string"] = p_hint_string;
	p_settings->add_property_info(info);
}

} // namespace

String verse_host_paths::engine_dir() {
	return resolve("UE_ROOT", ENGINE_SETTING);
}

String verse_host_paths::host_dll() {
	const String configured = resolve("VERSE_HOST_DLL", DLL_SETTING);
	if (!configured.is_empty()) {
		return configured;
	}
	const String root = engine_dir();
	if (root.is_empty()) {
		return String();
	}
	// The host must load from the engine's own Binaries directory: VNI records each Verse
	// package's source directory relative to the loaded module, and the compiler reads those
	// .verse files at runtime. A copy anywhere else compiles against an empty package set.
	return root.path_join(BINARIES_SUBDIR).path_join(HOST_DLL_NAME);
}

String verse_host_paths::cooker_exe() {
	const String configured = resolve("VERSE_COOKER", COOKER_SETTING);
	if (!configured.is_empty()) {
		return configured;
	}
	const String dll = host_dll();
	if (dll.is_empty()) {
		return String();
	}
	return dll.get_base_dir().path_join(COOKER_NAME);
}

String verse_host_paths::unconfigured_message() {
	return String("no Unreal checkout is configured, so there is no host to load. Set UE_ROOT to "
				  "the checkout that built ") +
			HOST_DLL_NAME +
			String(", or set Editor Settings > Verse > Host > Engine Dir to the same directory.");
}

void verse_host_paths::register_editor_settings() {
	Ref<EditorSettings> settings = editor_settings();
	if (settings.is_null()) {
		return;
	}
	declare(settings, ENGINE_SETTING, (int64_t)PROPERTY_HINT_GLOBAL_DIR, String());
	declare(settings, DLL_SETTING, (int64_t)PROPERTY_HINT_GLOBAL_FILE, String("*.dll,*.so,*.dylib"));
	declare(settings, COOKER_SETTING, (int64_t)PROPERTY_HINT_GLOBAL_FILE, String("*.exe,*"));
}
