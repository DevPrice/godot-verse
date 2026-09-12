#include "verse_runtime.h"

#include "verse_ref_table.h"
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
	const String dll_default = String();
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
	const String engine_default = String();
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

	const String dll_setting = settings->get_setting(dll_setting_name);
	if (dll_setting.is_empty()) {
		UtilityFunctions::push_error("VerseRuntime: " + dll_setting_name + " is unset; point it at <engine>/Engine/Binaries/Win64/verse_host.dll");
		return ERR_UNCONFIGURED;
	}

	const String dll_path = settings->globalize_path(dll_setting);
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
	godot_api.IsValid = &VerseRuntime::api_is_valid;
	godot_api.GetProperty = &VerseRuntime::api_get_property;
	godot_api.SetProperty = &VerseRuntime::api_set_property;
	godot_api.CallMethod = &VerseRuntime::api_call_method;
	godot_api.GetSingleton = &VerseRuntime::api_get_singleton;
	godot_api.ReleaseRef = &VerseRuntime::api_release_ref;
	godot_api.RetainRef = &VerseRuntime::api_retain_ref;
	godot_api.NewRef = &VerseRuntime::api_new_ref;
	godot_api.RefGet = &VerseRuntime::api_ref_get;
	godot_api.RefSet = &VerseRuntime::api_ref_set;
	godot_api.RefSize = &VerseRuntime::api_ref_size;
	godot_api.RefContents = &VerseRuntime::api_ref_contents;
	godot_api.InvokeCallable = &VerseRuntime::api_invoke_callable;

	// EngineDirUtf8 only needs to stay alive for the duration of host.Init below.
	const CharString engine_dir_utf8 = p_engine_dir.is_empty() ? CharString() : p_engine_dir.utf8();

	init_desc = vh_init_desc{};
	init_desc.StructSize = sizeof(vh_init_desc);
	init_desc.AbiVersion = VH_ABI_VERSION;
	init_desc.EngineDirUtf8 = p_engine_dir.is_empty() ? nullptr : engine_dir_utf8.get_data();
	init_desc.Godot = godot_api;
	init_desc.OnDiagnostic = &VerseRuntime::on_diagnostic;
	init_desc.DiagnosticCtx = this;
	init_desc.OnRuntimeError = &VerseRuntime::on_runtime_error;
	init_desc.RuntimeErrorCtx = this;
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

	// Before the host goes, and so while Godot is still alive to free them. The table holds
	// Variants -- Arrays, Dictionaries, Callables -- and freeing one after Godot's own teardown is
	// a use-after-free at exit rather than a leak. Releases arriving from the host afterwards name
	// nothing and are no-ops, which is exactly what a cleared table answers.
	verse_ref_table().clear();

	if (host.Shutdown != nullptr) {
		host.Shutdown();
	}
	host.unload();
}

bool VerseRuntime::is_host_loaded() const {
	return host.is_loaded();
}

Error VerseRuntime::compile_project(const PackedStringArray &p_globalized_paths, const PackedStringArray &p_module_paths, Dictionary *r_diagnostics_by_path) {
	if (!host.is_loaded()) {
		return ERR_UNAVAILABLE;
	}
	ERR_FAIL_COND_V(p_module_paths.size() != p_globalized_paths.size(), ERR_INVALID_PARAMETER);

	// The pointers handed to the host must outlive the call, so the CharStrings backing them
	// have to stay alive alongside the array of structs that points at them.
	std::vector<CharString> utf8_paths;
	std::vector<CharString> utf8_modules;
	std::vector<vh_source_file> files;
	utf8_paths.reserve(p_globalized_paths.size());
	utf8_modules.reserve(p_globalized_paths.size());
	files.reserve(p_globalized_paths.size());
	for (int64_t i = 0; i < p_globalized_paths.size(); i++) {
		utf8_paths.push_back(p_globalized_paths[i].utf8());
		utf8_modules.push_back(p_module_paths[i].utf8());
		files.push_back({ utf8_paths.back().get_data(), utf8_modules.back().get_data() });
	}

	diagnostic_sink = r_diagnostics_by_path;
	int32_t built_generation = 0;
	const int32_t status = host.CompileProject(files.data(), (int32_t)files.size(), &built_generation);
	diagnostic_sink = nullptr;

	if (status != VH_OK) {
		return ERR_COMPILATION_FAILED;
	}
	generation = built_generation;
	return OK;
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

TypedArray<Dictionary> VerseRuntime::class_exports(const String &p_class_name, bool *r_found) const {
	TypedArray<Dictionary> exports;
	if (r_found != nullptr) {
		*r_found = false;
	}
	if (!host.is_loaded()) {
		return exports;
	}

	const vh_export_desc *descs = nullptr;
	int32_t count = 0;
	if (host.ClassExportList(p_class_name.utf8().get_data(), &descs, &count) != VH_OK) {
		return exports;
	}
	if (r_found != nullptr) {
		*r_found = true;
	}

	for (int32_t i = 0; i < count; i++) {
		Dictionary entry;
		entry["name"] = String::utf8(descs[i].NameUtf8, descs[i].NameLen);
		entry["type"] = (int64_t)descs[i].Type;
		entry["variant_tag"] = (int64_t)descs[i].VariantTag;
		entry["element_variant_tag"] = (int64_t)descs[i].ElementVariantTag;
		entry["is_var"] = descs[i].IsVar != 0;
		entry["hint"] = (int64_t)descs[i].Hint;
		entry["hint_string"] = String::utf8(descs[i].HintStringUtf8, descs[i].HintStringLen);
		entry["native_class"] = String::utf8(descs[i].NativeClassUtf8, descs[i].NativeClassLen);
		entry["range_min"] = descs[i].RangeMin;
		entry["range_max"] = descs[i].RangeMax;
		entry["has_range_min"] = descs[i].HasRangeMin != 0;
		entry["has_range_max"] = descs[i].HasRangeMax != 0;
		entry["group_kind"] = (int64_t)descs[i].GroupKind;
		entry["group_name"] = String::utf8(descs[i].GroupNameUtf8, descs[i].GroupNameLen);
		entry["line"] = (int64_t)descs[i].Line;
		entry["column"] = (int64_t)descs[i].Column;
		entry["reject"] = (int64_t)descs[i].Reject;
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
	entry["signature"] = String::utf8(p_item.SignatureUtf8, p_item.SignatureLen);
	entry["is_overridable"] = p_item.IsOverridable != 0;
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

	// The same packing a method argument gets, rather than a switch of its own. Which types a member
	// can actually hold is the host's answer -- it knows the declared type and refuses the rest -- so
	// a second, narrower list here would only disagree with it.
	//
	// The arena outlives the call and no longer: a vh_value borrows its strings and its items.
	VerseArena arena;
	vh_value value = {};
	if (!variant_to_vh(p_value, arena.get(), value)) {
		return false;
	}

	return host.InstanceSetField(p_instance, p_name.utf8().get_data(), &value) == VH_OK;
}

bool VerseRuntime::set_instance_field_instance(vh_instance *p_instance, const String &p_name, vh_instance *p_value) {
	if (!host.is_loaded() || p_instance == nullptr) {
		return false;
	}
	return host.InstanceSetFieldInstance(p_instance, p_name.utf8().get_data(), p_value) == VH_OK;
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

int32_t VerseRuntime::call_instance(vh_instance *p_instance,
		const char *p_decorated_name,
		const Variant **p_args,
		int32_t p_arg_count,
		Variant &r_result) {
	r_result = Variant();
	if (!host.is_loaded() || p_instance == nullptr) {
		return VH_ERR_STATE;
	}

	// The arena outlives the call and nothing else: every string and container the arguments point
	// at is allocated from it, and the host has copied whatever it needed by the time this returns.
	VerseArena arena;
	std::vector<vh_value> wire;
	wire.resize((size_t)p_arg_count);
	for (int32_t i = 0; i < p_arg_count; ++i) {
		if (!variant_to_vh(*p_args[i], arena.get(), wire[(size_t)i])) {
			return VH_ERR_ARGUMENT;
		}
	}

	vh_value result = {};
	const int32_t status = host.InstanceCall(
			p_instance, p_decorated_name, wire.empty() ? nullptr : wire.data(), p_arg_count, nullptr, &result);
	if (status == VH_OK) {
		r_result = vh_to_variant(result);
	}
	return status;
}

Vector<VerseMethodInfo> VerseRuntime::class_methods(const String &p_class_name) const {
	Vector<VerseMethodInfo> methods;
	if (!host.is_loaded()) {
		return methods;
	}

	const vh_method_desc *descs = nullptr;
	int32_t count = 0;
	if (host.ClassMethodList(p_class_name.utf8().get_data(), &descs, &count) != VH_OK) {
		return methods;
	}

	methods.resize(count);
	for (int32_t i = 0; i < count; ++i) {
		const vh_method_desc &desc = descs[i];
		VerseMethodInfo &info = methods.write[i];
		info.name = StringName(String::utf8(desc.NameUtf8, desc.NameLen));
		info.decorated = String::utf8(desc.DecoratedUtf8, desc.DecoratedLen).utf8();
		if (desc.GodotVirtualLen > 0) {
			info.godot_virtual = StringName(String::utf8(desc.GodotVirtualUtf8, desc.GodotVirtualLen));
		}
		info.required_params = desc.RequiredParamCount;
		info.can_fail = desc.CanFail != 0;
		info.suspends = desc.Suspends != 0;
		info.returns_value = desc.ResultType != VH_TYPE_VOID;
		info.return_type = info.returns_value ? variant_type_for(desc.ResultType, desc.ResultVariantTag) : Variant::NIL;

		info.params.resize(desc.ParamCount);
		for (int32_t j = 0; j < desc.ParamCount; ++j) {
			const vh_param_desc &param = desc.Params[j];
			VerseMethodInfo::Param &out = info.params.write[j];
			out.name = StringName(String::utf8(param.NameUtf8, param.NameLen));
			out.type = variant_type_for(param.Type, param.VariantTag);
		}
	}
	return methods;
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


void VerseRuntime::api_release_ref(void *p_ctx, int64_t p_ref) {
	verse_ref_table().release(p_ref);
}

int64_t VerseRuntime::api_retain_ref(void *p_ctx, int64_t p_ref) {
	return verse_ref_table().retain(p_ref);
}

int64_t VerseRuntime::api_new_ref(void *p_ctx, int32_t p_variant_tag) {
	switch (p_variant_tag) {
		case VH_VARIANT_ARRAY:
			return verse_ref_table().mint(Array());
		case VH_VARIANT_DICTIONARY:
			return verse_ref_table().mint(Dictionary());
		case VH_VARIANT_PACKED_BYTE_ARRAY:
			return verse_ref_table().mint(PackedByteArray());
		case VH_VARIANT_PACKED_INT32_ARRAY:
			return verse_ref_table().mint(PackedInt32Array());
		case VH_VARIANT_PACKED_INT64_ARRAY:
			return verse_ref_table().mint(PackedInt64Array());
		case VH_VARIANT_PACKED_FLOAT32_ARRAY:
			return verse_ref_table().mint(PackedFloat32Array());
		case VH_VARIANT_PACKED_FLOAT64_ARRAY:
			return verse_ref_table().mint(PackedFloat64Array());
		case VH_VARIANT_PACKED_STRING_ARRAY:
			return verse_ref_table().mint(PackedStringArray());
		case VH_VARIANT_PACKED_VECTOR2_ARRAY:
			return verse_ref_table().mint(PackedVector2Array());
		case VH_VARIANT_PACKED_VECTOR3_ARRAY:
			return verse_ref_table().mint(PackedVector3Array());
		case VH_VARIANT_PACKED_COLOR_ARRAY:
			return verse_ref_table().mint(PackedColorArray());
		case VH_VARIANT_PACKED_VECTOR4_ARRAY:
			return verse_ref_table().mint(PackedVector4Array());
		default:
			// A Callable or a Signal cannot be made from nothing -- both name something to call --
			// and no other tag is a reference type at all.
			return 0;
	}
}

int32_t VerseRuntime::api_ref_get(void *p_ctx, int64_t p_ref, const vh_value *p_key, vh_arena *p_arena, vh_value *r_value) {
	const Variant *found = verse_ref_table().find(p_ref);
	if (found == nullptr || p_key == nullptr) {
		return VH_CALL_DEAD_OBJECT;
	}

	// `get`, not `get_indexed`: the latter takes an int64 index, so a Dictionary asked for a
	// string key silently read element zero. This is the keyed accessor, and it serves an Array
	// indexed by an integer and a Dictionary keyed by anything alike.
	//
	// A missing key and an index out of range are the same answer, and it is not an error: the
	// host turns VH_CALL_NO_SUCH_MEMBER into an ordinary Verse failure the script can handle.
	bool valid = false;
	const Variant got = found->get(vh_to_variant(*p_key), &valid);
	if (!valid) {
		return VH_CALL_NO_SUCH_MEMBER;
	}
	return variant_to_vh(got, p_arena, *r_value) ? VH_CALL_OK : VH_CALL_BAD_VALUE;
}

int32_t VerseRuntime::api_ref_set(void *p_ctx, int64_t p_ref, const vh_value *p_key, const vh_value *p_value) {
	VerseRefTable &table = verse_ref_table();
	const Variant *found = table.find(p_ref);
	if (found == nullptr || p_key == nullptr || p_value == nullptr) {
		return VH_CALL_DEAD_OBJECT;
	}

	// A copy, then written back. Godot's Array and Dictionary are references, so the copy shares
	// their storage and the write reaches every other holder -- which is the semantics the whole
	// reference design exists to preserve. A packed array is a value, and for one the write-back
	// is what makes the mutation stick.
	Variant container = *found;
	const Variant key = vh_to_variant(*p_key);

	// Writing one past the end appends, which is the only way to fill a fresh container: Godot's
	// indexed setter refuses an out-of-range index outright rather than growing, for a packed array
	// as much as for an Array. Without this the host's NewRefFrom -- how every Verse array reaches a
	// Godot method taking one -- left every container it built empty, and said nothing.
	//
	// Exactly one past the end, so a write at index 5 of a two-element array is still the error
	// GDScript makes it. A Dictionary is untouched: any key is a legal key there, and `size` is not
	// a position.
	if (key.get_type() == Variant::INT && container.get_type() != Variant::DICTIONARY) {
		const Variant size = container.call("size");
		if (size.get_type() == Variant::INT && (int64_t)key == (int64_t)size) {
			container.call("resize", (int64_t)size + 1);
		}
	}

	bool valid = false;
	container.set(key, vh_to_variant(*p_value), &valid);
	if (!valid) {
		return VH_CALL_BAD_VALUE;
	}
	table.assign(p_ref, container);
	return VH_CALL_OK;
}

int32_t VerseRuntime::api_ref_size(void *p_ctx, int64_t p_ref, int64_t *r_size) {
	const Variant *found = verse_ref_table().find(p_ref);
	if (found == nullptr || r_size == nullptr) {
		return VH_CALL_DEAD_OBJECT;
	}
	Variant container = *found;
	const Variant size = container.call("size");
	*r_size = size.get_type() == Variant::INT ? (int64_t)size : 0;
	return VH_CALL_OK;
}

int32_t VerseRuntime::api_ref_contents(void *p_ctx, int64_t p_ref, vh_arena *p_arena, vh_value *r_value) {
	const Variant *found = verse_ref_table().find(p_ref);
	if (found == nullptr) {
		return VH_CALL_DEAD_OBJECT;
	}

	// A Dictionary comes back as pairs and everything else as a sequence; both are shapes
	// vh_to_variant can rebuild, which is what makes the bulk converters round-trip.
	if (found->get_type() == Variant::DICTIONARY) {
		return variant_to_vh(*found, p_arena, *r_value) ? VH_CALL_OK : VH_CALL_BAD_VALUE;
	}

	// Every sequence type answers `size` and indexes by int, so one loop serves all eleven.
	Variant container = *found;
	const Variant size = container.call("size");
	const int64_t count = size.get_type() == Variant::INT ? (int64_t)size : 0;
	Array items;
	for (int64_t i = 0; i < count; i++) {
		bool valid = false;
		bool oob = false;
		items.push_back(container.get_indexed(i, valid, oob));
		if (!valid || oob) {
			return VH_CALL_BAD_VALUE;
		}
	}
	// array_to_vh_seq rather than variant_to_vh: the latter would mint a second id for the Array
	// just built, and the caller asked for the contents rather than another reference to them.
	return array_to_vh_seq(items, p_arena, *r_value) ? VH_CALL_OK : VH_CALL_BAD_VALUE;
}

int32_t VerseRuntime::api_invoke_callable(void *p_ctx, int64_t p_ref, const vh_value *p_args, int32_t p_arg_count, vh_arena *p_arena, vh_value *r_value) {
	const Variant *found = verse_ref_table().find(p_ref);
	if (found == nullptr || found->get_type() != Variant::CALLABLE) {
		return VH_CALL_DEAD_OBJECT;
	}

	const Callable callable = *found;
	if (!callable.is_valid()) {
		return VH_CALL_DEAD_OBJECT;
	}


	Array args;
	for (int32_t i = 0; i < p_arg_count; i++) {
		args.push_back(vh_to_variant(p_args[i]));
	}

	// A GDScript lambda that has been called, and is still referenced when Godot runs
	// ScriptServer::finish_languages(), segfaults the engine at exit. That is an upstream defect,
	// not this call's: it reproduces in eight lines of GDScript with no GDExtension loaded, and
	// GDScriptLanguage::finish names the case in its own comments (GH-102327). Spec R-TYPE-3 has
	// the reduction. Nothing here can avoid it -- Main::cleanup finishes the languages before it
	// deinitialises an extension, and ScriptLanguage::finish is never delivered to one -- so this
	// is deliberately not worked around.
	const Variant result = callable.callv(args);
	return variant_to_vh(result, p_arena, *r_value) ? VH_CALL_OK : VH_CALL_BAD_VALUE;
}

void VerseRuntime::on_runtime_error(void *p_ctx, const vh_runtime_error *p_error) {
	if (p_error == nullptr) {
		return;
	}

	const String message = String::utf8(p_error->MessageUtf8, p_error->MessageLen);

	// The innermost frame with a source location is what the error is *at*, so it is what
	// push_error is told -- Godot makes the file and line it is given clickable, and the rest of
	// the stack is only useful underneath it.
	const vh_stack_frame *site = nullptr;
	for (int32_t i = 0; i < p_error->FrameCount; i++) {
		if (p_error->Frames[i].PathLen > 0 && p_error->Frames[i].Line > 0) {
			site = &p_error->Frames[i];
			break;
		}
	}

	if (site != nullptr) {
		const String path = String::utf8(site->PathUtf8, site->PathLen);
		const String function = String::utf8(site->FunctionUtf8, site->FunctionLen);
		UtilityFunctions::push_error(message, function, path, site->Line);
	} else {
		UtilityFunctions::push_error(message);
	}

	// The rest of the stack, innermost first, as its own lines. Printed rather than pushed so one
	// error is one entry in the errors panel with its stack beneath it.
	for (int32_t i = 0; i < p_error->FrameCount; i++) {
		const vh_stack_frame &frame = p_error->Frames[i];
		String line = String("    at ") + String::utf8(frame.FunctionUtf8, frame.FunctionLen);
		if (frame.PathLen > 0) {
			line += String(" (") + String::utf8(frame.PathUtf8, frame.PathLen);
			if (frame.Line > 0) {
				line += String(":") + String::num_int64(frame.Line);
			}
			line += String(")");
		}
		UtilityFunctions::print(line);
	}
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
