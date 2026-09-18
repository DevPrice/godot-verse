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

	/// Whether the calling thread is inside one of these loads.
	///
	/// `ResourceLoader` answers ERR_BUSY to a load of something already being loaded further up the
	/// same thread's stack, and says nothing about why (`resource_loader.cpp:1049-1056`) -- the
	/// caller's own `ERR_FAIL_COND_V_MSG` is the only thing printed. A `.verse` load builds the
	/// project, a build generates the bindings, and generating them loads every `class_name`
	/// script, so a GDScript that names a Verse class reaches itself: `main.gd` -> `mover.verse` ->
	/// build -> `main.gd`. Nothing but this can tell the generator it is on that stack.
	static bool is_loading();
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
