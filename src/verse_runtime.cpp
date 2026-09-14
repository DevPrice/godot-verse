#include "verse_runtime.h"

#include "verse_callable.h"
#include "verse_ref_table.h"
#include "verse_script_language.h"
#include "verse_value.h"

#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/performance.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/classes/window.hpp>
#include <godot_cpp/classes/class_db_singleton.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/error_macros.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/char_string.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/node_path.hpp>
#include <godot_cpp/variant/signal.hpp>
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
	ClassDB::bind_method(D_METHOD("_monitor_queued_jobs"), &VerseRuntime::_monitor_queued_jobs);
	ClassDB::bind_method(D_METHOD("_monitor_pump_ms"), &VerseRuntime::_monitor_pump_ms);
	ClassDB::bind_method(D_METHOD("_monitor_sleeping_tasks"), &VerseRuntime::_monitor_sleeping_tasks);
	ClassDB::bind_method(D_METHOD("_monitor_analysis_wait_ms"), &VerseRuntime::_monitor_analysis_wait_ms);
	ClassDB::bind_method(D_METHOD("build_project"), &VerseRuntime::build_project);
}

VerseRuntime::~VerseRuntime() {
	unload_host();
}

PackedStringArray VerseRuntime::modules_declaring(const String &p_name) const {
	PackedStringArray modules;
	if (!host.is_loaded()) {
		return modules;
	}

	const CharString name_utf8 = p_name.utf8();
	const vh_module_ref *found = nullptr;
	int32_t count = 0;
	if (host.ResolveUnknownName(name_utf8.get_data(), &found, &count) != VH_OK) {
		return modules;
	}
	for (int32_t i = 0; i < count; i++) {
		modules.push_back(String::utf8(found[i].PathUtf8, found[i].PathLen));
	}
	return modules;
}

Error VerseRuntime::build_project() {
	// Delegated rather than done here, because which files are in the project is a question about
	// res:// and the language is what enumerates it. This is the same build the editor's Play
	// button and Build action ask for, reachable from a script for the benefit of anything that
	// has to make an edit live without an editor -- the headless integration suite included.
	VerseScriptLanguage *language = VerseScriptLanguage::singleton();
	return language != nullptr ? language->build_project() : ERR_UNAVAILABLE;
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
	godot_api.GetClassOf = &VerseRuntime::api_get_class_of;
	godot_api.MakeCallable = &VerseRuntime::api_make_callable;
	godot_api.CallStatic = &VerseRuntime::api_call_static;
	godot_api.CallUtility = &VerseRuntime::api_call_utility;
	godot_api.EmitSignal = &VerseRuntime::api_emit_signal;
	godot_api.ConnectSignal = &VerseRuntime::api_connect_signal;
	godot_api.DisconnectSignal = &VerseRuntime::api_disconnect_signal;
	godot_api.SignalTarget = &VerseRuntime::api_signal_target;
	godot_api.MakeSignalRef = &VerseRuntime::api_make_signal_ref;
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

TypedArray<Dictionary> VerseRuntime::complete_symbol(const String &p_globalized_path, const String &p_source, int32_t p_line, int32_t p_column, int32_t p_mode, bool *r_not_ready) const {
	TypedArray<Dictionary> options;
	if (!host.is_loaded()) {
		return options;
	}

	const vh_complete_item *items = nullptr;
	int32_t count = 0;
	const int32_t status = host.CompleteSymbol(p_globalized_path.utf8().get_data(), p_source.utf8().get_data(), p_line, p_column, p_mode, &items, &count);
	if (status != VH_OK) {
		if (r_not_ready != nullptr && status == VH_ERR_STATE) {
			*r_not_ready = true;
		}
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

TypedArray<Dictionary> VerseRuntime::class_override_candidates(const String &p_class_name) const {
	TypedArray<Dictionary> candidates;
	if (!host.is_loaded()) {
		return candidates;
	}

	const vh_complete_item *items = nullptr;
	int32_t count = 0;
	if (host.ClassOverrideCandidates(p_class_name.utf8().get_data(), &items, &count) != VH_OK) {
		return candidates;
	}

	for (int32_t i = 0; i < count; i++) {
		candidates.push_back(complete_item_to_dict(items[i]));
	}
	return candidates;
}

Dictionary VerseRuntime::signature_at(const String &p_globalized_path, const String &p_source, int32_t p_line, int32_t p_column, bool *r_not_ready) const {
	Dictionary result;
	if (!host.is_loaded()) {
		return result;
	}

	const vh_signature_desc *desc = nullptr;
	const int32_t status = host.SignatureAt(p_globalized_path.utf8().get_data(), p_source.utf8().get_data(), p_line, p_column, &desc);
	if (status != VH_OK || desc == nullptr) {
		if (r_not_ready != nullptr && status == VH_ERR_STATE) {
			*r_not_ready = true;
		}
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

int32_t VerseRuntime::invoke_callback(int64_t p_callback_id, const Variant **p_args, int32_t p_arg_count, Variant &r_result) {
	r_result = Variant();
	if (!host.is_loaded() || host.CallbackInvoke == nullptr) {
		return VH_ERR_STATE;
	}

	VerseArena arena;
	std::vector<vh_value> wire;
	wire.resize((size_t)p_arg_count);
	for (int32_t i = 0; i < p_arg_count; ++i) {
		if (!variant_to_vh(*p_args[i], arena.get(), wire[(size_t)i])) {
			return VH_ERR_ARGUMENT;
		}
	}

	vh_value result = {};
	const int32_t status = host.CallbackInvoke(
			p_callback_id, wire.empty() ? nullptr : wire.data(), p_arg_count, nullptr, &result);
	if (status == VH_OK) {
		r_result = vh_to_variant(result);
	}
	return status;
}

void VerseRuntime::release_callback(int64_t p_callback_id) {
	if (host.is_loaded() && host.CallbackRelease != nullptr) {
		host.CallbackRelease(p_callback_id);
	}
}

// The Callable goes straight into the reference table, because that is the one place the host can
// name a Godot value from: a `callable` in Verse is a godot_ref, and the id is what it holds.
int64_t VerseRuntime::api_make_callable(void *p_ctx, int64_t p_callback_id, vh_handle p_owner_handle) {
	return verse_ref_table().mint(Variant(VerseCallable::make(p_callback_id, p_owner_handle)));
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

Dictionary VerseRuntime::class_static_constants(const String &p_class_name) const {
	Dictionary out;
	if (!host.is_loaded() || host.ClassStaticList == nullptr) {
		return out;
	}
	const vh_static_desc *descs = nullptr;
	int32_t count = 0;
	if (host.ClassStaticList(p_class_name.utf8().get_data(), &descs, &count) != VH_OK) {
		return out;
	}
	for (int32_t i = 0; i < count; ++i) {
		if (descs[i].IsFunction == 0) {
			out[String::utf8(descs[i].NameUtf8, descs[i].NameLen)] = vh_to_variant(descs[i].Value);
		}
	}
	return out;
}

PackedStringArray VerseRuntime::class_static_methods(const String &p_class_name) const {
	PackedStringArray out;
	if (!host.is_loaded() || host.ClassStaticList == nullptr) {
		return out;
	}
	const vh_static_desc *descs = nullptr;
	int32_t count = 0;
	if (host.ClassStaticList(p_class_name.utf8().get_data(), &descs, &count) != VH_OK) {
		return out;
	}
	for (int32_t i = 0; i < count; ++i) {
		if (descs[i].IsFunction != 0) {
			out.push_back(String::utf8(descs[i].NameUtf8, descs[i].NameLen));
		}
	}
	return out;
}

bool VerseRuntime::class_is_abstract(const String &p_class_name) const {
	return host.is_loaded() && host.ClassIsAbstract != nullptr
			&& host.ClassIsAbstract(p_class_name.utf8().get_data()) != 0;
}

Vector<VerseSignalInfo> VerseRuntime::class_signals(const String &p_class_name) const {
	Vector<VerseSignalInfo> signals;
	if (!host.is_loaded() || host.ClassSignalList == nullptr) {
		return signals;
	}

	const vh_signal_desc *descs = nullptr;
	int32_t count = 0;
	if (host.ClassSignalList(p_class_name.utf8().get_data(), &descs, &count) != VH_OK) {
		return signals;
	}

	signals.resize(count);
	for (int32_t i = 0; i < count; ++i) {
		const vh_signal_desc &desc = descs[i];
		VerseSignalInfo &info = signals.write[i];
		info.name = StringName(String::utf8(desc.NameUtf8, desc.NameLen));
		info.reject = desc.Reject;
		info.reject_detail = String::utf8(desc.RejectDetailUtf8, desc.RejectDetailLen);
		info.line = desc.Line;
		info.column = desc.Column;
		info.args.resize(desc.ArgCount);
		for (int32_t j = 0; j < desc.ArgCount; ++j) {
			const vh_param_desc &arg = desc.Args[j];
			VerseSignalInfo::Arg &out = info.args.write[j];
			out.name = StringName(String::utf8(arg.NameUtf8, arg.NameLen));
			out.type = variant_type_for(arg.Type, arg.VariantTag);
		}
	}
	return signals;
}

// Emission is immediate on the Verse side too, which is the stated exception to "a write defers to
// commit": a handler runs before emit_signal returns, so "emit, then read what the handler changed"
// behaves the way a Godot author expects.
int32_t VerseRuntime::api_emit_signal(void *p_ctx, vh_handle p_handle, const char *p_name_utf8, int32_t p_name_len, const vh_value *p_args, int32_t p_arg_count) {
	Object *obj = UtilityFunctions::instance_from_id(p_handle);
	if (obj == nullptr) {
		return VH_CALL_DEAD_OBJECT;
	}

	// emit_signal is a vararg method, so it is reached through callv with the signal's name as the
	// first element rather than through a binding of its own.
	Array args;
	args.push_back(Variant(StringName(String::utf8(p_name_utf8, p_name_len))));
	for (int32_t i = 0; i < p_arg_count; ++i) {
		args.push_back(vh_to_variant(p_args[i]));
	}
	obj->callv("emit_signal", args);
	return VH_CALL_OK;
}

int32_t VerseRuntime::api_connect_signal(void *p_ctx, vh_handle p_handle, const char *p_name_utf8, int32_t p_name_len, const vh_value *p_target, int32_t p_flags) {
	Object *obj = UtilityFunctions::instance_from_id(p_handle);
	if (obj == nullptr || p_target == nullptr) {
		return VH_CALL_DEAD_OBJECT;
	}
	const Variant target = vh_to_variant(*p_target);
	if (target.get_type() != Variant::CALLABLE) {
		return VH_CALL_BAD_VALUE;
	}
	const StringName name(String::utf8(p_name_utf8, p_name_len));
	// No flags in 4a: one-shot belongs with Await, in the phase whose idiom wants it.
	return obj->connect(name, target, (uint32_t)p_flags) == OK ? VH_CALL_OK : VH_CALL_NO_SUCH_MEMBER;
}

int32_t VerseRuntime::api_disconnect_signal(void *p_ctx, vh_handle p_handle, const char *p_name_utf8, int32_t p_name_len, const vh_value *p_target) {
	Object *obj = UtilityFunctions::instance_from_id(p_handle);
	if (obj == nullptr || p_target == nullptr) {
		// A freed owner has already dropped every connection it had, so there is nothing to
		// disconnect and nothing wrong -- which is what makes Cancel idempotent past a free.
		return VH_CALL_OK;
	}
	const Variant target = vh_to_variant(*p_target);
	if (target.get_type() != Variant::CALLABLE) {
		return VH_CALL_BAD_VALUE;
	}
	const StringName name(String::utf8(p_name_utf8, p_name_len));
	if (obj->is_connected(name, target)) {
		obj->disconnect(name, target);
	}
	return VH_CALL_OK;
}

// What a Signal *value* names, which is what connecting to one needs and what a reference id does
// not say. The only way a script reaches a signal the mirror has no accessor for -- one a GDScript
// or C# script declared, or one made with add_user_signal (R-INT-1).
//
// The name is handed back as a pointer into `held`, which the host copies before its next call:
// the same bargain every other string this side hands over makes, and the reason it is a member
// rather than a local.
int32_t VerseRuntime::api_signal_target(void *p_ctx, int64_t p_ref, vh_handle *r_handle, const char **r_name_utf8) {
	VerseRuntime *self = static_cast<VerseRuntime *>(p_ctx);
	const Variant *found = verse_ref_table().find(p_ref);
	if (self == nullptr || found == nullptr || r_handle == nullptr || r_name_utf8 == nullptr) {
		return VH_CALL_BAD_VALUE;
	}
	if (found->get_type() != Variant::SIGNAL) {
		return VH_CALL_BAD_VALUE;
	}
	const Signal signal = *found;
	self->held_signal_name = String(signal.get_name()).utf8();
	*r_handle = (vh_handle)signal.get_object_id();
	*r_name_utf8 = self->held_signal_name.get_data();
	return VH_CALL_OK;
}

// Godot's own `Signal(object, "name")`, which has no other spelling on this wire -- the direct
// analogue of api_make_callable, and what makes a signal the mirror has no accessor for nameable
// at all rather than only receivable.
//
// Not validated against has_signal: Godot's own Signal value is not either, and a Signal naming a
// signal that does not exist fails at the connect, which is where the message is about the name.
int64_t VerseRuntime::api_make_signal_ref(void *p_ctx, vh_handle p_handle, const char *p_name_utf8, int32_t p_name_len) {
	Object *obj = UtilityFunctions::instance_from_id(p_handle);
	if (obj == nullptr) {
		return 0;
	}
	return verse_ref_table().mint(Variant(Signal(obj, StringName(String::utf8(p_name_utf8, p_name_len)))));
}

// R-SCN-3's dispatch half: a call with no object. ClassDB::class_call_static is Godot's own way of
// reaching a static without an instance, and it is what GDScript's `Tween.interpolate_value(...)`
// resolves to.
int32_t VerseRuntime::api_call_static(void *p_ctx, const char *p_class_utf8, int32_t p_class_len, const char *p_name_utf8, int32_t p_name_len, const vh_value *p_args, int32_t p_arg_count, vh_arena *p_arena, vh_value *r_value) {
	if (r_value == nullptr) {
		return VH_CALL_BAD_VALUE;
	}
	ClassDBSingleton *db = ClassDBSingleton::get_singleton();
	if (db == nullptr) {
		return VH_CALL_NO_SUCH_MEMBER;
	}
	const StringName class_name(String::utf8(p_class_utf8, p_class_len));
	const StringName method(String::utf8(p_name_utf8, p_name_len));
	if (!db->class_has_method(class_name, method, false)) {
		return VH_CALL_NO_SUCH_MEMBER;
	}

	// Through callv rather than through class_call_static, which godot-cpp binds as a variadic
	// *template*: the arguments are only known at runtime here, and the internal overload that
	// takes an array is private.
	Array args;
	args.push_back(Variant(class_name));
	args.push_back(Variant(method));
	for (int32_t i = 0; i < p_arg_count; ++i) {
		args.push_back(vh_to_variant(p_args[i]));
	}
	const Variant result = db->callv("class_call_static", args);
	return variant_to_vh(result, p_arena, *r_value) ? VH_CALL_OK : VH_CALL_BAD_VALUE;
}

// The @GlobalScope utilities. Reached through the Engine singleton's own `Callable` machinery
// rather than through a binding, because godot-cpp exposes each one as a free function and the
// bridge needs them by name.
int32_t VerseRuntime::api_call_utility(void *p_ctx, const char *p_name_utf8, int32_t p_name_len, const vh_value *p_args, int32_t p_arg_count, vh_arena *p_arena, vh_value *r_value) {
	if (r_value == nullptr) {
		return VH_CALL_BAD_VALUE;
	}
	const StringName name(String::utf8(p_name_utf8, p_name_len));

	std::vector<Variant> values((size_t)p_arg_count);
	std::vector<const Variant *> args((size_t)p_arg_count);
	for (int32_t i = 0; i < p_arg_count; ++i) {
		values[(size_t)i] = vh_to_variant(p_args[i]);
		args[(size_t)i] = &values[(size_t)i];
	}

	// A fixed table rather than a generic dispatch, and the reason is the GDExtension interface:
	// `variant_get_ptr_utility_function` hands back a *ptrcall*, which wants typed argument
	// pointers and a signature hash, so there is no by-name call that takes Variants. godot-cpp
	// binds each utility as an ordinary C++ function instead, which is what these reach.
	//
	// The **random family** is what has to be here, and it is the one exception to R-AUD-2: Verse
	// has `GetRandomFloat`, but a Verse-side RNG would silently ignore `seed()` and `randomize()`,
	// so these steer the engine's own stream. C# does the same thing for the same reason.
	// Everything else Verse already spells keeps Verse's spelling, and the rest is recorded as a
	// skip rather than being silently absent.
	Variant result;
	if (name == StringName("randf")) {
		result = UtilityFunctions::randf();
	} else if (name == StringName("randi")) {
		result = UtilityFunctions::randi();
	} else if (name == StringName("randf_range") && p_arg_count == 2) {
		result = UtilityFunctions::randf_range((double)*args[0], (double)*args[1]);
	} else if (name == StringName("randi_range") && p_arg_count == 2) {
		result = UtilityFunctions::randi_range((int64_t)*args[0], (int64_t)*args[1]);
	} else if (name == StringName("randfn") && p_arg_count == 2) {
		result = UtilityFunctions::randfn((double)*args[0], (double)*args[1]);
	} else if (name == StringName("randomize")) {
		UtilityFunctions::randomize();
	} else if (name == StringName("seed") && p_arg_count == 1) {
		UtilityFunctions::seed((int64_t)*args[0]);
	} else if (name == StringName("rand_from_seed") && p_arg_count == 1) {
		result = UtilityFunctions::rand_from_seed((int64_t)*args[0]);

		// The rest are here for the *other* reason: not that Verse's answer would be wrong, but
		// that Verse has no answer at all. Each is engine behaviour -- the editor's error list,
		// Godot's own type and error names, the object registry, the RID allocator -- and
		// GodotApi.native.verse gives each a typed Verse spelling over this call.
		//
		// The print family is vararg in Godot and takes one argument here, which is what a script
		// writes. godot-cpp binds them as variadic templates, so one argument is a legal call.
	} else if (name == StringName("push_error") && p_arg_count == 1) {
		UtilityFunctions::push_error(*args[0]);
	} else if (name == StringName("push_warning") && p_arg_count == 1) {
		UtilityFunctions::push_warning(*args[0]);
	} else if (name == StringName("print_rich") && p_arg_count == 1) {
		UtilityFunctions::print_rich(*args[0]);
	} else if (name == StringName("printerr") && p_arg_count == 1) {
		UtilityFunctions::printerr(*args[0]);
	} else if (name == StringName("print_verbose") && p_arg_count == 1) {
		UtilityFunctions::print_verbose(*args[0]);
	} else if (name == StringName("printraw") && p_arg_count == 1) {
		UtilityFunctions::printraw(*args[0]);
	} else if (name == StringName("type_string") && p_arg_count == 1) {
		result = UtilityFunctions::type_string((int64_t)*args[0]);
	} else if (name == StringName("error_string") && p_arg_count == 1) {
		result = UtilityFunctions::error_string((int64_t)*args[0]);
	} else if (name == StringName("instance_from_id") && p_arg_count == 1) {
		result = UtilityFunctions::instance_from_id((int64_t)*args[0]);
	} else if (name == StringName("is_instance_id_valid") && p_arg_count == 1) {
		result = UtilityFunctions::is_instance_id_valid((int64_t)*args[0]);
	} else if (name == StringName("rid_allocate_id")) {
		result = UtilityFunctions::rid_allocate_id();
	} else if (name == StringName("rid_from_int64") && p_arg_count == 1) {
		// A RID crosses as the integer it wraps, which is how the mirror types it everywhere else.
		result = (int64_t)UtilityFunctions::rid_from_int64((int64_t)*args[0]).get_id();
	} else {
		return VH_CALL_NO_SUCH_MEMBER;
	}
	return variant_to_vh(result, p_arena, *r_value) ? VH_CALL_OK : VH_CALL_BAD_VALUE;
}

void VerseRuntime::tick(double p_budget_seconds) {
	if (!host.is_loaded()) {
		UtilityFunctions::push_warning("VerseRuntime: tick called with no host loaded");
		return;
	}

	vh_tick_stats stats = {};
	stats.StructSize = sizeof(stats);
	host.Tick(p_budget_seconds, &stats);

	// R-ASYNC-6. A budget nobody can see the effect of is a number nobody can set, so what the pump
	// did is both readable as a custom monitor and said out loud when it runs out of time.
	last_tick_stats = stats;

	if (stats.Overran == 0) {
		overrun_frames = 0;
		return;
	}
	// Rate limited, and by a count rather than a clock: a project that is consistently over budget
	// is over budget on every frame, and one line per frame would bury every other message in the
	// output. The first says it, and then one per 600 frames -- about ten seconds at 60fps.
	if (overrun_frames % 600 == 0) {
		UtilityFunctions::push_warning(
				String("Verse: the frame budget (") + String::num(p_budget_seconds * 1000.0, 1) +
				" ms, verse/runtime/frame_budget_ms) ran out with " + String::num_int64(stats.JobsPending) +
				" queued job(s) left. They run next frame. The budget governs queued work only -- a task "
				"awaiting a Godot signal resumes inside the emission and is not budgeted.");
	}
	overrun_frames++;
}

// The two numbers worth watching, as Godot's own custom monitors: they show up in the profiler's
// Monitors tab beside the engine's, which is where someone tuning the budget is already looking.
//
// Registered lazily, on the first tick after a host is loaded, because Performance is a singleton
// the editor owns and adding a monitor twice is an error.
void VerseRuntime::register_monitors() {
	Performance *perf = Performance::get_singleton();
	if (perf == nullptr || monitors_registered) {
		return;
	}
	monitors_registered = true;
	perf->add_custom_monitor("verse/queued_jobs", Callable(this, "_monitor_queued_jobs"));
	perf->add_custom_monitor("verse/pump_ms", Callable(this, "_monitor_pump_ms"));
	perf->add_custom_monitor("verse/sleeping_tasks", Callable(this, "_monitor_sleeping_tasks"));
	// The stall the other three cannot show: analysis runs on a thread the host owns, so a frame
	// that spent 1.7 s waiting one out reports a pump that did nothing in no time at all.
	perf->add_custom_monitor("verse/analysis_wait_ms", Callable(this, "_monitor_analysis_wait_ms"));
}

double VerseRuntime::_monitor_queued_jobs() const {
	return (double)last_tick_stats.JobsPending;
}

double VerseRuntime::_monitor_pump_ms() const {
	return last_tick_stats.ElapsedSeconds * 1000.0;
}

double VerseRuntime::_monitor_sleeping_tasks() const {
	return (double)last_tick_stats.Sleeping;
}

double VerseRuntime::_monitor_analysis_wait_ms() const {
	return last_tick_stats.AnalysisWaitSeconds * 1000.0;
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
		// `_err_print_error`, not `UtilityFunctions::push_error`. They are not two spellings of one
		// thing: godot-cpp's push_error is GDScript's global, which is variadic and *concatenates*
		// its arguments, so passing (message, function, path, line) to it printed one run-together
		// string -- `...dropped.)(/Godot.org/Godot/node:)GetNameGodotClasses.native.verse28893`
		// (by-hand-findings.md B9). This one takes the four as what they are, and Godot's errors
		// panel makes the file and line clickable.
		const CharString path = String::utf8(site->PathUtf8, site->PathLen).utf8();
		const CharString function = String::utf8(site->FunctionUtf8, site->FunctionLen).utf8();
		_err_print_error(function.get_data(), path.get_data(), site->Line, message);
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

// The engine class, never the script's: a node carrying a Verse script is still an Area2D to
// Godot, and the host is the side that knows which of its instances that handle belongs to.
int32_t VerseRuntime::api_get_class_of(void *p_ctx, vh_handle p_handle, vh_arena *p_arena, vh_value *r_class_name) {
	if (r_class_name == nullptr) {
		return VH_CALL_BAD_VALUE;
	}
	Object *obj = UtilityFunctions::instance_from_id(p_handle);
	if (obj == nullptr) {
		return VH_CALL_DEAD_OBJECT;
	}
	return variant_to_vh(Variant(obj->get_class()), p_arena, *r_class_name) ? VH_CALL_OK : VH_CALL_BAD_VALUE;
}

void VerseRuntime::on_diagnostic(void *p_ctx, const vh_diagnostic *p_diagnostic) {
	const String file = p_diagnostic->FilePathLen > 0 ? String::utf8(p_diagnostic->FilePathUtf8, p_diagnostic->FilePathLen) : String("<unknown>");
	const String message = String::utf8(p_diagnostic->MessageUtf8, p_diagnostic->MessageLen);

	VerseRuntime *runtime = static_cast<VerseRuntime *>(p_ctx);
	if (runtime != nullptr && runtime->diagnostic_sink != nullptr) {
		Dictionary entry;
		entry["severity"] = p_diagnostic->Severity;
		entry["line"] = p_diagnostic->Line;
		entry["column"] = p_diagnostic->Column;
		entry["message"] = message;
		entry["path"] = file;
		// The compiler's own code for the diagnostic, which is how a caller recognises one
		// without matching on English. 3506 is ErrSemantic_UnknownIdentifier, and R-TOOL-12 is
		// built on noticing it.
		entry["code"] = p_diagnostic->ReferenceCode;

		Dictionary &sink = *runtime->diagnostic_sink;
		TypedArray<Dictionary> for_file = sink.has(file) ? TypedArray<Dictionary>(sink[file]) : TypedArray<Dictionary>();
		for_file.push_back(entry);
		sink[file] = for_file;

		// Whoever installed the sink decides what reaches the log, and every severity is theirs
		// to decide: analysis re-runs on every keystroke and every save, and a warning the
		// compiler repeats each time is the same noise as an error it repeats.
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
