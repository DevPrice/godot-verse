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

#include <vector>

using namespace godot;

void VerseRuntime::_bind_methods() {
	ClassDB::bind_method(D_METHOD("load_host"), static_cast<Error (VerseRuntime::*)()>(&VerseRuntime::load_host));
	ClassDB::bind_method(D_METHOD("load_host_from_path", "dll_path"), static_cast<Error (VerseRuntime::*)(const String &)>(&VerseRuntime::load_host));
	ClassDB::bind_method(D_METHOD("unload_host"), &VerseRuntime::unload_host);
	ClassDB::bind_method(D_METHOD("is_host_loaded"), &VerseRuntime::is_host_loaded);
	ClassDB::bind_method(D_METHOD("tick", "budget_seconds"), &VerseRuntime::tick);
}

VerseRuntime::~VerseRuntime() {
	unload_host();
}

Error VerseRuntime::load_host(const String &p_dll_path) {
	return load_host_internal(p_dll_path, String(), false);
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

	const String debugger_setting_name = "verse/host/enable_debugger";
	const bool debugger_default = false;
	if (!settings->has_setting(debugger_setting_name)) {
		settings->set_setting(debugger_setting_name, debugger_default);
	}
	settings->set_initial_value(debugger_setting_name, debugger_default);
	Dictionary debugger_property_info;
	debugger_property_info["name"] = debugger_setting_name;
	debugger_property_info["type"] = (int64_t)Variant::BOOL;
	debugger_property_info["hint"] = (int64_t)PROPERTY_HINT_NONE;
	debugger_property_info["hint_string"] = String();
	settings->add_property_info(debugger_property_info);

	const String dll_path = settings->globalize_path(settings->get_setting(dll_setting_name));
	const String engine_dir = settings->globalize_path(settings->get_setting(engine_setting_name));
	const bool enable_debugger = settings->get_setting(debugger_setting_name);

	return load_host_internal(dll_path, engine_dir, enable_debugger);
}

Error VerseRuntime::load_host_internal(const String &p_dll_path, const String &p_engine_dir, bool p_enable_debugger) {
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
	godot_api.Instantiate = &VerseRuntime::api_instantiate;
	godot_api.GetSingleton = &VerseRuntime::api_get_singleton;

	// EngineDirUtf8 only needs to stay alive for the duration of host.Init below.
	const CharString engine_dir_utf8 = p_engine_dir.is_empty() ? CharString() : p_engine_dir.utf8();

	init_desc = vh_init_desc{};
	init_desc.StructSize = sizeof(vh_init_desc);
	init_desc.AbiVersion = VH_ABI_VERSION;
	init_desc.EngineDirUtf8 = p_engine_dir.is_empty() ? nullptr : engine_dir_utf8.get_data();
	init_desc.Godot = godot_api;
	init_desc.OnDiagnostic = &VerseRuntime::on_diagnostic;
	init_desc.DiagnosticCtx = this;
	init_desc.EnableDebugger = p_enable_debugger ? 1 : 0;

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

	if (host.Shutdown != nullptr) {
		host.Shutdown();
	}
	host.unload();
}

bool VerseRuntime::is_host_loaded() const {
	return host.is_loaded();
}

Error VerseRuntime::compile_project(const PackedStringArray &p_globalized_paths, Dictionary *r_diagnostics_by_path) {
	if (!host.is_loaded()) {
		return ERR_UNAVAILABLE;
	}

	// The pointers handed to the host must outlive the call, so the CharStrings backing them
	// have to stay alive alongside the pointer array.
	std::vector<CharString> utf8_paths;
	std::vector<const char *> raw_paths;
	utf8_paths.reserve(p_globalized_paths.size());
	raw_paths.reserve(p_globalized_paths.size());
	for (int64_t i = 0; i < p_globalized_paths.size(); i++) {
		utf8_paths.push_back(p_globalized_paths[i].utf8());
		raw_paths.push_back(utf8_paths.back().get_data());
	}

	diagnostic_sink = r_diagnostics_by_path;
	const int32_t status = host.CompileProject(raw_paths.data(), (int32_t)raw_paths.size());
	diagnostic_sink = nullptr;

	return status == VH_OK ? OK : ERR_COMPILATION_FAILED;
}

Error VerseRuntime::check_project(const String &p_globalized_path, const String &p_source, Dictionary *r_diagnostics_by_path) {
	if (!host.is_loaded()) {
		return ERR_UNAVAILABLE;
	}

	const CharString path_utf8 = p_globalized_path.utf8();
	const CharString source_utf8 = p_source.utf8();

	diagnostic_sink = r_diagnostics_by_path;
	const int32_t status = host.CheckProject(path_utf8.get_data(), source_utf8.get_data());
	diagnostic_sink = nullptr;

	return status == VH_OK ? OK : ERR_COMPILATION_FAILED;
}

Error VerseRuntime::begin_check_project(const String &p_globalized_path, const String &p_source) {
	if (!host.is_loaded()) {
		return ERR_UNAVAILABLE;
	}

	const CharString path_utf8 = p_globalized_path.utf8();
	const CharString source_utf8 = p_source.utf8();

	// The host copies both before returning, so neither has to outlive this call.
	return host.CheckProjectBegin(path_utf8.get_data(), source_utf8.get_data()) == VH_OK ? OK : ERR_BUSY;
}

bool VerseRuntime::poll_check_project(Dictionary *r_diagnostics_by_path) {
	if (!host.is_loaded()) {
		return false;
	}

	vh_bool finished = 0;
	diagnostic_sink = r_diagnostics_by_path;
	host.CheckProjectPoll(&finished);
	diagnostic_sink = nullptr;

	return finished != 0;
}

bool VerseRuntime::is_check_project_busy() const {
	return host.is_loaded() && host.CheckProjectBusy() != 0;
}

bool VerseRuntime::has_class(const String &p_class_name) const {
	if (!host.is_loaded()) {
		return false;
	}
	return host.HasClass(p_class_name.utf8().get_data()) != 0;
}

TypedArray<Dictionary> VerseRuntime::class_exports(const String &p_class_name) const {
	TypedArray<Dictionary> exports;
	if (!host.is_loaded()) {
		return exports;
	}

	const vh_export_desc *descs = nullptr;
	int32_t count = 0;
	if (host.ClassExportList(p_class_name.utf8().get_data(), &descs, &count) != VH_OK) {
		return exports;
	}

	for (int32_t i = 0; i < count; i++) {
		Dictionary entry;
		entry["name"] = String::utf8(descs[i].NameUtf8, descs[i].NameLen);
		entry["type"] = (int64_t)descs[i].Type;
		entry["is_var"] = descs[i].IsVar != 0;
		entry["clamp_min"] = String::utf8(descs[i].ClampMinUtf8, descs[i].ClampMinLen);
		entry["clamp_max"] = String::utf8(descs[i].ClampMaxUtf8, descs[i].ClampMaxLen);
		entry["category"] = String::utf8(descs[i].CategoryUtf8, descs[i].CategoryLen);
		exports.push_back(entry);
	}
	return exports;
}

Dictionary VerseRuntime::lookup_symbol(const String &p_globalized_path, int32_t p_line, int32_t p_column) const {
	Dictionary result;
	if (!host.is_loaded()) {
		return result;
	}

	const vh_lookup_desc *desc = nullptr;
	if (host.LookupSymbol(p_globalized_path.utf8().get_data(), p_line, p_column, &desc) != VH_OK) {
		return result;
	}

	result["name"] = String::utf8(desc->NameUtf8, desc->NameLen);
	result["path"] = String::utf8(desc->PathUtf8, desc->PathLen);
	result["line"] = (int64_t)desc->Line;
	result["column"] = (int64_t)desc->Column;
	result["type"] = String::utf8(desc->TypeUtf8, desc->TypeLen);
	result["owner"] = String::utf8(desc->OwnerUtf8, desc->OwnerLen);
	result["kind"] = (int64_t)desc->Kind;
	result["is_var"] = desc->IsVar != 0;
	result["is_parameter"] = desc->IsParameter != 0;
	result["is_definition"] = desc->IsDefinition != 0;
	result["overridden_owner"] = String::utf8(desc->OverriddenOwnerUtf8, desc->OverriddenOwnerLen);
	result["overridden_path"] = String::utf8(desc->OverriddenPathUtf8, desc->OverriddenPathLen);
	result["overridden_line"] = (int64_t)desc->OverriddenLine;
	return result;
}

// One ABI completion item as the Dictionary every consumer here reads. Shared because the item
// shape is now returned by three entry points.
static Dictionary complete_item_to_dict(const vh_complete_item &p_item) {
	Dictionary entry;
	entry["name"] = String::utf8(p_item.NameUtf8, p_item.NameLen);
	entry["type"] = String::utf8(p_item.TypeUtf8, p_item.TypeLen);
	entry["owner"] = String::utf8(p_item.OwnerUtf8, p_item.OwnerLen);
	entry["path"] = String::utf8(p_item.PathUtf8, p_item.PathLen);
	entry["line"] = (int64_t)p_item.Line;
	entry["kind"] = (int64_t)p_item.Kind;
	entry["is_var"] = p_item.IsVar != 0;
	entry["param_count"] = (int64_t)p_item.ParamCount;
	return entry;
}

TypedArray<Dictionary> VerseRuntime::complete_symbol(const String &p_globalized_path, const String &p_source, int32_t p_line, int32_t p_column, int32_t p_mode) const {
	TypedArray<Dictionary> options;
	if (!host.is_loaded()) {
		return options;
	}

	const vh_complete_item *items = nullptr;
	int32_t count = 0;
	if (host.CompleteSymbol(p_globalized_path.utf8().get_data(), p_source.utf8().get_data(), p_line, p_column, p_mode, &items, &count) != VH_OK) {
		return options;
	}

	for (int32_t i = 0; i < count; i++) {
		options.push_back(complete_item_to_dict(items[i]));
	}
	return options;
}

TypedArray<Dictionary> VerseRuntime::class_members(const String &p_class_name) const {
	TypedArray<Dictionary> members;
	if (!host.is_loaded()) {
		return members;
	}

	const vh_complete_item *items = nullptr;
	int32_t count = 0;
	if (host.ClassMembers(p_class_name.utf8().get_data(), &items, &count) != VH_OK) {
		return members;
	}

	for (int32_t i = 0; i < count; i++) {
		members.push_back(complete_item_to_dict(items[i]));
	}
	return members;
}

Dictionary VerseRuntime::signature_at(const String &p_globalized_path, const String &p_source, int32_t p_line, int32_t p_column) const {
	Dictionary result;
	if (!host.is_loaded()) {
		return result;
	}

	const vh_signature_desc *desc = nullptr;
	if (host.SignatureAt(p_globalized_path.utf8().get_data(), p_source.utf8().get_data(), p_line, p_column, &desc) != VH_OK || desc == nullptr) {
		return result;
	}

	TypedArray<Dictionary> params;
	for (int32_t i = 0; i < desc->ParamCount; i++) {
		params.push_back(complete_item_to_dict(desc->Params[i]));
	}

	result["name"] = String::utf8(desc->NameUtf8, desc->NameLen);
	result["result"] = String::utf8(desc->ResultUtf8, desc->ResultLen);
	result["params"] = params;
	return result;
}

Variant VerseRuntime::instance_field(vh_instance *p_instance, const String &p_name) const {
	if (!host.is_loaded() || p_instance == nullptr) {
		return Variant();
	}
	const vh_value *value = nullptr;
	if (host.InstanceGetField(p_instance, p_name.utf8().get_data(), &value) != VH_OK || value == nullptr) {
		return Variant();
	}
	return vh_to_variant(*value);
}

Variant VerseRuntime::class_default_field(const String &p_class_name, const String &p_name) const {
	if (!host.is_loaded()) {
		return Variant();
	}
	const vh_value *value = nullptr;
	if (host.ClassDefaultField(p_class_name.utf8().get_data(), p_name.utf8().get_data(), &value) != VH_OK || value == nullptr) {
		return Variant();
	}
	return vh_to_variant(*value);
}

bool VerseRuntime::set_instance_field(vh_instance *p_instance, const String &p_name, const Variant &p_value) {
	if (!host.is_loaded() || p_instance == nullptr) {
		return false;
	}

	vh_value value = {};
	value.VariantTag = VH_VARIANT_NIL;

	// Held until the call returns: vh_value borrows the bytes rather than owning them.
	CharString text;

	switch (p_value.get_type()) {
		case Variant::BOOL:
			value.Type = VH_TYPE_LOGIC;
			value.Logic = ((bool)p_value) ? 1 : 0;
			break;
		case Variant::INT:
			value.Type = VH_TYPE_INT;
			value.Int = (int64_t)p_value;
			break;
		case Variant::FLOAT:
			value.Type = VH_TYPE_FLOAT;
			value.Float = (double)p_value;
			break;
		case Variant::STRING:
			text = String(p_value).utf8();
			value.Type = VH_TYPE_STRING;
			value.String.Utf8 = text.get_data();
			value.String.Len = text.length();
			break;
		default:
			return false;
	}

	return host.InstanceSetField(p_instance, p_name.utf8().get_data(), &value) == VH_OK;
}

vh_instance *VerseRuntime::instantiate(const String &p_class_name, int64_t p_object_id) {
	if (!host.is_loaded()) {
		return nullptr;
	}
	vh_instance *instance = nullptr;
	host.Instantiate(p_class_name.utf8().get_data(), p_object_id, &instance);
	return instance;
}

void VerseRuntime::release_instance(vh_instance *p_instance) {
	if (p_instance != nullptr && host.ReleaseInstance != nullptr) {
		host.ReleaseInstance(p_instance);
	}
}

bool VerseRuntime::instance_has_function(vh_instance *p_instance, const char *p_decorated_name) const {
	if (!host.is_loaded() || p_instance == nullptr) {
		return false;
	}
	return host.InstanceHasFunction(p_instance, p_decorated_name) != 0;
}

Error VerseRuntime::call_instance_void(vh_instance *p_instance, const char *p_decorated_name) {
	if (!host.is_loaded() || p_instance == nullptr) {
		return ERR_UNAVAILABLE;
	}
	const int32_t status = host.InstanceCallVoid(p_instance, p_decorated_name);
	if (status != VH_OK) {
		UtilityFunctions::push_error(String("VerseRuntime: ") + String(p_decorated_name) + String(" failed with status ") + String::num_int64(status));
		return FAILED;
	}
	return OK;
}

Error VerseRuntime::call_instance_void_float(vh_instance *p_instance, const char *p_decorated_name, double p_arg) {
	if (!host.is_loaded() || p_instance == nullptr) {
		return ERR_UNAVAILABLE;
	}
	const int32_t status = host.InstanceCallVoidFloat(p_instance, p_decorated_name, p_arg);
	if (status != VH_OK) {
		UtilityFunctions::push_error(String("VerseRuntime: ") + String(p_decorated_name) + String(" failed with status ") + String::num_int64(status));
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

int32_t VerseRuntime::api_get_property(void *p_ctx, vh_handle p_handle, const char *p_name_utf8, int32_t p_name_len, vh_arena *p_arena, vh_value *r_value) {
	if (r_value == nullptr) {
		return VH_CALL_BAD_VALUE;
	}
	Object *obj = UtilityFunctions::instance_from_id(p_handle);
	if (obj == nullptr) {
		return VH_CALL_DEAD_OBJECT;
	}

	const StringName name(String::utf8(p_name_utf8, p_name_len));
	const Variant value = obj->get(name);
	if (value.get_type() == Variant::NIL) {
		// obj->get has no "does this property exist" signal of its own; NIL is the only miss
		// indicator available, so a genuinely nil property also reads as absent.
		return VH_CALL_NO_SUCH_MEMBER;
	}

	return variant_to_vh(value, p_arena, *r_value) ? VH_CALL_OK : VH_CALL_BAD_VALUE;
}

int32_t VerseRuntime::api_set_property(void *p_ctx, vh_handle p_handle, const char *p_name_utf8, int32_t p_name_len, const vh_value *p_value) {
	if (p_value == nullptr) {
		return VH_CALL_BAD_VALUE;
	}
	Object *obj = UtilityFunctions::instance_from_id(p_handle);
	if (obj == nullptr) {
		return VH_CALL_DEAD_OBJECT;
	}

	// Object::set is void and silently ignores an unknown name, so a write has no miss to report.
	const StringName name(String::utf8(p_name_utf8, p_name_len));
	obj->set(name, vh_to_variant(*p_value));
	return VH_CALL_OK;
}

int32_t VerseRuntime::api_call_method(void *p_ctx, vh_handle p_handle, const char *p_name_utf8, int32_t p_name_len, const vh_value *p_args, int32_t p_arg_count, vh_arena *p_arena, vh_value *r_value) {
	Object *obj = UtilityFunctions::instance_from_id(p_handle);
	if (obj == nullptr) {
		return VH_CALL_DEAD_OBJECT;
	}

	const StringName name(String::utf8(p_name_utf8, p_name_len));
	if (!obj->has_method(name)) {
		return VH_CALL_NO_SUCH_MEMBER;
	}

	Array args;
	for (int32_t i = 0; i < p_arg_count; i++) {
		args.push_back(vh_to_variant(p_args[i]));
	}
	const Variant result = obj->callv(name, args);

	if (r_value == nullptr) {
		return VH_CALL_OK;
	}
	return variant_to_vh(result, p_arena, *r_value) ? VH_CALL_OK : VH_CALL_BAD_VALUE;
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

vh_handle VerseRuntime::api_instantiate(void *p_ctx, const char *p_class_name_utf8, int32_t p_class_name_len) {
	const StringName class_name(String::utf8(p_class_name_utf8, p_class_name_len));
	if (!ClassDB::class_exists(class_name) || !ClassDB::can_instantiate(class_name)) {
		return 0;
	}
	Object *obj = ClassDB::instantiate(class_name);
	if (obj == nullptr) {
		return 0;
	}
	return obj->get_instance_id();
}

vh_handle VerseRuntime::api_get_singleton(void *p_ctx, const char *p_name_utf8, int32_t p_name_len) {
	Object *singleton = Engine::get_singleton()->get_singleton(StringName(String::utf8(p_name_utf8, p_name_len)));
	return singleton != nullptr ? singleton->get_instance_id() : 0;
}

void VerseRuntime::on_diagnostic(void *p_ctx, const vh_diagnostic *p_diagnostic) {
	const String file = p_diagnostic->FilePathLen > 0 ? String::utf8(p_diagnostic->FilePathUtf8, p_diagnostic->FilePathLen) : String("<unknown>");
	const String message = String::utf8(p_diagnostic->MessageUtf8, p_diagnostic->MessageLen);

	VerseRuntime *runtime = static_cast<VerseRuntime *>(p_ctx);
	if (runtime != nullptr && runtime->diagnostic_sink != nullptr && p_diagnostic->Severity == VH_SEVERITY_ERROR) {
		Dictionary error;
		error["line"] = p_diagnostic->Line;
		error["column"] = p_diagnostic->Column;
		error["message"] = message;
		error["path"] = file;

		Dictionary &sink = *runtime->diagnostic_sink;
		TypedArray<Dictionary> for_file = sink.has(file) ? TypedArray<Dictionary>(sink[file]) : TypedArray<Dictionary>();
		for_file.push_back(error);
		sink[file] = for_file;

		// Whoever installed the sink decides what reaches the log. Analysis re-runs on every
		// keystroke and every save, so logging from here repeats one error indefinitely.
		return;
	}

	const String formatted = file + String(":") + String::num_int64(p_diagnostic->Line) + String(":") + String::num_int64(p_diagnostic->Column) + String(": ") + message;

	if (p_diagnostic->Severity == VH_SEVERITY_ERROR) {
		UtilityFunctions::push_error(formatted);
	} else if (p_diagnostic->Severity == VH_SEVERITY_WARNING) {
		UtilityFunctions::push_warning(formatted);
	} else {
		UtilityFunctions::print(formatted);
	}
}
