#include "verse_script_instance.h"

#include "verse_script.h"
#include "verse_script_language.h"

#include <godot_cpp/classes/object.hpp>
#include <godot_cpp/core/memory.hpp>
#include <godot_cpp/godot.hpp>
#include <godot_cpp/variant/variant.hpp>

using namespace godot;

namespace {

constexpr const char *kVerseReadyName = "Ready";
constexpr const char *kVerseProcessName = "Update(:float)";
constexpr const char *kVersePhysicsProcessName = "PhysicsUpdate(:float)";

GDExtensionBool set_func(GDExtensionScriptInstanceDataPtr p_instance, GDExtensionConstStringNamePtr p_name, GDExtensionConstVariantPtr p_value) {
	return false;
}

GDExtensionBool get_func(GDExtensionScriptInstanceDataPtr p_instance, GDExtensionConstStringNamePtr p_name, GDExtensionVariantPtr r_ret) {
	return false;
}

const GDExtensionPropertyInfo *get_property_list_func(GDExtensionScriptInstanceDataPtr p_instance, uint32_t *r_count) {
	*r_count = 0;
	return nullptr;
}

void free_property_list_func(GDExtensionScriptInstanceDataPtr p_instance, const GDExtensionPropertyInfo *p_list, uint32_t p_count) {
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

const GDExtensionMethodInfo *get_method_list_func(GDExtensionScriptInstanceDataPtr p_instance, uint32_t *r_count) {
	*r_count = 0;
	return nullptr;
}

void free_method_list_func(GDExtensionScriptInstanceDataPtr p_instance, const GDExtensionMethodInfo *p_list, uint32_t p_count) {
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
	const char *verse_name = VerseScriptInstance::verse_name_for(*reinterpret_cast<const StringName *>(p_name));
	if (verse_name == kVerseReadyName) {
		return self->has_ready;
	}
	if (verse_name == kVerseProcessName) {
		return self->has_process;
	}
	if (verse_name == kVersePhysicsProcessName) {
		return self->has_physics_process;
	}
	return false;
}

GDExtensionInt get_method_argument_count_func(GDExtensionScriptInstanceDataPtr p_instance, GDExtensionConstStringNamePtr p_name, GDExtensionBool *r_is_valid) {
	*r_is_valid = false;
	return 0;
}

void call_func(GDExtensionScriptInstanceDataPtr p_self, GDExtensionConstStringNamePtr p_method, const GDExtensionConstVariantPtr *p_args, GDExtensionInt p_argument_count, GDExtensionVariantPtr r_return, GDExtensionCallError *r_error) {
	*reinterpret_cast<Variant *>(r_return) = Variant();

	VerseScriptInstance *self = static_cast<VerseScriptInstance *>(p_self);
	const char *verse_name = VerseScriptInstance::verse_name_for(*reinterpret_cast<const StringName *>(p_method));
	if (verse_name == nullptr) {
		r_error->error = GDEXTENSION_CALL_ERROR_INVALID_METHOD;
		return;
	}

	if (verse_name == kVerseReadyName) {
		self->script->call_verse_void(verse_name);
	} else {
		if (p_argument_count < 1) {
			r_error->error = GDEXTENSION_CALL_ERROR_TOO_FEW_ARGUMENTS;
			r_error->argument = 0;
			r_error->expected = 1;
			return;
		}
		const double delta = *reinterpret_cast<const Variant *>(p_args[0]);
		self->script->call_verse_void_float(verse_name, delta);
	}

	r_error->error = GDEXTENSION_CALL_OK;
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
	memdelete(static_cast<VerseScriptInstance *>(p_instance));
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

const char *VerseScriptInstance::verse_name_for(const StringName &p_method) {
	// The decoration is the host's, not Godot's: a plain Update(Delta:float) in Verse source is
	// stored as Update(:float), so these right-hand sides must match the host's mangling exactly
	// or resolution silently fails instead of erroring.
	if (p_method == StringName("_ready")) {
		return kVerseReadyName;
	}
	if (p_method == StringName("_process")) {
		return kVerseProcessName;
	}
	if (p_method == StringName("_physics_process")) {
		return kVersePhysicsProcessName;
	}
	return nullptr;
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
	instance->has_ready = p_script->verse_has_function(kVerseReadyName);
	instance->has_process = p_script->verse_has_function(kVerseProcessName);
	instance->has_physics_process = p_script->verse_has_function(kVersePhysicsProcessName);

	return create3(&script_instance_info, instance);
}
