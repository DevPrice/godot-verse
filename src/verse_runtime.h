#pragma once

#include "verse_host.h"
#include "verse_host_abi.h"

#include <godot_cpp/classes/global_constants.hpp>
#include <godot_cpp/classes/object.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/typed_array.hpp>

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

	// Reads verse/host/dll_path, verse/host/engine_dir and verse/host/enable_debugger from
	// ProjectSettings (creating them with defaults if absent) and loads the host from there.
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

	// Multi-script API, used by VerseScript. Each VerseScript owns one handle; the
	// single-script methods above stay for VerseTicker and the bound script API.
	//
	// Verse's compilation unit is the package, not the file, and the host can only build once
	// per process, so every .verse file in the project is compiled together.
	//
	// While r_diagnostics_by_path is non-null every diagnostic the host reports is filed under
	// its own source path as { line, column, message, path } instead of reaching the output log.
	godot::Error compile_project(const godot::PackedStringArray &p_globalized_paths, godot::Dictionary *r_diagnostics_by_path);
	vh_script *open_script(const godot::String &p_globalized_path);
	void release_script_handle(vh_script *p_script);
	bool handle_has_function(vh_script *p_script, const char *p_decorated_name) const;
	godot::Error call_handle_void(vh_script *p_script, const char *p_decorated_name);
	godot::Error call_handle_void_float(vh_script *p_script, const char *p_decorated_name, double p_arg);

	// A script that defines a top-level class named after its file is driven through an instance
	// of that class rather than through the module's free functions. Returns null when the class
	// does not exist, which is how VerseScript tells the two shapes apart.
	bool has_class(const godot::String &p_class_name) const;
	vh_instance *instantiate(const godot::String &p_class_name, int64_t p_object_id);
	void release_instance(vh_instance *p_instance);
	bool instance_has_function(vh_instance *p_instance, const char *p_decorated_name) const;
	godot::Error call_instance_void(vh_instance *p_instance, const char *p_decorated_name);
	godot::Error call_instance_void_float(vh_instance *p_instance, const char *p_decorated_name, double p_arg);

private:
	VerseHostLibrary host;
	vh_init_desc init_desc = {};
	vh_godot_api godot_api = {};
	vh_script *current_script = nullptr;
	godot::Dictionary *diagnostic_sink = nullptr;

	godot::Error load_host_internal(const godot::String &p_dll_path, const godot::String &p_engine_dir, bool p_enable_debugger);
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
	static vh_handle api_instantiate(void *p_ctx, const char *p_class_name_utf8, int32_t p_class_name_len);
	static vh_handle api_get_singleton(void *p_ctx, const char *p_name_utf8, int32_t p_name_len);

	static void on_diagnostic(void *p_ctx, const vh_diagnostic *p_diagnostic);
};
