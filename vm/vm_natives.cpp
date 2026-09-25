#include "vm_natives.h"

#include <cmath>
#include <string>

#include "vm_interpreter.h"
#include "vm_number.h"
#include "vm_sidecar.h"
#include "vm_values.h"

namespace vm {

namespace {

struct NativeBinding {
	const char *key;
	NativeFn implementation;
};

Value argument(const NativeCall &p_call, uint32_t p_index) {
	return follow(p_call.arguments[p_index]);
}

// spec/natives.md §3.6: a native reads an argument only once it is concrete.
bool unbound_argument(NativeCall &r_call) {
	for (uint32_t index = 0; index < r_call.argument_count; ++index) {
		const Value value = argument(r_call, index);
		if (is_unbound_placeholder(value)) {
			r_call.result = value;
			return true;
		}
	}
	return false;
}

Outcome to_string_native(NativeCall &r_call) {
	if (unbound_argument(r_call)) {
		return Outcome::Park;
	}
	Value value = argument(r_call, 0);
	if (value.is_float()) {
		value = Value::from_float(canonical_float(value.as_float()));
	}
	return value_to_string(r_call.heap, value, r_call.result, r_call.error);
}

// spec/natives.md §5.5.
Outcome err_native(NativeCall &r_call) {
	if (unbound_argument(r_call)) {
		return Outcome::Park;
	}
	std::string message;
	if (!string_bytes(argument(r_call, 0), message)) {
		return Outcome::Invalid;
	}
	r_call.error.diagnostic = "ErrorRequested";
	r_call.error.description = "A runtime error was explicitly raised from user code.";
	r_call.error.message = "User Message: '" + message + "'";
	return Outcome::Error;
}

// spec/natives.md §4: Abs of an integer is exact, of a float its sign bit cleared.
Outcome abs_native(NativeCall &r_call) {
	if (unbound_argument(r_call)) {
		return Outcome::Park;
	}
	const Value value = argument(r_call, 0);
	if (value.is_float()) {
		r_call.result = Value::from_float(std::fabs(value.as_float()));
		return Outcome::Ok;
	}
	if (!is_int(value)) {
		return Outcome::Invalid;
	}
	bool negative = false;
	value_order(OrderOp::Lt, value, Value::from_int32(0), negative);
	if (!negative) {
		r_call.result = value;
		return Outcome::Ok;
	}
	return value_neg(r_call.heap, value, r_call.result);
}

Outcome floor_intrinsic(NativeCall &r_call) {
	if (unbound_argument(r_call)) {
		return Outcome::Park;
	}
	return rational_floor(r_call.heap, argument(r_call, 0), r_call.result);
}

Outcome ceil_intrinsic(NativeCall &r_call) {
	if (unbound_argument(r_call)) {
		return Outcome::Park;
	}
	return rational_ceil(r_call.heap, argument(r_call, 0), r_call.result);
}

Outcome float_rounding(NativeCall &r_call, FloatRounding p_rounding) {
	if (unbound_argument(r_call)) {
		return Outcome::Park;
	}
	const Value value = argument(r_call, 0);
	if (!value.is_float()) {
		return Outcome::Invalid;
	}
	return float_to_int(r_call.heap, canonical_float(value.as_float()), p_rounding, r_call.result, r_call.error);
}

Outcome floor_float(NativeCall &r_call) {
	return float_rounding(r_call, FloatRounding::Floor);
}

Outcome ceil_float(NativeCall &r_call) {
	return float_rounding(r_call, FloatRounding::Ceil);
}

Outcome int_float(NativeCall &r_call) {
	return float_rounding(r_call, FloatRounding::Truncate);
}

// spec/natives.md §5.2: Round never raises -- the measured 64-bit conversion answers -2^63 at
// exactly 2^63 and 0 beyond either end.
Outcome round_float(NativeCall &r_call) {
	if (unbound_argument(r_call)) {
		return Outcome::Park;
	}
	const Value value = argument(r_call, 0);
	if (!value.is_float()) {
		return Outcome::Invalid;
	}
	const double number = value.as_float();
	if (!std::isfinite(number)) {
		return Outcome::Fail;
	}
	const double rounded = std::rint(number);
	if (rounded >= -9223372036854775808.0 && rounded < 9223372036854775808.0) {
		r_call.result = make_int(r_call.heap, int64_t(rounded));
	} else if (rounded == 9223372036854775808.0) {
		r_call.result = make_int(r_call.heap, INT64_MIN);
	} else {
		r_call.result = Value::from_int32(0);
	}
	return Outcome::Ok;
}

Outcome quotient_native(NativeCall &r_call) {
	if (unbound_argument(r_call)) {
		return Outcome::Park;
	}
	return euclidean_quotient(r_call.heap, argument(r_call, 0), argument(r_call, 1), r_call.result, r_call.error);
}

Outcome mod_native(NativeCall &r_call) {
	if (unbound_argument(r_call)) {
		return Outcome::Park;
	}
	return euclidean_mod(r_call.heap, argument(r_call, 0), argument(r_call, 1), r_call.result, r_call.error);
}

Outcome concatenate_maps_native(NativeCall &r_call) {
	if (unbound_argument(r_call)) {
		return Outcome::Park;
	}
	return concatenate_maps(r_call.heap, argument(r_call, 0), argument(r_call, 1), r_call.result);
}

// godot-natives.md §8.1: deferred to the root commit.
Outcome print_native(NativeCall &r_call) {
	if (unbound_argument(r_call)) {
		return Outcome::Park;
	}
	std::string text;
	if (!string_bytes(argument(r_call, 0), text)) {
		return Outcome::Invalid;
	}
	Interpreter *interpreter = r_call.interpreter;
	if (interpreter != nullptr && interpreter->godot.Print != nullptr) {
		const vh_godot_api api = interpreter->godot;
		interpreter->defer([api, text]() {
			api.Print(api.Ctx, text.data(), int32_t(text.size()));
		});
	}
	r_call.result = r_call.heap.false_value();
	return Outcome::Ok;
}

const ClassCell *superclass_of(const ClassCell *p_class) {
	if (p_class->inherited.empty()) {
		return nullptr;
	}
	const ClassCell *first = p_class->inherited.front();
	return first != nullptr && first->class_kind != ClassKind::Interface ? first : nullptr;
}

// godot-natives.md §4.3: the first ancestor that names a Godot class, a binding before the
// mirror. Empty when the walk reaches vh_object, which names none.
std::string mint_class_name(const Interpreter &p_interpreter, const ClassCell *p_class) {
	for (const ClassCell *current = p_class; current != nullptr; current = superclass_of(current)) {
		const ClassIndexEntry *entry = p_interpreter.program.entry_for(current);
		if (entry == nullptr) {
			continue;
		}
		if (entry->origin == ClassOrigin::Binding && p_interpreter.sidecar != nullptr) {
			if (const SidecarBinding *binding = p_interpreter.sidecar->find_binding(entry->name->text)) {
				return binding->godot.empty() ? binding->script : binding->godot;
			}
		}
		if (entry->origin == ClassOrigin::Mirrored && p_interpreter.mirrored_godot_name != nullptr) {
			if (const char *name = p_interpreter.mirrored_godot_name(entry->name->text)) {
				return name;
			}
		}
	}
	return std::string();
}

// godot-natives.md §8.12: adopt the handle the host is constructing this object for, or mint.
Outcome adopt_or_mint_native(NativeCall &r_call) {
	if (unbound_argument(r_call)) {
		return Outcome::Park;
	}
	const Value object = argument(r_call, 0);
	if (!is_cell_kind(object, CellKind::Object)) {
		return Outcome::Invalid;
	}
	Interpreter *interpreter = r_call.interpreter;
	if (interpreter == nullptr) {
		r_call.result = Value::from_int32(0);
		return Outcome::Ok;
	}
	if (interpreter->adopting_object == object.as_cell()) {
		const int64_t handle = interpreter->adopting_handle;
		interpreter->adopting_object = nullptr;
		interpreter->adopting_handle = 0;
		r_call.result = make_int(r_call.heap, handle);
		return Outcome::Ok;
	}
	const std::string name = mint_class_name(*interpreter, cell_as<ObjectCell>(object)->object_class);
	if (name.empty() || interpreter->godot.InstantiateClass == nullptr) {
		r_call.result = Value::from_int32(0);
		return Outcome::Ok;
	}
	const vh_handle handle = interpreter->godot.InstantiateClass(interpreter->godot.Ctx, name.data(), int32_t(name.size()));
	if (handle == 0) {
		r_call.error.diagnostic = "ErrRuntime_NativeInternal";
		r_call.error.description = "An internal runtime error occurred in native code that was called from Verse. There is no other information available.";
		r_call.error.message = "Godot would not make a `" + name +
				"`, so this class has no object to be. A Godot class that is abstract, or that the engine only ever hands out as a singleton, cannot be constructed -- derive from one that can, or reach the singleton through its accessor.";
		return Outcome::Error;
	}
	// T3.5 registers the discard compensation here; T3.9 releases the peer when the object dies.
	interpreter->minted_peers[handle] = object.as_cell();
	r_call.result = make_int(r_call.heap, handle);
	return Outcome::Ok;
}

constexpr NativeBinding kNatives[] = {
	{ "(/Verse.org/Verse/(/Verse.org/Verse:)ToString(:int):)Native", &to_string_native },
	{ "(/Verse.org/Verse/(/Verse.org/Verse:)ToString(:float):)Native", &to_string_native },
	{ "(/Verse.org/Verse/(/Verse.org/Verse:)ToString(:char):)Native", &to_string_native },
	{ "(/Verse.org/Verse/(/Verse.org/Verse:)ToString(:char32):)Native", &to_string_native },
	{ "(/Verse.org/Verse/(/Verse.org/Verse:)Err(:[]char):)Native", &err_native },
	{ "(/Verse.org/Verse/(/Verse.org/Verse:)Abs:)Native", &abs_native },
	{ "(/Verse.org/Verse/(/Verse.org/Verse:)Floor:)Native", &floor_intrinsic },
	{ "(/Verse.org/Verse/(/Verse.org/Verse:)Ceil:)Native", &ceil_intrinsic },
	{ "(/Verse.org/Verse/(/Verse.org/Verse:)Floor(:float):)Native", &floor_float },
	{ "(/Verse.org/Verse/(/Verse.org/Verse:)Ceil(:float):)Native", &ceil_float },
	{ "(/Verse.org/Verse/(/Verse.org/Verse:)Round(:float):)Native", &round_float },
	{ "(/Verse.org/Verse/(/Verse.org/Verse:)Int(:float):)Native", &int_float },
	{ "(/Verse.org/Verse/(/Verse.org/Verse:)Quotient(:int,:int):)Native", &quotient_native },
	{ "(/Verse.org/Verse/(/Verse.org/Verse:)Mod(:int,:int):)Native", &mod_native },
	{ "(/Verse.org/Verse/(/Verse.org/Verse:)ConcatenateMaps:)Native", &concatenate_maps_native },
	{ "(/Godot.org/Godot/(/Godot.org/Godot:)Print(:[]char):)Native", &print_native },
	{ "(/Godot.org/Godot/(/Godot.org/Godot:)VhAdoptOrMint(:(/Godot.org/Godot:)vh_object):)Native", &adopt_or_mint_native },
};

} // namespace

const char *const kMissingProcedureKey = "(/Verse.org/Verse/(/Verse.org/Verse:)MissingProcedure:)Native";
const char *const kMissingProcedureName = "(/Verse.org/Verse:)MissingProcedure";

NativeFn native_implementation(std::string_view p_binding_key) {
	for (const NativeBinding &binding : kNatives) {
		if (p_binding_key == binding.key) {
			return binding.implementation;
		}
	}
	return nullptr;
}

Outcome native_not_implemented(NativeCall &r_call) {
	r_call.error.diagnostic = "ErrRuntime_NativeInternal";
	r_call.error.description = "An internal runtime error occurred in native code that was called from Verse. There is no other information available.";
	r_call.error.message = "The native function " + r_call.procedure->binding_key->text + " is not implemented by this runtime.";
	return Outcome::Error;
}

Outcome native_missing_procedure(NativeCall &r_call) {
	r_call.error.diagnostic = "ErrRuntime_InvalidFunctionCall";
	r_call.error.description = "Attempted to call an invalid function.";
	r_call.error.message = "Attempted to call an uninitialized function.";
	return Outcome::Error;
}

} // namespace vm
