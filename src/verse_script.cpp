#include "verse_script.h"

#include "verse_runtime.h"
#include "verse_script_instance.h"
#include "verse_script_language.h"

#include <gdextension_interface.h>

#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/object.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/godot.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <godot_cpp/variant/variant.hpp>

using namespace godot;

namespace {

VerseRuntime *get_runtime() {
	return Object::cast_to<VerseRuntime>(Engine::get_singleton()->get_singleton("VerseRuntime"));
}

GDExtensionInterfacePlaceholderScriptInstanceUpdate get_placeholder_instance_update_fn() {
	static GDExtensionInterfacePlaceholderScriptInstanceUpdate fn = (GDExtensionInterfacePlaceholderScriptInstanceUpdate)
			gdextension_interface::get_proc_address("placeholder_script_instance_update");
	return fn;
}

GDExtensionInterfacePlaceholderScriptInstanceCreate get_placeholder_instance_create_fn() {
	static GDExtensionInterfacePlaceholderScriptInstanceCreate fn = (GDExtensionInterfacePlaceholderScriptInstanceCreate)
			gdextension_interface::get_proc_address("placeholder_script_instance_create");
	return fn;
}

} // namespace

void VerseScript::_bind_methods() {
}

VerseScript::~VerseScript() {
	release_handle();
}

void VerseScript::release_handle() {
	if (handle == nullptr) {
		return;
	}

	VerseRuntime *runtime = get_runtime();
	if (runtime != nullptr) {
		runtime->release_script_handle(handle);
	}
	handle = nullptr;
	valid = false;
}

Error VerseScript::compile() {
	VerseRuntime *runtime = get_runtime();
	if (runtime == nullptr) {
		// Loading many .verse resources with no runtime registered (e.g. during an editor
		// filesystem scan) would otherwise spam the log once per resource.
		static bool warned = false;
		if (!warned) {
			UtilityFunctions::push_warning("VerseScript: VerseRuntime singleton is not available");
			warned = true;
		}
		return ERR_UNAVAILABLE;
	}

	VerseScriptLanguage *language = VerseScriptLanguage::singleton();
	if (language == nullptr) {
		return ERR_UNAVAILABLE;
	}

	release_handle();

	const String path = get_path();
	if (path.is_empty()) {
		return ERR_UNCONFIGURED;
	}

	// One file cannot be compiled on its own: the whole project is built together, and it is
	// built once. This script is only usable if that build succeeded outright — the linker
	// requires a complete program, so one bad file leaves nothing assembled.
	const Error build_status = language->ensure_project_built();

	// Reached on save and on reload, where the answer has to be about the text being saved. A
	// background analysis started by the last keystroke may still be in flight, and its
	// predecessor's diagnostics can describe an edit the author has already undone -- which would
	// mark this script invalid over a mistake that is no longer in the file.
	language->settle_checks();

	handle = runtime->open_script(ProjectSettings::get_singleton()->globalize_path(path));
	valid = handle != nullptr && build_status == OK && language->diagnostics_for(path).is_empty();
	class_shaped = valid && runtime->has_class(verse_class_name());

	// The export list only exists once the project has been analysed, and a placeholder created
	// before that got an empty one.
	update_placeholders();
	return valid ? OK : ERR_COMPILATION_FAILED;
}

bool VerseScript::is_class_shaped() const {
	return class_shaped;
}

String VerseScript::verse_class_name() const {
	return get_path().get_file().get_basename();
}

vh_instance *VerseScript::make_instance(int64_t p_object_id) const {
	VerseRuntime *runtime = get_runtime();
	return runtime != nullptr ? runtime->instantiate(verse_class_name(), p_object_id) : nullptr;
}

Variant VerseScript::instance_field(vh_instance *p_instance, const StringName &p_name) const {
	VerseRuntime *runtime = get_runtime();
	return runtime != nullptr ? runtime->instance_field(p_instance, String(p_name)) : Variant();
}

bool VerseScript::set_instance_field(vh_instance *p_instance, const StringName &p_name, const Variant &p_value) const {
	VerseRuntime *runtime = get_runtime();
	return runtime != nullptr && runtime->set_instance_field(p_instance, String(p_name), p_value);
}

void VerseScript::free_instance(vh_instance *p_instance) const {
	VerseRuntime *runtime = get_runtime();
	if (runtime != nullptr) {
		runtime->release_instance(p_instance);
	}
}

bool VerseScript::instance_has_function(vh_instance *p_instance, const char *p_decorated_name) const {
	VerseRuntime *runtime = get_runtime();
	return runtime != nullptr && runtime->instance_has_function(p_instance, p_decorated_name);
}

Error VerseScript::call_instance_void(vh_instance *p_instance, const char *p_decorated_name) const {
	VerseRuntime *runtime = get_runtime();
	return runtime != nullptr ? runtime->call_instance_void(p_instance, p_decorated_name) : ERR_UNAVAILABLE;
}

Error VerseScript::call_instance_void_float(vh_instance *p_instance, const char *p_decorated_name, double p_arg) const {
	VerseRuntime *runtime = get_runtime();
	return runtime != nullptr ? runtime->call_instance_void_float(p_instance, p_decorated_name, p_arg) : ERR_UNAVAILABLE;
}

bool VerseScript::is_compiled() const {
	return valid && handle != nullptr;
}

bool VerseScript::verse_has_function(const char *p_decorated_name) const {
	VerseRuntime *runtime = get_runtime();
	if (runtime == nullptr || handle == nullptr) {
		return false;
	}
	return runtime->handle_has_function(handle, p_decorated_name);
}

Error VerseScript::call_verse_void(const char *p_decorated_name) {
	VerseRuntime *runtime = get_runtime();
	if (runtime == nullptr || handle == nullptr) {
		return ERR_UNAVAILABLE;
	}
	return runtime->call_handle_void(handle, p_decorated_name);
}

Error VerseScript::call_verse_void_float(const char *p_decorated_name, double p_arg) {
	VerseRuntime *runtime = get_runtime();
	if (runtime == nullptr || handle == nullptr) {
		return ERR_UNAVAILABLE;
	}
	return runtime->call_handle_void_float(handle, p_decorated_name, p_arg);
}

bool VerseScript::_editor_can_reload_from_file() {
	return true;
}

void VerseScript::_placeholder_erased(void *p_placeholder) {
	for (size_t i = 0; i < placeholders.size(); i++) {
		if (placeholders[i] == p_placeholder) {
			placeholders.erase(placeholders.begin() + i);
			return;
		}
	}
}

StringName VerseScript::_get_doc_class_name() const {
	return StringName();
}

TypedArray<Dictionary> VerseScript::_get_documentation() const {
	return TypedArray<Dictionary>();
}

String VerseScript::_get_class_icon_path() const {
	return String();
}

Variant VerseScript::_get_script_method_argument_count(const StringName &p_method) const {
	return Variant();
}

bool VerseScript::_can_instantiate() const {
	// Without the editor check a non-tool script gets a real instance in the editor and its
	// Ready() runs while the scene is merely open, which is what Script::can_instantiate guards
	// against for GDScript. Godot falls back to a placeholder instance when this is false.
	return is_compiled() && (_is_tool() || !Engine::get_singleton()->is_editor_hint());
}

bool VerseScript::_is_valid() const {
	return valid;
}

bool VerseScript::_is_tool() const {
	return false;
}

bool VerseScript::_is_abstract() const {
	return false;
}

StringName VerseScript::_get_instance_base_type() const {
	// Real Verse base classes (node2d and friends) are Phase 3's other half, so until
	// they exist every Verse script attaches at Node.
	return "Node";
}

void *VerseScript::_instance_create(Object *p_for_object) const {
	return VerseScriptInstance::create(const_cast<VerseScript *>(this), p_for_object);
}

void *VerseScript::_placeholder_instance_create(Object *p_for_object) const {
	GDExtensionInterfacePlaceholderScriptInstanceCreate create_placeholder = get_placeholder_instance_create_fn();
	if (create_placeholder == nullptr || p_for_object == nullptr) {
		return nullptr;
	}

	VerseScriptLanguage *language = VerseScriptLanguage::singleton();
	if (language == nullptr) {
		return nullptr;
	}

	void *placeholder = create_placeholder(language->_owner, _owner, p_for_object->_owner);
	if (placeholder != nullptr) {
		placeholders.push_back(placeholder);
		const_cast<VerseScript *>(this)->update_placeholders();
	}
	return placeholder;
}

bool VerseScript::_instance_has(Object *p_object) const {
	return false;
}

bool VerseScript::_has_source_code() const {
	return !source_code.is_empty();
}

String VerseScript::_get_source_code() const {
	return source_code;
}

void VerseScript::_set_source_code(const String &p_code) {
	source_code = p_code;
}

Error VerseScript::_reload(bool p_keep_state) {
	return compile();
}

bool VerseScript::_has_method(const StringName &p_method) const {
	const char *verse_name = VerseScriptInstance::verse_name_for(p_method);
	if (verse_name == nullptr) {
		return false;
	}
	return verse_has_function(verse_name);
}

bool VerseScript::_has_static_method(const StringName &p_method) const {
	return false;
}

Dictionary VerseScript::_get_method_info(const StringName &p_method) const {
	Dictionary info;
	if (!_has_method(p_method)) {
		return info;
	}
	info["name"] = p_method;
	info["flags"] = (int64_t)METHOD_FLAG_NORMAL;
	return info;
}

ScriptLanguage *VerseScript::_get_language() const {
	return VerseScriptLanguage::singleton();
}

Ref<Script> VerseScript::_get_base_script() const {
	return Ref<Script>();
}

StringName VerseScript::_get_global_name() const {
	return StringName();
}

bool VerseScript::_inherits_script(const Ref<Script> &p_script) const {
	return false;
}

bool VerseScript::_has_script_signal(const StringName &p_signal) const {
	return false;
}

TypedArray<Dictionary> VerseScript::_get_script_signal_list() const {
	return TypedArray<Dictionary>();
}

bool VerseScript::_has_property_default_value(const StringName &p_property) const {
	return _get_property_default_value(p_property).get_type() != Variant::NIL;
}

Variant VerseScript::_get_property_default_value(const StringName &p_property) const {
	VerseRuntime *runtime = get_runtime();
	if (!class_shaped || runtime == nullptr) {
		return Variant();
	}
	return runtime->class_default_field(verse_class_name(), String(p_property));
}

// The export list is recomputed from the semantic program on every _get_script_property_list, so
// there is no cache here to invalidate -- but a placeholder holds its own copy and has to be
// handed the new one.
void VerseScript::_update_exports() {
	update_placeholders();
}

void VerseScript::update_placeholders() {
	GDExtensionInterfacePlaceholderScriptInstanceUpdate update = get_placeholder_instance_update_fn();
	if (update == nullptr || placeholders.empty()) {
		return;
	}

	const TypedArray<Dictionary> properties = _get_script_property_list();

	Dictionary values;
	for (int64_t i = 0; i < properties.size(); i++) {
		const Dictionary property = properties[i];
		// A group header is a layout marker, not a property, and has no value to report.
		if (((int64_t)property["usage"] & PROPERTY_USAGE_GROUP) != 0) {
			continue;
		}
		const StringName name = property["name"];
		values[name] = _get_property_default_value(name);
	}

	for (void *placeholder : placeholders) {
		update(placeholder, (GDExtensionConstTypePtr)&properties, (GDExtensionConstTypePtr)&values);
	}
}

TypedArray<Dictionary> VerseScript::_get_script_method_list() const {
	TypedArray<Dictionary> methods;
	static const char *const lifecycle_methods[] = { "_ready", "_process", "_physics_process" };
	for (const char *method_name : lifecycle_methods) {
		const StringName method(method_name);
		if (_has_method(method)) {
			methods.push_back(_get_method_info(method));
		}
	}
	return methods;
}

namespace {
Variant::Type variant_type_for(int64_t p_vh_type) {
	switch ((vh_type)p_vh_type) {
		case VH_TYPE_LOGIC:
			return Variant::BOOL;
		case VH_TYPE_INT:
			return Variant::INT;
		case VH_TYPE_FLOAT:
			return Variant::FLOAT;
		case VH_TYPE_STRING:
			return Variant::STRING;
		default:
			return Variant::NIL;
	}
}
Dictionary property_for(const Dictionary &p_entry, Variant::Type p_type) {
	const String clamp_min = p_entry["clamp_min"];
	const String clamp_max = p_entry["clamp_max"];
	// Godot's range hint needs both ends, and @clamp_min carries a string rather than a number,
	// so a member is only ranged when both parse. Anything else falls back to a plain field.
	const bool ranged = (p_type == Variant::FLOAT || p_type == Variant::INT) &&
			clamp_min.is_valid_float() && clamp_max.is_valid_float();

	Dictionary property;
	property["name"] = p_entry["name"];
	property["type"] = (int64_t)p_type;
	property["hint"] = (int64_t)(ranged ? PROPERTY_HINT_RANGE : PROPERTY_HINT_NONE);
	property["hint_string"] = ranged ? (clamp_min + String(",") + clamp_max) : String();
	// A non-var is editable here too, because the host applies a stored value while the instance
	// is still unsealed -- before any Verse code has run. That is initialization, not mutation, so
	// it keeps the author's `var`/non-var distinction rather than reaching around it. The
	// difference the inspector cannot show is that a non-var written *after* _ready is refused,
	// which only a remote inspector on a running game can reach.
	property["usage"] = (int64_t)(PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_SCRIPT_VARIABLE);
	return property;
}

} // namespace

TypedArray<Dictionary> VerseScript::_get_script_property_list() const {
	TypedArray<Dictionary> properties;

	VerseRuntime *runtime = get_runtime();
	if (!class_shaped || runtime == nullptr) {
		return properties;
	}

	const TypedArray<Dictionary> exports = runtime->class_exports(verse_class_name());

	// Godot has no per-property group field: a group is a PROPERTY_USAGE_GROUP entry that claims
	// every property listed after it, so members sharing an @category have to be emitted as one
	// run. The empty category goes first and stays ungrouped.
	PackedStringArray categories;
	categories.push_back(String());
	for (int64_t i = 0; i < exports.size(); i++) {
		const String category = Dictionary(exports[i])["category"];
		if (!category.is_empty() && !categories.has(category)) {
			categories.push_back(category);
		}
	}

	for (int64_t c = 0; c < categories.size(); c++) {
		const String category = categories[c];
		bool group_emitted = category.is_empty();

		for (int64_t i = 0; i < exports.size(); i++) {
			const Dictionary entry = exports[i];
			if (String(entry["category"]) != category) {
				continue;
			}

			const Variant::Type type = variant_type_for(entry["type"]);
			// A Verse type with no Variant counterpart -- char, a map, a tuple -- would reach the
			// inspector as an untyped blank that silently swallows whatever is typed into it.
			if (type == Variant::NIL) {
				continue;
			}

			if (!group_emitted) {
				Dictionary group;
				group["name"] = category;
				group["type"] = (int64_t)Variant::NIL;
				group["hint"] = (int64_t)PROPERTY_HINT_NONE;
				group["hint_string"] = String();
				group["usage"] = (int64_t)PROPERTY_USAGE_GROUP;
				properties.push_back(group);
				group_emitted = true;
			}

			properties.push_back(property_for(entry, type));
		}
	}
	return properties;
}

int32_t VerseScript::_get_member_line(const StringName &p_member) const {
	return -1;
}

Dictionary VerseScript::_get_constants() const {
	return Dictionary();
}

TypedArray<StringName> VerseScript::_get_members() const {
	return TypedArray<StringName>();
}

bool VerseScript::_is_placeholder_fallback_enabled() const {
	return false;
}

Variant VerseScript::_get_rpc_config() const {
	return Variant();
}
