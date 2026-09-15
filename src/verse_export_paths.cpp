#include "verse_export_paths.h"

#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/classes/project_settings.hpp>

using namespace godot;

namespace {

// Godot's platform identifiers, which are also the feature tags an export carries. Ordered as
// EditorExportPlatform registers them; the first match wins, and they are mutually exclusive.
const char *PLATFORM_TAGS[] = { "windows", "linuxbsd", "macos", "android", "ios", "web" };

// Every architecture Godot has a tag for. "universal" is not here: a macOS universal build
// carries no architecture tag at all, and an empty arch is how data_dir_name spells that.
const char *ARCH_TAGS[] = { "x86_64", "x86_32", "arm64", "arm32", "rv64", "ppc64", "ppc32", "loongarch64", "wasm32" };

String first_tag_present(const PackedStringArray &p_features, const char *const *p_tags, int p_count) {
	for (int i = 0; i < p_count; i++) {
		if (p_features.has(String(p_tags[i]))) {
			return String(p_tags[i]);
		}
	}
	return String();
}

PackedStringArray tags_of_this_build() {
	PackedStringArray present;
	OS *os = OS::get_singleton();
	for (const char *tag : PLATFORM_TAGS) {
		if (os->has_feature(tag)) {
			present.push_back(String(tag));
		}
	}
	for (const char *tag : ARCH_TAGS) {
		if (os->has_feature(tag)) {
			present.push_back(String(tag));
		}
	}
	return present;
}

// OS::get_safe_dir_name's rule, reimplemented because it is not bound for extensions: every
// character a filesystem could object to becomes an underscore, and the result is trimmed.
String safe_dir_name(const String &p_name) {
	const String invalid = ": * ? \" < > | \\ / . ,";
	String safe = p_name.strip_edges();
	const PackedStringArray parts = invalid.split(" ");
	for (int i = 0; i < parts.size(); i++) {
		safe = safe.replace(parts[i], "_");
	}
	return safe;
}

} // namespace

String verse_paths::platform_tag(const PackedStringArray &p_features) {
	return first_tag_present(p_features, PLATFORM_TAGS, (int)(sizeof(PLATFORM_TAGS) / sizeof(PLATFORM_TAGS[0])));
}

String verse_paths::arch_tag(const PackedStringArray &p_features) {
	return first_tag_present(p_features, ARCH_TAGS, (int)(sizeof(ARCH_TAGS) / sizeof(ARCH_TAGS[0])));
}

String verse_paths::data_dir_name(const String &p_app, const String &p_platform, const String &p_arch) {
	String name = String("verse_") + safe_dir_name(p_app) + String("_") + p_platform;
	if (!p_arch.is_empty()) {
		name += String("_") + p_arch;
	}
	return name;
}

String verse_paths::data_dir_name_for_this_build() {
	if (!OS::get_singleton()->has_feature("template")) {
		return String();
	}
	const PackedStringArray features = tags_of_this_build();
	const String app = ProjectSettings::get_singleton()->get_setting("application/config/name", String("godot"));
	return data_dir_name(app, platform_tag(features), arch_tag(features));
}

String verse_paths::data_dir_for_this_build() {
	const String name = data_dir_name_for_this_build();
	if (name.is_empty()) {
		return String();
	}
	// On macOS the executable is inside Contents/MacOS and the directory is exported into
	// Contents/Resources, so this is not the answer there -- but OS::get_bundle_resource_dir is
	// not bound for extensions, and macOS is blocked on hardware anyway (phase-7-design.md §10).
	return OS::get_singleton()->get_executable_path().get_base_dir().path_join(name);
}
