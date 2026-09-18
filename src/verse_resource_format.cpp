#include "verse_resource_format.h"

#include "verse_script.h"

#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/object.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/memory.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string_name.hpp>

using namespace godot;

namespace {

// Per thread, because that is the scope ResourceLoader's own cyclic-load detection uses: a load on
// another thread is not this thread's cycle and must not be made to look like one.
thread_local int load_depth = 0;

struct LoadDepthScope {
	LoadDepthScope() { load_depth++; }
	~LoadDepthScope() { load_depth--; }
};

} // namespace

bool VerseResourceFormatLoader::is_loading() {
	return load_depth > 0;
}

PackedStringArray VerseResourceFormatLoader::_get_recognized_extensions() const {
	PackedStringArray extensions;
	extensions.push_back("verse");
	return extensions;
}

bool VerseResourceFormatLoader::_handles_type(const StringName &p_type) const {
	return p_type == StringName("Script") || p_type == StringName("VerseScript");
}

String VerseResourceFormatLoader::_get_resource_type(const String &p_path) const {
	if (p_path.get_extension().to_lower() == "verse") {
		return "VerseScript";
	}
	return String();
}

Variant VerseResourceFormatLoader::_load(const String &p_path, const String &p_original_path, bool p_use_sub_threads, int32_t p_cache_mode) const {
	const LoadDepthScope in_load;

	const String text = FileAccess::get_file_as_string(p_path);
	if (FileAccess::get_open_error() != OK) {
		return ERR_FILE_CANT_OPEN;
	}

	Ref<VerseScript> script = memnew(VerseScript);
	script->set_path(p_original_path.is_empty() ? p_path : p_original_path);
	script->_set_source_code(text);
	// A failed compile must not fail the load: the file still has to open in the editor so
	// the author can fix it. Diagnostics went to VerseScriptLanguage during compile().
	script->compile();

	return script;
}

Error VerseResourceFormatSaver::_save(const Ref<Resource> &p_resource, const String &p_path, uint32_t p_flags) {
	VerseScript *script = Object::cast_to<VerseScript>(p_resource.ptr());
	if (script == nullptr) {
		return ERR_INVALID_PARAMETER;
	}

	Ref<FileAccess> file = FileAccess::open(p_path, FileAccess::WRITE);
	if (file.is_null()) {
		return ERR_CANT_OPEN;
	}
	file->store_string(script->_get_source_code());
	file->close();

	script->_reload(false);
	return OK;
}

bool VerseResourceFormatSaver::_recognize(const Ref<Resource> &p_resource) const {
	return Object::cast_to<VerseScript>(p_resource.ptr()) != nullptr;
}

PackedStringArray VerseResourceFormatSaver::_get_recognized_extensions(const Ref<Resource> &p_resource) const {
	PackedStringArray extensions;
	if (_recognize(p_resource)) {
		extensions.push_back("verse");
	}
	return extensions;
}
