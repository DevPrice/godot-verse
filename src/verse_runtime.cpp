#include "verse_runtime.h"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/char_string.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

using namespace godot;

void VerseRuntime::_bind_methods() {
	ClassDB::bind_method(D_METHOD("load_host", "dll_path"), &VerseRuntime::load_host);
	ClassDB::bind_method(D_METHOD("unload_host"), &VerseRuntime::unload_host);
	ClassDB::bind_method(D_METHOD("is_host_loaded"), &VerseRuntime::is_host_loaded);
	ClassDB::bind_method(D_METHOD("compile_file", "path"), &VerseRuntime::compile_file);
	ClassDB::bind_method(D_METHOD("run_main", "path"), &VerseRuntime::run_main);
	ClassDB::bind_method(D_METHOD("tick", "budget_seconds"), &VerseRuntime::tick);
}

VerseRuntime::~VerseRuntime() {
	unload_host();
}

void VerseRuntime::release_current_script() {
	if (current_script != nullptr && host.ReleaseScript != nullptr) {
		host.ReleaseScript(current_script);
	}
	current_script = nullptr;
}

Error VerseRuntime::load_host(const String &p_dll_path) {
	if (host.is_loaded()) {
		unload_host();
	}

	String error_message;
	if (!host.load(p_dll_path, error_message)) {
		UtilityFunctions::push_error(String("VerseRuntime: failed to load host library: ") + error_message);
		return ERR_CANT_OPEN;
	}

	godot_api = vh_godot_api{};
	godot_api.StructSize = sizeof(vh_godot_api);
	godot_api.Ctx = this;
	godot_api.Print = &VerseRuntime::api_print;
	godot_api.GetNode = &VerseRuntime::api_get_node;
	godot_api.IsValid = &VerseRuntime::api_is_valid;
	godot_api.GetProperty = &VerseRuntime::api_get_property;
	godot_api.SetProperty = &VerseRuntime::api_set_property;
	godot_api.CallMethod = &VerseRuntime::api_call_method;
	godot_api.GetChildCount = &VerseRuntime::api_get_child_count;
	godot_api.GetChild = &VerseRuntime::api_get_child;

	init_desc = vh_init_desc{};
	init_desc.StructSize = sizeof(vh_init_desc);
	init_desc.AbiVersion = VH_ABI_VERSION;
	init_desc.EngineDirUtf8 = nullptr;
	init_desc.Godot = godot_api;
	init_desc.OnDiagnostic = &VerseRuntime::on_diagnostic;
	init_desc.DiagnosticCtx = this;
	init_desc.EnableDebugger = 0;

	const int32_t status = host.Init(&init_desc);
	if (status != VH_OK) {
		UtilityFunctions::push_error(String("VerseRuntime: vh_init failed with status ") + String::num_int64(status));
		host.unload();
		return FAILED;
	}

	return OK;
}

void VerseRuntime::unload_host() {
	if (!host.is_loaded()) {
		return;
	}

	release_current_script();
	if (host.Shutdown != nullptr) {
		host.Shutdown();
	}
	host.unload();
}

bool VerseRuntime::is_host_loaded() const {
	return host.is_loaded();
}

Error VerseRuntime::compile_file(const String &p_path) {
	if (!host.is_loaded()) {
		UtilityFunctions::push_warning("VerseRuntime: compile_file called with no host loaded");
		return ERR_UNAVAILABLE;
	}

	release_current_script();

	const CharString utf8_path = p_path.utf8();
	const int32_t status = host.CompileFile(utf8_path.get_data(), &current_script);
	if (status != VH_OK) {
		UtilityFunctions::push_error(String("VerseRuntime: vh_compile_file failed with status ") + String::num_int64(status));
		return ERR_COMPILATION_FAILED;
	}

	return OK;
}

Error VerseRuntime::run_main(const String &p_path) {
	if (!host.is_loaded()) {
		UtilityFunctions::push_warning("VerseRuntime: run_main called with no host loaded");
		return ERR_UNAVAILABLE;
	}

	const Error compile_status = compile_file(p_path);
	if (compile_status != OK) {
		return compile_status;
	}

	int64_t exit_code = 0;
	const int32_t status = host.RunMain(current_script, nullptr, 0, &exit_code);
	if (status != VH_OK) {
		UtilityFunctions::push_error(String("VerseRuntime: vh_run_main failed with status ") + String::num_int64(status));
		return FAILED;
	}

	return OK;
}

void VerseRuntime::tick(double p_budget_seconds) {
	if (!host.is_loaded()) {
		UtilityFunctions::push_warning("VerseRuntime: tick called with no host loaded");
		return;
	}

	host.Tick(p_budget_seconds);
}

void VerseRuntime::api_print(void *p_ctx, const char *p_utf8, int32_t p_len) {
	UtilityFunctions::print(String::utf8(p_utf8, p_len));
}

vh_handle VerseRuntime::api_get_node(void *p_ctx, const char *p_path_utf8, int32_t p_path_len) {
	UtilityFunctions::push_warning("VerseRuntime: GetNode is not implemented yet");
	return 0;
}

vh_bool VerseRuntime::api_is_valid(void *p_ctx, vh_handle p_handle) {
	UtilityFunctions::push_warning("VerseRuntime: IsValid is not implemented yet");
	return 0;
}

vh_bool VerseRuntime::api_get_property(void *p_ctx, vh_handle p_handle, const char *p_name_utf8, int32_t p_name_len, vh_arena *p_arena, vh_value *r_value) {
	UtilityFunctions::push_warning("VerseRuntime: GetProperty is not implemented yet");
	return 0;
}

vh_bool VerseRuntime::api_set_property(void *p_ctx, vh_handle p_handle, const char *p_name_utf8, int32_t p_name_len, const vh_value *p_value) {
	UtilityFunctions::push_warning("VerseRuntime: SetProperty is not implemented yet");
	return 0;
}

vh_bool VerseRuntime::api_call_method(void *p_ctx, vh_handle p_handle, const char *p_name_utf8, int32_t p_name_len, const vh_value *p_args, int32_t p_arg_count, vh_arena *p_arena, vh_value *r_value) {
	UtilityFunctions::push_warning("VerseRuntime: CallMethod is not implemented yet");
	return 0;
}

int32_t VerseRuntime::api_get_child_count(void *p_ctx, vh_handle p_handle) {
	UtilityFunctions::push_warning("VerseRuntime: GetChildCount is not implemented yet");
	return 0;
}

vh_handle VerseRuntime::api_get_child(void *p_ctx, vh_handle p_handle, int32_t p_index) {
	UtilityFunctions::push_warning("VerseRuntime: GetChild is not implemented yet");
	return 0;
}

void VerseRuntime::on_diagnostic(void *p_ctx, const vh_diagnostic *p_diagnostic) {
	const String file = p_diagnostic->FilePathLen > 0 ? String::utf8(p_diagnostic->FilePathUtf8, p_diagnostic->FilePathLen) : String("<unknown>");
	const String message = String::utf8(p_diagnostic->MessageUtf8, p_diagnostic->MessageLen);
	const String formatted = file + String(":") + String::num_int64(p_diagnostic->Line) + String(":") + String::num_int64(p_diagnostic->Column) + String(": ") + message;

	if (p_diagnostic->Severity == VH_SEVERITY_ERROR) {
		UtilityFunctions::push_error(formatted);
	} else {
		UtilityFunctions::push_warning(formatted);
	}
}
