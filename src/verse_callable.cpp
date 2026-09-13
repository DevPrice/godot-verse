#include "verse_callable.h"

#include "verse_runtime.h"
#include "verse_value.h"

#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/object.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <godot_cpp/variant/variant.hpp>

#include <vector>

using namespace godot;

namespace {
VerseRuntime *verse_runtime() {
	return Object::cast_to<VerseRuntime>(Engine::get_singleton()->get_singleton("VerseRuntime"));
}
}

Callable VerseCallable::make(int64_t p_callback_id, int64_t p_owner_id) {
	return Callable(memnew(VerseCallable(p_callback_id, p_owner_id)));
}

VerseCallable::~VerseCallable() {
	if (VerseRuntime *runtime = verse_runtime()) {
		runtime->release_callback(callback_id);
	}
}

uint32_t VerseCallable::hash() const {
	// The id, which is what identity means here: two Callables over one id are the same
	// subscription, and two Subscribes of one method are two ids and so two subscriptions.
	return (uint32_t)(callback_id ^ (callback_id >> 32));
}

String VerseCallable::get_as_text() const {
	return String("Verse callback #") + String::num_int64(callback_id);
}

bool VerseCallable::compare_equal(const CallableCustom *p_a, const CallableCustom *p_b) {
	return p_a == p_b;
}

bool VerseCallable::compare_less(const CallableCustom *p_a, const CallableCustom *p_b) {
	return p_a < p_b;
}

CallableCustom::CompareEqualFunc VerseCallable::get_compare_equal_func() const {
	return &VerseCallable::compare_equal;
}

CallableCustom::CompareLessFunc VerseCallable::get_compare_less_func() const {
	return &VerseCallable::compare_less;
}

bool VerseCallable::is_valid() const {
	// ObjectDB's answer, not ours. This is the whole reason the bound case is the one 4a accepts:
	// Godot drops a freed object's connections on its own, so nothing here tracks lifetimes.
	return UtilityFunctions::is_instance_id_valid(owner_id);
}

ObjectID VerseCallable::get_object() const {
	return ObjectID((uint64_t)owner_id);
}

void VerseCallable::call(const Variant **p_arguments, int p_argcount, Variant &r_return_value, GDExtensionCallError &r_call_error) const {
	r_return_value = Variant();
	r_call_error.error = GDEXTENSION_CALL_OK;

	VerseRuntime *runtime = verse_runtime();
	if (runtime == nullptr) {
		r_call_error.error = GDEXTENSION_CALL_ERROR_INSTANCE_IS_NULL;
		return;
	}

	const int32_t status = runtime->invoke_callback(callback_id, p_arguments, p_argcount, r_return_value);
	switch (status) {
		case VH_OK:
			return;

		// A <decides> handler that ran and declined. Nil is the Godot spelling of that, and it is
		// not a call error: the handler answered, and its answer was "no".
		case VH_ERR_FAILED:
			return;

		case VH_ERR_ARGUMENT:
			r_call_error.error = GDEXTENSION_CALL_ERROR_INVALID_ARGUMENT;
			return;

		// The script raised, or the frame is halted behind an earlier raise, or the node has gone.
		// Each has already been reported where it happened; making it a call error too would turn
		// one script's mistake into a second error against whoever emitted the signal.
		default:
			return;
	}
}
