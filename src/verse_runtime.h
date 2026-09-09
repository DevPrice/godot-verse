#pragma once

#include "verse_host.h"
#include "verse_host_abi.h"

#include <godot_cpp/classes/global_constants.hpp>
#include <godot_cpp/classes/object.hpp>
#include <godot_cpp/variant/string.hpp>

// The "VerseRuntime" engine singleton. Owns the verse_host.dll loader, the vh_init_desc handed
// to it, and the last compiled/run vh_script. Every method degrades to ERR_UNAVAILABLE plus a
// warning when no host is loaded; nothing here may crash for that reason.
class VerseRuntime : public godot::Object {
	GDCLASS(VerseRuntime, godot::Object)

protected:
	static void _bind_methods();

public:
	VerseRuntime() = default;
	~VerseRuntime() override;

	// Reads verse/host/dll_path and verse/host/engine_dir from ProjectSettings (creating them
	// with defaults if absent) and loads the host from there.
	godot::Error load_host();
	godot::Error load_host(const godot::String &p_dll_path);
	void unload_host();
	bool is_host_loaded() const;

	godot::Error compile_file(const godot::String &p_path);
	godot::Error run_main(const godot::String &p_path);
	void release_script();

	bool script_has_function(const godot::String &p_decorated_name) const;
	godot::Error call_void(const godot::String &p_decorated_name);
	godot::Error call_void_float(const godot::String &p_decorated_name, double p_arg);

	void tick(double p_budget_seconds);

private:
	VerseHostLibrary host;
	vh_init_desc init_desc = {};
	vh_godot_api godot_api = {};
	vh_script *current_script = nullptr;

	godot::Error load_host_internal(const godot::String &p_dll_path, const godot::String &p_engine_dir);
	void release_current_script();

	static void api_print(void *p_ctx, const char *p_utf8, int32_t p_len);
	static vh_handle api_get_node(void *p_ctx, const char *p_path_utf8, int32_t p_path_len);
	static vh_bool api_is_valid(void *p_ctx, vh_handle p_handle);
	static vh_bool api_get_property(void *p_ctx, vh_handle p_handle, const char *p_name_utf8, int32_t p_name_len, vh_arena *p_arena, vh_value *r_value);
	static vh_bool api_set_property(void *p_ctx, vh_handle p_handle, const char *p_name_utf8, int32_t p_name_len, const vh_value *p_value);
	static vh_bool api_call_method(void *p_ctx, vh_handle p_handle, const char *p_name_utf8, int32_t p_name_len, const vh_value *p_args, int32_t p_arg_count, vh_arena *p_arena, vh_value *r_value);
	static int32_t api_get_child_count(void *p_ctx, vh_handle p_handle);
	static vh_handle api_get_child(void *p_ctx, vh_handle p_handle, int32_t p_index);
	static vh_bool api_get_meta(void *p_ctx, vh_handle p_handle, vh_arena *p_arena, vh_value *r_value);

	static void on_diagnostic(void *p_ctx, const vh_diagnostic *p_diagnostic);
};
