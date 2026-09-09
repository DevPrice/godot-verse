#pragma once

#include <godot_cpp/classes/resource_format_loader.hpp>
#include <godot_cpp/classes/resource_format_saver.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/string_name.hpp>

// Turns a .verse file on disk into a VerseScript resource. Without these Godot can open a
// .verse file in the text editor but cannot attach one to a node.
class VerseResourceFormatLoader : public godot::ResourceFormatLoader {
	GDCLASS(VerseResourceFormatLoader, godot::ResourceFormatLoader)

protected:
	static void _bind_methods() {}

public:
	godot::PackedStringArray _get_recognized_extensions() const override;
	bool _handles_type(const godot::StringName &p_type) const override;
	godot::String _get_resource_type(const godot::String &p_path) const override;
	godot::Variant _load(const godot::String &p_path, const godot::String &p_original_path, bool p_use_sub_threads, int32_t p_cache_mode) const override;
};

class VerseResourceFormatSaver : public godot::ResourceFormatSaver {
	GDCLASS(VerseResourceFormatSaver, godot::ResourceFormatSaver)

protected:
	static void _bind_methods() {}

public:
	godot::Error _save(const godot::Ref<godot::Resource> &p_resource, const godot::String &p_path, uint32_t p_flags) override;
	bool _recognize(const godot::Ref<godot::Resource> &p_resource) const override;
	godot::PackedStringArray _get_recognized_extensions(const godot::Ref<godot::Resource> &p_resource) const override;
};
