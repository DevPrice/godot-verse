#include "verse_script_instance.h"

#include "verse_script.h"
#include "verse_script_language.h"

#include <godot_cpp/classes/object.hpp>
#include <godot_cpp/core/memory.hpp>
#include <godot_cpp/godot.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/typed_array.hpp>
#include <godot_cpp/variant/variant.hpp>

#include <deque>
#include <map>
#include <vector>

using namespace godot;

namespace {

struct MethodListStorage;
struct PropertyListStorage;

// One entry per outstanding get_method_list_func answer. Godot hands the array back to
// free_method_list_func and nothing else, so the array's own address is the only key available.
std::map<const GDExtensionMethodInfo *, MethodListStorage *> &method_list_storage() {
	static std::map<const GDExtensionMethodInfo *, MethodListStorage *> table;
	return table;
}

std::map<const GDExtensionPropertyInfo *, PropertyListStorage *> &property_list_storage() {
	static std::map<const GDExtensionPropertyInfo *, PropertyListStorage *> table;
	return table;
}

GDExtensionBool set_func(GDExtensionScriptInstanceDataPtr p_instance, GDExtensionConstStringNamePtr p_name, GDExtensionConstVariantPtr p_value) {
	VerseScriptInstance *self = static_cast<VerseScriptInstance *>(p_instance);
	return self->set_field(*reinterpret_cast<const StringName *>(p_name),
			*reinterpret_cast<const Variant *>(p_value));
}

GDExtensionBool get_func(GDExtensionScriptInstanceDataPtr p_instance, GDExtensionConstStringNamePtr p_name, GDExtensionVariantPtr r_ret) {
	VerseScriptInstance *self = static_cast<VerseScriptInstance *>(p_instance);
	if (self->verse_object == nullptr) {
		return false;
	}

	// Only logic, int, float and string cross the ABI, so a nil result is always a failure to
	// read rather than a member that genuinely holds nil -- Verse has no nil to hold.
	const Variant value = self->script->instance_field(self->verse_object, *reinterpret_cast<const StringName *>(p_name));
	if (value.get_type() == Variant::NIL) {
		return false;
	}

	*reinterpret_cast<Variant *>(r_ret) = value;
	return true;
}

// The strings one property list answer points at, for as long as Godot holds the array.
struct PropertyListStorage {
	std::vector<GDExtensionPropertyInfo> infos;
	// Deques for the same reason MethodListStorage uses them: a GDExtensionPropertyInfo holds the
	// address of one of these, and a vector that grew would move every string already handed over.
	std::deque<StringName> names;
	std::deque<String> hint_strings;
};

// A *live* instance's own properties, which is what the exported members are.
//
// Not the same path as the inspector's: a non-tool script gets a placeholder in the editor, and
// update_placeholders pushes the same list there. This is the one a running game walks -- which is
// what PackedScene::pack, Object::get_property_list and any reflective tool ask, so a stub here
// meant an exported member was invisible to every one of them even though get and set worked.
const GDExtensionPropertyInfo *get_property_list_func(GDExtensionScriptInstanceDataPtr p_instance, uint32_t *r_count) {
	*r_count = 0;
	VerseScriptInstance *self = static_cast<VerseScriptInstance *>(p_instance);
	if (self->script.is_null()) {
		return nullptr;
	}

	// The same list the script hands the inspector, entry for entry: one description of a member,
	// whichever kind of instance is asking.
	const TypedArray<Dictionary> exports = self->script->_get_script_property_list();
	if (exports.is_empty()) {
		return nullptr;
	}

	PropertyListStorage *storage = memnew(PropertyListStorage);
	storage->infos.reserve((size_t)exports.size());

	for (int64_t i = 0; i < exports.size(); i++) {
		const Dictionary entry = exports[i];
		storage->names.push_back(StringName(entry.get("name", String())));
		storage->names.push_back(StringName(entry.get("class_name", StringName())));
		storage->hint_strings.push_back(String(entry.get("hint_string", String())));

		GDExtensionPropertyInfo info = {};
		info.type = (GDExtensionVariantType)(int64_t)entry.get("type", (int64_t)Variant::NIL);
		info.name = (GDExtensionStringNamePtr)&storage->names[storage->names.size() - 2];
		info.class_name = (GDExtensionStringNamePtr)&storage->names.back();
		info.hint = (uint32_t)(int64_t)entry.get("hint", (int64_t)PROPERTY_HINT_NONE);
		info.hint_string = (GDExtensionStringPtr)&storage->hint_strings.back();
		info.usage = (uint32_t)(int64_t)entry.get("usage", (int64_t)PROPERTY_USAGE_DEFAULT);
		storage->infos.push_back(info);
	}

	property_list_storage()[storage->infos.data()] = storage;
	*r_count = (uint32_t)storage->infos.size();
	return storage->infos.data();
}

void free_property_list_func(GDExtensionScriptInstanceDataPtr p_instance, const GDExtensionPropertyInfo *p_list, uint32_t p_count) {
	auto &table = property_list_storage();
	const auto found = table.find(p_list);
	if (found == table.end()) {
		return;
	}
	memdelete(found->second);
	table.erase(found);
}

GDExtensionBool property_can_revert_func(GDExtensionScriptInstanceDataPtr p_instance, GDExtensionConstStringNamePtr p_name) {
	return false;
}

GDExtensionBool property_get_revert_func(GDExtensionScriptInstanceDataPtr p_instance, GDExtensionConstStringNamePtr p_name, GDExtensionVariantPtr r_ret) {
	return false;
}

GDExtensionObjectPtr get_owner_func(GDExtensionScriptInstanceDataPtr p_instance) {
	VerseScriptInstance *self = static_cast<VerseScriptInstance *>(p_instance);
	return self->owner->_owner;
}

void get_property_state_func(GDExtensionScriptInstanceDataPtr p_instance, GDExtensionScriptInstancePropertyStateAdd p_add_func, void *p_userdata) {
}

// Everything one get_method_list_func answer points at, freed as a unit by its matching
// free_method_list_func.
//
// GDExtensionMethodInfo holds bare pointers to a StringName per name and per argument, so the
// names have to outlive the call that returned them and die exactly when Godot says so. Holding
// them in one block keyed by the array Godot was handed is what makes that a single delete.
struct MethodListStorage {
	std::vector<GDExtensionMethodInfo> infos;
	std::vector<GDExtensionPropertyInfo> arguments;
	// Deques rather than vectors: a GDExtensionPropertyInfo points at one of these, and a vector
	// that grew would move every name already pointed at.
	std::deque<StringName> names;
	std::deque<String> hint_strings;
};

GDExtensionPropertyInfo make_property(MethodListStorage &r_storage, const StringName &p_name, Variant::Type p_type) {
	r_storage.names.push_back(p_name);
	r_storage.names.push_back(StringName());
	r_storage.hint_strings.push_back(String());

	GDExtensionPropertyInfo info = {};
	info.type = (GDExtensionVariantType)p_type;
	info.name = (GDExtensionStringNamePtr)&r_storage.names[r_storage.names.size() - 2];
	info.class_name = (GDExtensionStringNamePtr)&r_storage.names.back();
	info.hint = PROPERTY_HINT_NONE;
	info.hint_string = (GDExtensionStringPtr)&r_storage.hint_strings.back();
	info.usage = p_type == Variant::NIL ? (PROPERTY_USAGE_DEFAULT | PROPERTY_USAGE_NIL_IS_VARIANT)
									   : PROPERTY_USAGE_DEFAULT;
	return info;
}

const GDExtensionMethodInfo *get_method_list_func(GDExtensionScriptInstanceDataPtr p_instance, uint32_t *r_count) {
	*r_count = 0;
	VerseScriptInstance *self = static_cast<VerseScriptInstance *>(p_instance);
	if (self->script.is_null()) {
		return nullptr;
	}

	const Vector<VerseMethodInfo> &methods = self->script->methods();
	if (methods.is_empty()) {
		return nullptr;
	}

	MethodListStorage *storage = memnew(MethodListStorage);
	storage->infos.reserve((size_t)methods.size());

	// Filled to its final length before any GDExtensionPropertyInfo points into it, for the reason
	// the deques exist: a reallocation here would move arguments Godot has already been given.
	size_t total_arguments = 0;
	for (int64_t i = 0; i < methods.size(); i++) {
		total_arguments += (size_t)methods[i].params.size();
	}
	storage->arguments.reserve(total_arguments);

	for (int64_t i = 0; i < methods.size(); i++) {
		const VerseMethodInfo &method = methods[i];
		const size_t first_argument = storage->arguments.size();
		for (int64_t j = 0; j < method.params.size(); j++) {
			storage->arguments.push_back(make_property(*storage, method.params[j].name, method.params[j].type));
		}

		// The name Godot calls it by: a script method keeps its Verse spelling, and one that
		// overrides a Godot virtual is listed under Godot's name, because that is what the engine
		// will look for.
		storage->names.push_back(method.godot_virtual == StringName() ? method.name : method.godot_virtual);

		GDExtensionMethodInfo info = {};
		info.name = (GDExtensionStringNamePtr)&storage->names.back();
		info.return_value = make_property(*storage, StringName(),
				method.returns_value ? method.return_type : Variant::NIL);
		info.flags = METHOD_FLAG_NORMAL;
		info.id = 0;
		info.argument_count = (uint32_t)method.params.size();
		info.arguments = method.params.is_empty() ? nullptr : storage->arguments.data() + first_argument;
		info.default_argument_count = 0;
		info.default_arguments = nullptr;
		storage->infos.push_back(info);
	}

	method_list_storage()[storage->infos.data()] = storage;
	*r_count = (uint32_t)storage->infos.size();
	return storage->infos.data();
}

void free_method_list_func(GDExtensionScriptInstanceDataPtr p_instance, const GDExtensionMethodInfo *p_list, uint32_t p_count) {
	auto &table = method_list_storage();
	const auto found = table.find(p_list);
	if (found != table.end()) {
		memdelete(found->second);
		table.erase(found);
	}
}

GDExtensionVariantType get_property_type_func(GDExtensionScriptInstanceDataPtr p_instance, GDExtensionConstStringNamePtr p_name, GDExtensionBool *r_is_valid) {
	*r_is_valid = false;
	return GDEXTENSION_VARIANT_TYPE_NIL;
}

GDExtensionBool validate_property_func(GDExtensionScriptInstanceDataPtr p_instance, GDExtensionPropertyInfo *p_property) {
	return false;
}

GDExtensionBool has_method_func(GDExtensionScriptInstanceDataPtr p_instance, GDExtensionConstStringNamePtr p_name) {
	VerseScriptInstance *self = static_cast<VerseScriptInstance *>(p_instance);
	return self->resolve(*reinterpret_cast<const StringName *>(p_name)) != nullptr;
}

GDExtensionInt get_method_argument_count_func(GDExtensionScriptInstanceDataPtr p_instance, GDExtensionConstStringNamePtr p_name, GDExtensionBool *r_is_valid) {
	VerseScriptInstance *self = static_cast<VerseScriptInstance *>(p_instance);
	const VerseMethodInfo *method = self->resolve(*reinterpret_cast<const StringName *>(p_name));
	*r_is_valid = method != nullptr;
	return method != nullptr ? (GDExtensionInt)method->params.size() : 0;
}

void call_func(GDExtensionScriptInstanceDataPtr p_self, GDExtensionConstStringNamePtr p_method, const GDExtensionConstVariantPtr *p_args, GDExtensionInt p_argument_count, GDExtensionVariantPtr r_return, GDExtensionCallError *r_error) {
	*reinterpret_cast<Variant *>(r_return) = Variant();

	VerseScriptInstance *self = static_cast<VerseScriptInstance *>(p_self);
	const VerseMethodInfo *method = self->resolve(*reinterpret_cast<const StringName *>(p_method));
	if (method == nullptr || self->verse_object == nullptr) {
		r_error->error = GDEXTENSION_CALL_ERROR_INVALID_METHOD;
		return;
	}

	if (p_argument_count < method->required_params) {
		r_error->error = GDEXTENSION_CALL_ERROR_TOO_FEW_ARGUMENTS;
		r_error->argument = method->required_params;
		r_error->expected = method->required_params;
		return;
	}
	if (p_argument_count > method->params.size()) {
		r_error->error = GDEXTENSION_CALL_ERROR_TOO_MANY_ARGUMENTS;
		r_error->argument = (int32_t)method->params.size();
		r_error->expected = (int32_t)method->params.size();
		return;
	}

	// GDExtensionConstVariantPtr is an opaque pointer per argument, not an array of Variants, so
	// the pointers are re-pointed rather than cast through.
	std::vector<const Variant *> args((size_t)p_argument_count);
	for (int64_t i = 0; i < p_argument_count; i++) {
		args[(size_t)i] = reinterpret_cast<const Variant *>(p_args[i]);
	}

	Variant result;
	const int32_t status = self->script->call_instance(
			self->verse_object, method->decorated.get_data(),
			args.empty() ? nullptr : args.data(), (int32_t)p_argument_count, result);

	switch (status) {
		case VH_OK:
			*reinterpret_cast<Variant *>(r_return) = result;
			r_error->error = GDEXTENSION_CALL_OK;
			return;

		// A <decides> method that ran and declined. Nil is the Godot spelling of that, and it is
		// not a call error: the script answered, and its answer was "no".
		case VH_ERR_FAILED:
			r_error->error = GDEXTENSION_CALL_OK;
			return;

		case VH_ERR_ARGUMENT:
			r_error->error = GDEXTENSION_CALL_ERROR_INVALID_ARGUMENT;
			r_error->argument = 0;
			r_error->expected = (int32_t)(method->params.is_empty() ? Variant::NIL : method->params[0].type);
			return;

		case VH_ERR_NOT_FOUND:
			r_error->error = GDEXTENSION_CALL_ERROR_INVALID_METHOD;
			return;

		// Another script raised earlier this frame, so this call did not happen. Not a call error:
		// the method exists and the arguments were fine, and nothing about this node is wrong.
		// Deliberately silent -- the raise was reported once with its file, line and stack, and a
		// line per skipped call would bury it under one entry for every node in the frame. The host
		// says when everything resumes.
		case VH_ERR_HALTED:
			r_error->error = GDEXTENSION_CALL_OK;
			return;

		// Called from a thread other than the one Verse runs on (R-ASYNC-8). The host ran nothing
		// and has already said so with the method named; this is a call error because, unlike a
		// halted frame, the caller did something wrong and can fix it -- marshal the call back to
		// the main thread.
		case VH_ERR_THREAD:
			r_error->error = GDEXTENSION_CALL_ERROR_INVALID_METHOD;
			return;

		// A raise in this call. It has already been reported with its file, line and Verse stack
		// through the runtime error callback, so saying anything more here would only duplicate it
		// -- and the call did happen, so it is not a call error either.
		default:
			r_error->error = GDEXTENSION_CALL_OK;
			return;
	}
}

void notification_func(GDExtensionScriptInstanceDataPtr p_instance, int32_t p_what, GDExtensionBool p_reversed) {
}

void to_string_func(GDExtensionScriptInstanceDataPtr p_instance, GDExtensionBool *r_is_valid, GDExtensionStringPtr r_out) {
	*r_is_valid = false;
}

void refcount_incremented_func(GDExtensionScriptInstanceDataPtr p_instance) {
}

GDExtensionBool refcount_decremented_func(GDExtensionScriptInstanceDataPtr p_instance) {
	return true;
}

GDExtensionObjectPtr get_script_func(GDExtensionScriptInstanceDataPtr p_instance) {
	VerseScriptInstance *self = static_cast<VerseScriptInstance *>(p_instance);
	return self->script.ptr()->_owner;
}

GDExtensionBool is_placeholder_func(GDExtensionScriptInstanceDataPtr p_instance) {
	return false;
}

GDExtensionScriptLanguagePtr get_language_func(GDExtensionScriptInstanceDataPtr p_instance) {
	VerseScriptLanguage *language = VerseScriptLanguage::singleton();
	return language != nullptr ? language->_owner : nullptr;
}

void free_func(GDExtensionScriptInstanceDataPtr p_instance) {
	// Godot owns this pointer and frees it exactly once, when the owner drops the script;
	// nothing else may delete the instance.
	VerseScriptInstance *self = static_cast<VerseScriptInstance *>(p_instance);
	if (VerseScriptLanguage *language = VerseScriptLanguage::singleton()) {
		language->unregister_instance(self->owner_id);
	}
	if (self->verse_object != nullptr) {
		self->script->free_instance(self->verse_object);
	}
	memdelete(self);
}

const GDExtensionScriptInstanceInfo3 script_instance_info = {
	.set_func = set_func,
	.get_func = get_func,
	.get_property_list_func = get_property_list_func,
	.free_property_list_func = free_property_list_func,
	.get_class_category_func = nullptr,
	.property_can_revert_func = property_can_revert_func,
	.property_get_revert_func = property_get_revert_func,
	.get_owner_func = get_owner_func,
	.get_property_state_func = get_property_state_func,
	.get_method_list_func = get_method_list_func,
	.free_method_list_func = free_method_list_func,
	.get_property_type_func = get_property_type_func,
	.validate_property_func = validate_property_func,
	.has_method_func = has_method_func,
	.get_method_argument_count_func = get_method_argument_count_func,
	.call_func = call_func,
	.notification_func = notification_func,
	.to_string_func = to_string_func,
	.refcount_incremented_func = refcount_incremented_func,
	.refcount_decremented_func = refcount_decremented_func,
	.get_script_func = get_script_func,
	.is_placeholder_func = is_placeholder_func,
	.set_fallback_func = set_func,
	.get_fallback_func = get_func,
	.get_language_func = get_language_func,
	.free_func = free_func,
};

} // namespace

const VerseMethodInfo *VerseScriptInstance::resolve(const StringName &p_name) const {
	const VerseMethodInfo *method = script.is_valid() ? script->find_method(p_name) : nullptr;
	if (method == nullptr) {
		return nullptr;
	}
	// A Godot virtual resolves on every instance, because the mirrored class it derives from gives
	// it an empty body -- so "does the class declare it" is not the question Godot is asking.
	// Whether *this* instance overrides it is, and that is what decides the process list.
	const bool *found = implemented.getptr(method->name);
	return found != nullptr && *found ? method : nullptr;
}

GDExtensionScriptInstancePtr VerseScriptInstance::create(VerseScript *p_script, Object *p_owner) {
	if (p_script == nullptr || p_owner == nullptr || !p_script->is_compiled()) {
		return nullptr;
	}

	static GDExtensionInterfaceScriptInstanceCreate3 create3 = (GDExtensionInterfaceScriptInstanceCreate3)(void *)gdextension_interface::get_proc_address("script_instance_create3");
	if (create3 == nullptr) {
		return nullptr;
	}

	VerseScriptInstance *instance = memnew(VerseScriptInstance);
	instance->script = Ref<VerseScript>(p_script);
	instance->owner = p_owner;
	instance->owner_id = (int64_t)p_owner->get_instance_id();

	instance->verse_object = p_script->make_instance(instance->owner_id);
	if (instance->verse_object == nullptr) {
		memdelete(instance);
		return nullptr;
	}
	// Asked once per method at attach time rather than per call. A method the class declares but
	// only inherits is recorded as absent, which is what keeps a node that overrides none of the
	// per-frame virtuals out of the process list.
	const Vector<VerseMethodInfo> &methods = p_script->methods();
	for (int64_t i = 0; i < methods.size(); i++) {
		const VerseMethodInfo &method = methods[i];
		// Only a virtual can be inherited-but-not-implemented; a method the script declares itself
		// is implemented by definition, and asking the host would answer the same at more cost.
		const bool implemented = method.godot_virtual == StringName()
				|| p_script->instance_has_function(instance->verse_object, method.decorated.get_data());
		instance->implemented.insert(method.name, implemented);
	}

	if (VerseScriptLanguage *language = VerseScriptLanguage::singleton()) {
		language->register_instance(instance->owner_id, instance);
	}

	return create3(&script_instance_info, instance);
}

bool VerseScriptInstance::set_field(const StringName &p_name, const Variant &p_value) {
	if (verse_object == nullptr) {
		return false;
	}
	if (p_value.get_type() != Variant::OBJECT) {
		return script->set_instance_field(verse_object, p_name, p_value);
	}

	Object *target = p_value;

	// Where the target carries a Verse script of its own, the object to hold is the one that script
	// already built for it. Handing the handle over instead would have the host construct a second
	// Verse object around the same node: two sets of members, and nothing to say which of them the
	// author is looking at. A failure here is a class the member cannot hold, which the handle path
	// would not fix -- it would only write the same wrong thing less visibly.
	VerseScriptLanguage *language = VerseScriptLanguage::singleton();
	VerseScriptInstance *held = language != nullptr && target != nullptr
			? language->instance_for((int64_t)target->get_instance_id())
			: nullptr;

	const bool wrote = held != nullptr
			? script->set_instance_field_instance(verse_object, p_name, held->verse_object)
			: script->set_instance_field(verse_object, p_name, p_value);
	if (!wrote) {
		return false;
	}

	if (Resource *resource = Object::cast_to<Resource>(target)) {
		held_resources[p_name] = Ref<Resource>(resource);
	} else {
		held_resources.erase(p_name);
	}
	return true;
}
