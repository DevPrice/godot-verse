#include "verse_runtime.h"

#include "verse_value.h"

#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/classes/window.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/char_string.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/node_path.hpp>
#include <godot_cpp/variant/string_name.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

using namespace godot;

void VerseRuntime::_bind_methods() {
	ClassDB::bind_method(D_METHOD("load_host"), static_cast<Error (VerseRuntime::*)()>(&VerseRuntime::load_host));
	ClassDB::bind_method(D_METHOD("load_host_from_path", "dll_path"), static_cast<Error (VerseRuntime::*)(const String &)>(&VerseRuntime::load_host));
	ClassDB::bind_method(D_METHOD("unload_host"), &VerseRuntime::unload_host);
	ClassDB::bind_method(D_METHOD("is_host_loaded"), &VerseRuntime::is_host_loaded);
	ClassDB::bind_method(D_METHOD("compile_file", "path"), &VerseRuntime::compile_file);
	ClassDB::bind_method(D_METHOD("run_main", "path"), &VerseRuntime::run_main);
	ClassDB::bind_method(D_METHOD("release_script"), &VerseRuntime::release_script);
	ClassDB::bind_method(D_METHOD("script_has_function", "decorated_name"), &VerseRuntime::script_has_function);
	ClassDB::bind_method(D_METHOD("call_void", "decorated_name"), &VerseRuntime::call_void);
	ClassDB::bind_method(D_METHOD("call_void_float", "decorated_name", "arg"), &VerseRuntime::call_void_float);
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
	return load_host_internal(p_dll_path, String());
}

Error VerseRuntime::load_host() {
	ProjectSettings *settings = ProjectSettings::get_singleton();

	const String dll_setting_name = "verse/host/dll_path";
	// The host must be loaded from the engine's own Binaries/Win64: VNI records each Verse
	// package's source directory relative to the loaded module, and the compiler reads those
	// .verse files at runtime. A copy anywhere else compiles against an empty package set.
	const String dll_default = "C:/UnrealEngine/Engine/Binaries/Win64/verse_host.dll";
	if (!settings->has_setting(dll_setting_name)) {
		settings->set_setting(dll_setting_name, dll_default);
	}
	settings->set_initial_value(dll_setting_name, dll_default);
	Dictionary dll_property_info;
	dll_property_info["name"] = dll_setting_name;
	dll_property_info["type"] = (int64_t)Variant::STRING;
	dll_property_info["hint"] = (int64_t)PROPERTY_HINT_NONE;
	dll_property_info["hint_string"] = String();
	settings->add_property_info(dll_property_info);

	const String engine_setting_name = "verse/host/engine_dir";
	const String engine_default = "C:/UnrealEngine/Engine";
	if (!settings->has_setting(engine_setting_name)) {
		settings->set_setting(engine_setting_name, engine_default);
	}
	settings->set_initial_value(engine_setting_name, engine_default);
	Dictionary engine_property_info;
	engine_property_info["name"] = engine_setting_name;
	engine_property_info["type"] = (int64_t)Variant::STRING;
	engine_property_info["hint"] = (int64_t)PROPERTY_HINT_NONE;
	engine_property_info["hint_string"] = String();
	settings->add_property_info(engine_property_info);

	const String dll_path = settings->globalize_path(settings->get_setting(dll_setting_name));
	const String engine_dir = settings->globalize_path(settings->get_setting(engine_setting_name));

	return load_host_internal(dll_path, engine_dir);
}

Error VerseRuntime::load_host_internal(const String &p_dll_path, const String &p_engine_dir) {
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
	godot_api.GetMeta = &VerseRuntime::api_get_meta;

	// EngineDirUtf8 only needs to stay alive for the duration of host.Init below.
	const CharString engine_dir_utf8 = p_engine_dir.is_empty() ? CharString() : p_engine_dir.utf8();

	init_desc = vh_init_desc{};
	init_desc.StructSize = sizeof(vh_init_desc);
	init_desc.AbiVersion = VH_ABI_VERSION;
	init_desc.EngineDirUtf8 = p_engine_dir.is_empty() ? nullptr : engine_dir_utf8.get_data();
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

void VerseRuntime::release_script() {
	release_current_script();
}

bool VerseRuntime::script_has_function(const String &p_decorated_name) const {
	if (!host.is_loaded() || current_script == nullptr) {
		return false;
	}
	const CharString name_utf8 = p_decorated_name.utf8();
	return host.ScriptHasFunction(current_script, name_utf8.get_data()) != 0;
}

Error VerseRuntime::call_void(const String &p_decorated_name) {
	if (!host.is_loaded() || current_script == nullptr) {
		UtilityFunctions::push_warning("VerseRuntime: call_void called with no script loaded");
		return ERR_UNAVAILABLE;
	}

	const CharString name_utf8 = p_decorated_name.utf8();
	const int32_t status = host.CallVoid(current_script, name_utf8.get_data());
	if (status != VH_OK) {
		UtilityFunctions::push_error(String("VerseRuntime: vh_call_void failed with status ") + String::num_int64(status));
		return FAILED;
	}
	return OK;
}

Error VerseRuntime::call_void_float(const String &p_decorated_name, double p_arg) {
	if (!host.is_loaded() || current_script == nullptr) {
		UtilityFunctions::push_warning("VerseRuntime: call_void_float called with no script loaded");
		return ERR_UNAVAILABLE;
	}

	const CharString name_utf8 = p_decorated_name.utf8();
	const int32_t status = host.CallVoidFloat(current_script, name_utf8.get_data(), p_arg);
	if (status != VH_OK) {
		UtilityFunctions::push_error(String("VerseRuntime: vh_call_void_float failed with status ") + String::num_int64(status));
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
	SceneTree *tree = Object::cast_to<SceneTree>(Engine::get_singleton()->get_main_loop());
	if (tree == nullptr) {
		return 0;
	}
	Node *root = tree->get_root();
	if (root == nullptr) {
		return 0;
	}

	const NodePath path(String::utf8(p_path_utf8, p_path_len));
	Node *node = root->get_node_or_null(path);
	if (node == nullptr) {
		return 0;
	}
	return node->get_instance_id();
}

vh_bool VerseRuntime::api_is_valid(void *p_ctx, vh_handle p_handle) {
	return UtilityFunctions::is_instance_id_valid(p_handle) ? 1 : 0;
}

vh_bool VerseRuntime::api_get_property(void *p_ctx, vh_handle p_handle, const char *p_name_utf8, int32_t p_name_len, vh_arena *p_arena, vh_value *r_value) {
	if (r_value == nullptr) {
		return 0;
	}
	Object *obj = UtilityFunctions::instance_from_id(p_handle);
	if (obj == nullptr) {
		return 0;
	}

	const StringName name(String::utf8(p_name_utf8, p_name_len));
	const Variant value = obj->get(name);
	if (value.get_type() == Variant::NIL) {
		// obj->get has no "does this property exist" signal of its own; NIL is the only miss
		// indicator available, so a genuinely nil property also reads as absent.
		return 0;
	}

	return variant_to_vh(value, p_arena, *r_value) ? 1 : 0;
}

vh_bool VerseRuntime::api_set_property(void *p_ctx, vh_handle p_handle, const char *p_name_utf8, int32_t p_name_len, const vh_value *p_value) {
	if (p_value == nullptr) {
		return 0;
	}
	Object *obj = UtilityFunctions::instance_from_id(p_handle);
	if (obj == nullptr) {
		return 0;
	}

	const StringName name(String::utf8(p_name_utf8, p_name_len));
	obj->set(name, vh_to_variant(*p_value));
	return 1;
}

vh_bool VerseRuntime::api_call_method(void *p_ctx, vh_handle p_handle, const char *p_name_utf8, int32_t p_name_len, const vh_value *p_args, int32_t p_arg_count, vh_arena *p_arena, vh_value *r_value) {
	Object *obj = UtilityFunctions::instance_from_id(p_handle);
	if (obj == nullptr) {
		return 0;
	}

	const StringName name(String::utf8(p_name_utf8, p_name_len));
	Array args;
	for (int32_t i = 0; i < p_arg_count; i++) {
		args.push_back(vh_to_variant(p_args[i]));
	}
	const Variant result = obj->callv(name, args);

	if (r_value == nullptr) {
		return 1;
	}
	return variant_to_vh(result, p_arena, *r_value) ? 1 : 0;
}

int32_t VerseRuntime::api_get_child_count(void *p_ctx, vh_handle p_handle) {
	Node *node = Object::cast_to<Node>(UtilityFunctions::instance_from_id(p_handle));
	if (node == nullptr) {
		return 0;
	}
	return node->get_child_count();
}

vh_handle VerseRuntime::api_get_child(void *p_ctx, vh_handle p_handle, int32_t p_index) {
	Node *node = Object::cast_to<Node>(UtilityFunctions::instance_from_id(p_handle));
	if (node == nullptr || p_index < 0 || p_index >= node->get_child_count()) {
		return 0;
	}
	Node *child = node->get_child(p_index);
	if (child == nullptr) {
		return 0;
	}
	return child->get_instance_id();
}

vh_bool VerseRuntime::api_get_meta(void *p_ctx, vh_handle p_handle, vh_arena *p_arena, vh_value *r_value) {
	if (r_value == nullptr) {
		return 0;
	}
	Object *obj = UtilityFunctions::instance_from_id(p_handle);
	if (obj == nullptr) {
		return 0;
	}

	Dictionary meta;
	Node *node = Object::cast_to<Node>(obj);
	if (node != nullptr) {
		meta["name"] = String(node->get_name());
		meta["class"] = node->get_class();
		meta["path"] = String(node->get_path());
	} else {
		meta["name"] = String();
		meta["class"] = obj->get_class();
		meta["path"] = String();
	}

	return variant_to_vh(meta, p_arena, *r_value) ? 1 : 0;
}

void VerseRuntime::on_diagnostic(void *p_ctx, const vh_diagnostic *p_diagnostic) {
	const String file = p_diagnostic->FilePathLen > 0 ? String::utf8(p_diagnostic->FilePathUtf8, p_diagnostic->FilePathLen) : String("<unknown>");
	const String message = String::utf8(p_diagnostic->MessageUtf8, p_diagnostic->MessageLen);
	const String formatted = file + String(":") + String::num_int64(p_diagnostic->Line) + String(":") + String::num_int64(p_diagnostic->Column) + String(": ") + message;

	if (p_diagnostic->Severity == VH_SEVERITY_ERROR) {
		UtilityFunctions::push_error(formatted);
	} else if (p_diagnostic->Severity == VH_SEVERITY_WARNING) {
		UtilityFunctions::push_warning(formatted);
	} else {
		UtilityFunctions::print(formatted);
	}
}
