#include "vm_natives.h"

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "vm_interpreter.h"
#include "vm_loader.h"
#include "vm_math.h"
#include "vm_number.h"
#include "vm_objects.h"
#include "vm_sidecar.h"
#include "vm_values.h"

namespace vm {

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

namespace {

struct NativeBinding {
	const char *key;
	NativeFn implementation;
};

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
	if (interpreter->mint_suppressed || name.empty() || interpreter->godot.InstantiateClass == nullptr) {
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
	interpreter->record_mint(handle, object.as_cell());
	r_call.result = make_int(r_call.heap, handle);
	return Outcome::Ok;
}

// spec/natives.md §3.6: an array or map argument's own elements are checked for concreteness as
// they are converted, not up front with the rest of the call's arguments (unbound_argument covers
// only the call's own positional arguments).
bool element_unbound(NativeCall &r_call, Value p_element) {
	const Value followed = follow(p_element);
	if (is_unbound_placeholder(followed)) {
		r_call.result = followed;
		return true;
	}
	return false;
}

// spec/natives.md §5.4.
Outcome join_strings_native(NativeCall &r_call) {
	if (unbound_argument(r_call)) {
		return Outcome::Park;
	}
	const Value strings = argument(r_call, 0);
	std::string separator;
	if (!string_bytes(argument(r_call, 1), separator)) {
		return Outcome::Invalid;
	}
	int64_t length = 0;
	if (value_length(strings, length) != Outcome::Ok) {
		return Outcome::Invalid;
	}
	std::string joined;
	for (int64_t index = 0; index < length; ++index) {
		Value element;
		if (array_index(strings, make_int(r_call.heap, index), element) != Outcome::Ok) {
			return Outcome::Invalid;
		}
		if (element_unbound(r_call, element)) {
			return Outcome::Park;
		}
		std::string piece;
		if (!string_bytes(follow(element), piece)) {
			return Outcome::Invalid;
		}
		if (index > 0) {
			joined += separator;
		}
		joined += piece;
		if (joined.size() > size_t(INT32_MAX)) {
			r_call.error.diagnostic = "ErrRuntime_InvalidArrayLength";
			r_call.error.description = "Invalid array length.";
			r_call.error.message = "Invalid array length.";
			return Outcome::Error;
		}
	}
	r_call.result = make_string(r_call.heap, joined);
	return Outcome::Ok;
}

// spec/natives.md §5.5: a warning, not an error. Nothing rolls back and nothing reaches the
// embedder's runtime-error callback; stderr is this runtime's substitute for Epic's engine-log-only
// sink.
Outcome warn_native(NativeCall &r_call) {
	if (unbound_argument(r_call)) {
		return Outcome::Park;
	}
	std::string message;
	if (!string_bytes(argument(r_call, 0), message)) {
		return Outcome::Invalid;
	}
	std::fprintf(stderr, "WarningRequested: A runtime warning was explicitly raised from user code. (User Message: '%s')\n", message.c_str());
	r_call.result = r_call.heap.false_value();
	return Outcome::Ok;
}

// spec/natives.md §4: the argument is [key type, value type]; format.md §4's simple-type code 11.
Outcome weak_map_native(NativeCall &r_call) {
	if (unbound_argument(r_call)) {
		return Outcome::Park;
	}
	const Value types = argument(r_call, 0);
	int64_t length = 0;
	Value key_type;
	Value value_type;
	if (value_length(types, length) != Outcome::Ok || length != 2 ||
			array_index(types, Value::from_int32(0), key_type) != Outcome::Ok ||
			array_index(types, Value::from_int32(1), value_type) != Outcome::Ok) {
		return Outcome::Invalid;
	}
	SimpleTypeCell *cell = r_call.heap.make<SimpleTypeCell>();
	cell->code = 11;
	cell->components = { key_type, value_type };
	r_call.result = Value::from_cell(cell);
	return Outcome::Ok;
}

// spec/natives.md §5.6, §11.1 Q2: frozen at first use rather than at vh_init, since this file has
// no init hook to freeze it at (Runtime::boot, vm_runtime.cpp, is where that would belong once this
// is routed there). One process-lifetime sample matches the observable contract either way: this
// host runs no engine frame to refresh it against.
double frozen_epoch_seconds() {
	static const double epoch = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
	return epoch;
}

Outcome get_seconds_since_epoch_native(NativeCall &r_call) {
	r_call.result = Value::from_float(frozen_epoch_seconds());
	return Outcome::Ok;
}

// spec/natives.md §6: seeded from OS entropy at first use, not reproducible, and its advance is not
// undone by a failing transaction (§3.8) -- which a plain local variable already gives for free.
std::mt19937_64 &random_generator() {
	static std::mt19937_64 generator(std::random_device{}());
	return generator;
}

// spec/natives.md §5.11 and §6 share this rule for the NaN/Inf edges: p if p<q else q, and p if
// q<p else q -- not std::min/std::max, whose NaN behaviour differs by argument order.
double verse_min(double p_left, double p_right) { return p_left < p_right ? p_left : p_right; }
double verse_max(double p_left, double p_right) { return p_right < p_left ? p_left : p_right; }

Outcome get_random_int_native(NativeCall &r_call) {
	if (unbound_argument(r_call)) {
		return Outcome::Park;
	}
	int64_t low = 0;
	int64_t high = 0;
	Outcome outcome = int_to_int64(argument(r_call, 0), low, r_call.error);
	if (outcome != Outcome::Ok) {
		return outcome;
	}
	outcome = int_to_int64(argument(r_call, 1), high, r_call.error);
	if (outcome != Outcome::Ok) {
		return outcome;
	}
	if (low == high) {
		r_call.result = make_int(r_call.heap, low);
		return Outcome::Ok;
	}
	if (low > high) {
		std::swap(low, high);
	}
	const uint64_t range = uint64_t(high) - uint64_t(low);
	uint64_t mask = range;
	mask |= mask >> 1;
	mask |= mask >> 2;
	mask |= mask >> 4;
	mask |= mask >> 8;
	mask |= mask >> 16;
	mask |= mask >> 32;
	uint64_t draw;
	do {
		draw = random_generator()();
	} while ((draw & mask) > range);
	r_call.result = make_int(r_call.heap, int64_t(uint64_t(low) + (draw & mask)));
	return Outcome::Ok;
}

Outcome get_random_float_native(NativeCall &r_call) {
	if (unbound_argument(r_call)) {
		return Outcome::Park;
	}
	const Value low_value = argument(r_call, 0);
	const Value high_value = argument(r_call, 1);
	if (!low_value.is_float() || !high_value.is_float()) {
		return Outcome::Invalid;
	}
	double low = low_value.as_float();
	double high = high_value.as_float();
	if (low == high) {
		r_call.result = Value::from_float(low);
		return Outcome::Ok;
	}
	if (low > high) {
		std::swap(low, high);
	}
	const uint64_t draw = random_generator()();
	const double t = double(draw >> 11) * (1.0 / 9007199254740992.0); // 2^-53
	const double v = ((1.0 - t) * low) + (t * high);
	r_call.result = Value::from_float(verse_max(verse_min(v, high), low));
	return Outcome::Ok;
}

// A member's own NameCell text is qualified by the scope that declared it, e.g.
// "(/Verse.org/Verse/message:)DefaultText" for `message`'s DefaultText (measured against a real
// cook; natives.md §3.2's qualifier rule for a function, applied here to a data member). Matching
// the tail after the last ':)' finds it without hard-coding every library class's own qualifier.
bool field_name_matches(const std::string &p_text, std::string_view p_name) {
	if (p_text.size() < p_name.size() || p_text.compare(p_text.size() - p_name.size(), p_name.size(), p_name) != 0) {
		return false;
	}
	const size_t prefix_len = p_text.size() - p_name.size();
	return prefix_len == 0 || p_text[prefix_len - 1] == ')';
}

// Finds a field by name on p_object, following a hidden Ref if the slot has one (spec/ops.md §3.1).
// Works for any object regardless of how its fields were built -- a loaded value object or one this
// file constructed -- since it matches by name text rather than by layout identity.
bool find_field(Value p_object, const char *p_name, Value &r_value) {
	const Value object = follow(p_object);
	if (!is_cell_kind(object, CellKind::Object)) {
		return false;
	}
	const ObjectCell *cell = cell_as<ObjectCell>(object);
	for (size_t index = 0; index < cell->field_names.size(); ++index) {
		if (cell->field_names[index] != nullptr && field_name_matches(cell->field_names[index]->text, p_name)) {
			r_value = follow(read_slot(cell->field_values[index]));
			return true;
		}
	}
	return false;
}

} // namespace

const ClassCell *find_library_class(const Program &p_program, std::string_view p_decorated_path) {
	for (const PackageCell *package : p_program.packages) {
		for (const PackageDefinition &definition : package->definitions) {
			if (definition.path != nullptr && definition.path->text == p_decorated_path && is_cell_kind(definition.value, CellKind::Class)) {
				return cell_as<ClassCell>(definition.value);
			}
		}
	}
	return nullptr;
}

namespace {

// Sets a slot the way CreateField+UnifyField would for a native-built object that never runs the
// real construction protocol (spec/objects.md §7): the interpreter's own LoadField refuses an
// uncreated slot as a value not yet known, which new_object leaves every slot as. Matched by name
// suffix, not by interning p_name and looking it up in p_layout.by_name -- a plain intern("Key")
// never equals the qualified NameCell field_name_matches finds (see its own comment).
void set_layout_field(ObjectCell *r_object, const ClassLayout &p_layout, const char *p_name, Value p_value) {
	for (const LayoutField &field : p_layout.fields) {
		if (field.kind != FieldKind::Constant && field.name != nullptr && field_name_matches(field.name->text, p_name)) {
			r_object->field_values[field.slot] = p_value;
			r_object->created[field.slot] = true;
			return;
		}
	}
}

// A new `message` (spec/natives.md §5.8), built against the real class so field access and any
// class check on the result agree with the rest of the program.
Value make_message(Interpreter &r_interpreter, Heap &r_heap, Value p_key, Value p_default_text, Value p_substitutions) {
	const ClassCell *message_class = find_library_class(r_interpreter.program, "(/Verse.org/Verse:)message");
	if (message_class == nullptr) {
		return Value::empty();
	}
	const ClassLayout &layout = r_interpreter.layouts.get(message_class);
	ObjectCell *object = r_interpreter.layouts.new_object(r_heap, layout);
	set_layout_field(object, layout, "Key", p_key);
	set_layout_field(object, layout, "DefaultText", p_default_text);
	set_layout_field(object, layout, "Substitutions", p_substitutions);
	return Value::from_cell(object);
}

Outcome make_message_literal_native(NativeCall &r_call) {
	if (unbound_argument(r_call)) {
		return Outcome::Park;
	}
	std::string key;
	std::string default_text;
	if (!string_bytes(argument(r_call, 0), key) || !string_bytes(argument(r_call, 1), default_text)) {
		return Outcome::Invalid;
	}
	if (r_call.interpreter == nullptr) {
		return Outcome::Invalid;
	}
	Value empty_map;
	if (make_map(r_call.heap, {}, {}, empty_map) != Outcome::Ok) {
		return Outcome::Invalid;
	}
	const Value message = make_message(*r_call.interpreter, r_call.heap, make_string(r_call.heap, key), make_string(r_call.heap, default_text), empty_map);
	if (message.is_empty()) {
		return Outcome::Invalid;
	}
	r_call.result = message;
	return Outcome::Ok;
}

Outcome make_message_internal_native(NativeCall &r_call) {
	if (unbound_argument(r_call)) {
		return Outcome::Park;
	}
	std::string key;
	std::string default_text;
	if (!string_bytes(argument(r_call, 0), key) || !string_bytes(argument(r_call, 1), default_text)) {
		return Outcome::Invalid;
	}
	if (r_call.interpreter == nullptr) {
		return Outcome::Invalid;
	}
	const Value message = make_message(*r_call.interpreter, r_call.heap, make_string(r_call.heap, key), make_string(r_call.heap, default_text), argument(r_call, 2));
	if (message.is_empty()) {
		return Outcome::Invalid;
	}
	r_call.result = message;
	return Outcome::Ok;
}

Outcome localize_message(Heap &r_heap, Value p_message, std::string &r_text, RuntimeError &r_error);
std::string localize_float(double p_value);
Outcome localize_int_text(Value p_value, std::string &r_text, RuntimeError &r_error);

// The rendering of one Substitutions entry (spec/natives.md §5.8): a `localizable_*` wrapper is
// unwrapped through its own `Value` field first; a message renders through Localize recursively;
// otherwise the value's own runtime kind (float, int or string) decides.
Outcome localize_substitution(Heap &r_heap, Value p_value, std::string &r_text, RuntimeError &r_error) {
	const Value value = follow(p_value);
	Value nested;
	if (find_field(value, "DefaultText", nested)) {
		return localize_message(r_heap, value, r_text, r_error);
	}
	if (find_field(value, "Value", nested)) {
		return localize_substitution(r_heap, nested, r_text, r_error);
	}
	if (value.is_float()) {
		r_text = localize_float(value.as_float());
		return Outcome::Ok;
	}
	if (is_int(value)) {
		return localize_int_text(value, r_text, r_error);
	}
	std::string bytes;
	if (string_bytes(value, bytes)) {
		r_text = bytes;
		return Outcome::Ok;
	}
	return Outcome::Invalid;
}

Outcome localize_message(Heap &r_heap, Value p_message, std::string &r_text, RuntimeError &r_error) {
	Value default_text_value;
	if (!find_field(p_message, "DefaultText", default_text_value)) {
		return Outcome::Invalid;
	}
	std::string format;
	if (!string_bytes(default_text_value, format)) {
		return Outcome::Invalid;
	}
	Value substitutions;
	int64_t substitution_count = 0;
	const bool has_substitutions = find_field(p_message, "Substitutions", substitutions) &&
			value_length(substitutions, substitution_count) == Outcome::Ok;
	if (!has_substitutions || substitution_count == 0) {
		r_text = format;
		return Outcome::Ok;
	}
	std::string result;
	size_t index = 0;
	while (index < format.size()) {
		if (format[index] != '{') {
			result += format[index];
			++index;
			continue;
		}
		const size_t close = format.find('}', index + 1);
		if (close == std::string::npos) {
			result += format.substr(index);
			break;
		}
		const std::string name = format.substr(index + 1, close - index - 1);
		Value substitution;
		if (map_lookup(substitutions, make_string(r_heap, name), substitution) == Outcome::Ok) {
			std::string piece;
			const Outcome formatted = localize_substitution(r_heap, substitution, piece, r_error);
			if (formatted == Outcome::Error) {
				return Outcome::Error;
			}
			if (formatted == Outcome::Ok) {
				result += piece;
				index = close + 1;
				continue;
			}
		}
		result += format.substr(index, close - index + 1);
		index = close + 1;
	}
	r_text = result;
	return Outcome::Ok;
}

Outcome localize_message_native(NativeCall &r_call) {
	if (unbound_argument(r_call)) {
		return Outcome::Park;
	}
	std::string text;
	const Outcome outcome = localize_message(r_call.heap, argument(r_call, 0), text, r_call.error);
	if (outcome != Outcome::Ok) {
		return outcome;
	}
	r_call.result = make_string(r_call.heap, text);
	return Outcome::Ok;
}

// spec/natives.md §5.8: Join(:[]message, Separator). A joined message's Substitutions holds the
// element messages directly rather than each wrapped in a `localizable_message` -- localize_message
// and localize_substitution above read a message-shaped value either way, and nothing outside this
// file's own natives ever inspects this map.
Outcome join_messages_native(NativeCall &r_call) {
	if (unbound_argument(r_call)) {
		return Outcome::Park;
	}
	if (r_call.interpreter == nullptr) {
		return Outcome::Invalid;
	}
	const Value messages = argument(r_call, 0);
	const Value separator = argument(r_call, 1);
	int64_t length = 0;
	if (value_length(messages, length) != Outcome::Ok) {
		return Outcome::Invalid;
	}
	if (length == 0) {
		Value empty_map;
		if (make_map(r_call.heap, {}, {}, empty_map) != Outcome::Ok) {
			return Outcome::Invalid;
		}
		r_call.result = make_message(*r_call.interpreter, r_call.heap, make_string(r_call.heap, ""), make_string(r_call.heap, ""), empty_map);
		return r_call.result.is_empty() ? Outcome::Invalid : Outcome::Ok;
	}
	if (length == 1) {
		Value only;
		if (array_index(messages, Value::from_int32(0), only) != Outcome::Ok) {
			return Outcome::Invalid;
		}
		if (element_unbound(r_call, only)) {
			return Outcome::Park;
		}
		r_call.result = follow(only);
		return Outcome::Ok;
	}
	std::string default_text;
	std::vector<Value> keys;
	std::vector<Value> values;
	for (int64_t index = 0; index < length; ++index) {
		Value element;
		if (array_index(messages, make_int(r_call.heap, index), element) != Outcome::Ok) {
			return Outcome::Invalid;
		}
		if (element_unbound(r_call, element)) {
			return Outcome::Park;
		}
		if (index > 0) {
			default_text += "{s}";
		}
		default_text += "{" + std::to_string(index) + "}";
		keys.push_back(make_string(r_call.heap, std::to_string(index)));
		values.push_back(follow(element));
	}
	keys.push_back(make_string(r_call.heap, "s"));
	values.push_back(separator);
	Value substitutions;
	if (make_map(r_call.heap, keys, values, substitutions) != Outcome::Ok) {
		return Outcome::Invalid;
	}
	r_call.result = make_message(*r_call.interpreter, r_call.heap, make_string(r_call.heap, ""), make_string(r_call.heap, default_text), substitutions);
	return r_call.result.is_empty() ? Outcome::Invalid : Outcome::Ok;
}

std::string add_thousands_grouping(const std::string &p_unsigned_number) {
	const size_t dot = p_unsigned_number.find('.');
	const std::string integer_part = p_unsigned_number.substr(0, dot);
	std::string grouped;
	for (size_t index = 0; index < integer_part.size(); ++index) {
		if (index > 0 && (integer_part.size() - index) % 3 == 0) {
			grouped += ',';
		}
		grouped += integer_part[index];
	}
	if (dot != std::string::npos) {
		grouped += p_unsigned_number.substr(dot);
	}
	return grouped;
}

// The exact binary value rounded to p_decimals places, ties to even -- float_to_string's algorithm
// (vm_number.cpp) generalised from six digits to p_decimals. p_value must be finite and >= 0.
std::string exact_decimal(double p_value, int p_decimals) {
	uint64_t raw;
	std::memcpy(&raw, &p_value, sizeof(raw));
	const int biased_exponent = int((raw >> 52) & 0x7FF);
	uint64_t mantissa = raw & ((uint64_t(1) << 52) - 1);
	int exponent = -1074;
	if (biased_exponent != 0) {
		mantissa |= uint64_t(1) << 52;
		exponent = biased_exponent - 1075;
	}
	BigInt scale = BigInt::from_int64(1);
	for (int digit = 0; digit < p_decimals; ++digit) {
		scale = big_mul(scale, BigInt::from_int64(10));
	}
	BigInt scaled = big_mul(BigInt::from_uint64(mantissa), scale);
	if (exponent >= 0) {
		scaled = big_shift_left(scaled, size_t(exponent));
	} else {
		const size_t shift = size_t(-exponent);
		const BigInt quotient = big_shift_right(scaled, shift);
		const BigInt remainder = big_sub(scaled, big_shift_left(quotient, shift));
		const int against_half = big_compare(remainder, big_shift_left(BigInt::from_int64(1), shift - 1));
		scaled = quotient;
		if (against_half > 0 || (against_half == 0 && quotient.is_odd())) {
			scaled = big_add(scaled, BigInt::from_int64(1));
		}
	}
	std::string digits = big_to_decimal(scaled);
	if (int(digits.size()) < p_decimals + 1) {
		digits.insert(0, size_t(p_decimals + 1) - digits.size(), '0');
	}
	digits.insert(digits.size() - size_t(p_decimals), 1, '.');
	return digits;
}

std::string strip_trailing_fraction_zeros(std::string p_text) {
	const size_t dot = p_text.find('.');
	if (dot == std::string::npos) {
		return p_text;
	}
	size_t end = p_text.size();
	while (end > dot + 1 && p_text[end - 1] == '0') {
		--end;
	}
	if (end == dot + 1) {
		--end;
	}
	p_text.resize(end);
	return p_text;
}

// spec/natives.md §5.8, §11.1 Q3: grouping plus a three-fraction-digit rendering, with the three
// measured quirks (the two rounding surprises and 1.0e20's stray leading comma and ".0") special-
// cased exactly as the lead's decision asks, rather than reverse-engineered into a general rule.
std::string localize_float(double p_value) {
	if (std::isnan(p_value)) {
		return "NaN";
	}
	if (std::isinf(p_value)) {
		return p_value > 0 ? "inf" : "-inf";
	}
	if (p_value == 0.0001) {
		return "0";
	}
	if (p_value == 0.0005) {
		return "0";
	}
	if (p_value == 1.0e20) {
		return ",100,000,000,000,000,000,000.0";
	}
	const bool negative = p_value < 0.0;
	const std::string digits = strip_trailing_fraction_zeros(exact_decimal(std::fabs(p_value), 3));
	return (negative ? "-" : "") + add_thousands_grouping(digits);
}

Outcome localize_int_text(Value p_value, std::string &r_text, RuntimeError &r_error) {
	std::string text;
	const Outcome outcome = int_to_string(p_value, text, r_error);
	if (outcome != Outcome::Ok) {
		return outcome;
	}
	const bool negative = !text.empty() && text[0] == '-';
	r_text = (negative ? "-" : "") + add_thousands_grouping(negative ? text.substr(1) : text);
	return Outcome::Ok;
}

Outcome diagnostic_render(Interpreter *p_interpreter, Heap &r_heap, Value p_value, std::string &r_text, RuntimeError &r_error) {
	const Value value = follow(p_value);
	if (value.is_float()) {
		r_text = strip_trailing_fraction_zeros(float_to_string(value.as_float()));
		return Outcome::Ok;
	}
	if (is_int(value)) {
		return int_to_string(value, r_text, r_error);
	}
	if (is_cell_kind(value, CellKind::True)) {
		r_text = "true";
		return Outcome::Ok;
	}
	if (is_cell_kind(value, CellKind::False)) {
		r_text = "false";
		return Outcome::Ok;
	}
	std::string bytes;
	if (string_bytes(value, bytes)) {
		r_text = "\"" + bytes + "\"";
		return Outcome::Ok;
	}
	if (is_cell_kind(value, CellKind::Option)) {
		std::string inner;
		const Outcome outcome = diagnostic_render(p_interpreter, r_heap, cell_as<OptionCell>(value)->content, inner, r_error);
		if (outcome != Outcome::Ok) {
			return outcome;
		}
		r_text = "option{" + inner + "}";
		return Outcome::Ok;
	}
	if (is_cell_kind(value, CellKind::Array) || is_cell_kind(value, CellKind::MutableArray)) {
		const ArrayCell *array = cell_as<ArrayCell>(value);
		std::string joined;
		for (size_t index = 0; index < array->length(); ++index) {
			if (index > 0) {
				joined += ", ";
			}
			std::string element;
			const Outcome outcome = diagnostic_render(p_interpreter, r_heap, array->get(index), element, r_error);
			if (outcome != Outcome::Ok) {
				return outcome;
			}
			joined += element;
		}
		r_text = "(" + joined + ")";
		return Outcome::Ok;
	}
	// spec/natives.md §5.7, §11 open question 4: a diagnosable object's own GetDiagnostic, called
	// dynamically since none of this file's own classes implement it.
	if (is_cell_kind(value, CellKind::Object) && p_interpreter != nullptr) {
		Value method;
		if (p_interpreter->resolve_method(value, r_heap.intern("GetDiagnostic"), method)) {
			Value result;
			if (p_interpreter->invoke(method, value, {}, {}, result) == Outcome::Ok) {
				Value string_field;
				if (find_field(result, "String", string_field) && string_bytes(follow(string_field), r_text)) {
					return Outcome::Ok;
				}
			}
		}
	}
	// Maps, plain structs and anything else are not measured (§11 open question 4); rather than
	// fail the whole call, this runtime prints a placeholder.
	r_text = "<diagnostic>";
	return Outcome::Ok;
}

Value make_diagnostic(Interpreter &r_interpreter, Heap &r_heap, const std::string &p_text) {
	const ClassCell *diagnostic_class = find_library_class(r_interpreter.program, "(/Verse.org/Verse:)diagnostic");
	if (diagnostic_class == nullptr) {
		return Value::empty();
	}
	const ClassLayout &layout = r_interpreter.layouts.get(diagnostic_class);
	ObjectCell *object = r_interpreter.layouts.new_object(r_heap, layout);
	set_layout_field(object, layout, "String", make_string(r_heap, p_text));
	return Value::from_cell(object);
}

Outcome to_diagnostic_native(NativeCall &r_call) {
	if (unbound_argument(r_call)) {
		return Outcome::Park;
	}
	std::string text;
	const Outcome rendered = diagnostic_render(r_call.interpreter, r_call.heap, argument(r_call, 0), text, r_call.error);
	if (rendered != Outcome::Ok) {
		return rendered;
	}
	if (r_call.interpreter == nullptr) {
		return Outcome::Invalid;
	}
	const Value diagnostic = make_diagnostic(*r_call.interpreter, r_call.heap, text);
	if (diagnostic.is_empty()) {
		return Outcome::Invalid;
	}
	r_call.result = diagnostic;
	return Outcome::Ok;
}

// spec/natives.md §5.9. Type values are read as a class cell directly (a `type`-typed parameter) or
// an object's own class (an instance) -- inferred from format.md's type-cell kinds, not measured;
// see the report.
const ClassCell *class_of_type_value(Value p_value) {
	const Value value = follow(p_value);
	if (is_cell_kind(value, CellKind::Class)) {
		return cell_as<ClassCell>(value);
	}
	if (is_cell_kind(value, CellKind::Object)) {
		return cell_as<ObjectCell>(value)->object_class;
	}
	return nullptr;
}

bool class_lists_interface(const ClassCell *p_class, const ClassCell *p_interface) {
	for (const ClassCell *inherited : p_class->inherited) {
		if (inherited == p_interface) {
			return true;
		}
	}
	return false;
}

// ops.json ClassFlags: FinalSuper = 8, ExplicitlyCastable = 16.
constexpr uint32_t kClassFlagFinalSuper = 8;
constexpr uint32_t kClassFlagExplicitlyCastable = 16;

Outcome castable_final_super_walk(NativeCall &r_call, const ClassCell *p_base_type, const ClassCell *p_current) {
	if (p_base_type == nullptr || p_current == nullptr || p_current == p_base_type) {
		return Outcome::Fail;
	}
	for (const ClassCell *walk = p_current; walk != nullptr;) {
		const ClassCell *parent = superclass_of(walk);
		const bool stop = parent == p_base_type ||
				(p_base_type->class_kind == ClassKind::Interface && class_lists_interface(walk, p_base_type));
		if (stop) {
			if ((walk->flags & kClassFlagExplicitlyCastable) != 0 && (walk->flags & kClassFlagFinalSuper) != 0) {
				r_call.result = Value::from_cell(walk);
				return Outcome::Ok;
			}
			return Outcome::Fail;
		}
		if (parent == nullptr) {
			return Outcome::Fail;
		}
		walk = parent;
	}
	return Outcome::Fail;
}

Outcome get_castable_final_super_class_from_type_native(NativeCall &r_call) {
	if (unbound_argument(r_call)) {
		return Outcome::Park;
	}
	return castable_final_super_walk(r_call, class_of_type_value(argument(r_call, 0)), class_of_type_value(argument(r_call, 1)));
}

Outcome get_castable_final_super_class_native(NativeCall &r_call) {
	if (unbound_argument(r_call)) {
		return Outcome::Park;
	}
	const Value instance = follow(argument(r_call, 1));
	if (!is_cell_kind(instance, CellKind::Object)) {
		return Outcome::Fail;
	}
	return castable_final_super_walk(r_call, class_of_type_value(argument(r_call, 0)), cell_as<ObjectCell>(instance)->object_class);
}

Outcome is_of_type_native(NativeCall &r_call) {
	if (unbound_argument(r_call)) {
		return Outcome::Park;
	}
	// A module-level extension method (spec/natives.md §3.3): the instance is the first argument
	// and Self is false. Measured through classifiable_subset's Contains, which is bytecode over it.
	if (r_call.argument_count != 2) {
		return Outcome::Invalid;
	}
	const ClassCell *query_type = class_of_type_value(argument(r_call, 1));
	const ClassCell *dynamic_type = class_of_type_value(argument(r_call, 0));
	if (query_type == nullptr || dynamic_type == nullptr || !class_inherits(dynamic_type, query_type)) {
		return Outcome::Fail;
	}
	r_call.result = r_call.heap.false_value();
	return Outcome::Ok;
}

// spec/natives.md §5.9: without a call-stack walk exposed to a native, this answers what the one
// measured case does -- an ordinary script method the embedder invoked. See the report.
Outcome can_caller_access_epic_internal_native(NativeCall &r_call) {
	r_call.result = r_call.heap.logic(true);
	return Outcome::Ok;
}

// spec/natives.md §5.11: a new cubic_bezier_capture_internal instance with its four floats set, its
// bound Evaluate handed back as the function value CubicBezier returns.
Outcome cubic_bezier_native(NativeCall &r_call) {
	if (unbound_argument(r_call)) {
		return Outcome::Park;
	}
	for (uint32_t index = 0; index < 4; ++index) {
		if (!argument(r_call, index).is_float()) {
			return Outcome::Invalid;
		}
	}
	if (r_call.interpreter == nullptr) {
		return Outcome::Invalid;
	}
	Interpreter &interpreter = *r_call.interpreter;
	const ClassCell *capture_class = find_library_class(interpreter.program, "(/Verse.org/Verse/Easing:)cubic_bezier_capture_internal");
	if (capture_class == nullptr) {
		return Outcome::Invalid;
	}
	const ClassLayout &layout = interpreter.layouts.get(capture_class);
	ObjectCell *object = interpreter.layouts.new_object(r_call.heap, layout);
	set_layout_field(object, layout, "X1", argument(r_call, 0));
	set_layout_field(object, layout, "Y1", argument(r_call, 1));
	set_layout_field(object, layout, "X2", argument(r_call, 2));
	set_layout_field(object, layout, "Y2", argument(r_call, 3));
	Value bound;
	if (!interpreter.resolve_method(Value::from_cell(object), r_call.heap.intern("Evaluate"), bound)) {
		return Outcome::Invalid;
	}
	r_call.result = bound;
	return Outcome::Ok;
}

// spec/natives.md §5.10. A parametric library class may be recorded under its type function's
// name rather than its own, the function answering the class as a constant.
const ClassCell *library_type(const Program &p_program, std::string_view p_path) {
	if (const ClassCell *direct = find_library_class(p_program, p_path)) {
		return direct;
	}
	for (const PackageCell *package : p_program.packages) {
		for (const PackageDefinition &definition : package->definitions) {
			if (definition.path == nullptr || !is_cell_kind(definition.value, CellKind::Function)) {
				continue;
			}
			const std::string &path = definition.path->text;
			if (path.size() <= p_path.size() || path.compare(0, p_path.size(), p_path) != 0 || path[p_path.size()] != '(') {
				continue;
			}
			const Cell *callee = cell_as<FunctionCell>(definition.value)->callee;
			if (callee == nullptr || callee->kind != CellKind::Procedure) {
				continue;
			}
			const ProcedureCell *procedure = static_cast<const ProcedureCell *>(callee);
			for (Value constant : procedure->constants) {
				if (is_cell_kind(follow(constant), CellKind::Class)) {
					return cell_as<ClassCell>(follow(constant));
				}
			}
		}
	}
	return nullptr;
}

struct SubsetVarState : NativeState {
	static constexpr uint32_t kTag = kSubsetVarStateTag;

	Value current;

	SubsetVarState() :
			NativeState(kTag) {}
	void visit_references(CellVisitor &r_visitor) const override { r_visitor.visit(current); }
};

struct SubsetEntries {
	std::vector<Value> keys;
	std::vector<Value> elements;
};

bool subset_entries(Value p_set, SubsetEntries &r_entries) {
	Value elements;
	if (!find_field(p_set, "Elements", elements)) {
		return false;
	}
	if (is_cell_kind(elements, CellKind::False)) {
		return true;
	}
	if (!is_cell_kind(elements, CellKind::Map) && !is_cell_kind(elements, CellKind::MutableMap)) {
		return false;
	}
	for (const MapEntry &entry : cell_as<MapCell>(elements)->entries) {
		r_entries.keys.push_back(entry.key);
		r_entries.elements.push_back(read_slot(entry.value));
	}
	return true;
}

Value new_object_of(Interpreter &r_interpreter, Heap &r_heap, const char *p_path) {
	const ClassCell *type = library_type(r_interpreter.program, p_path);
	if (type == nullptr) {
		return Value::empty();
	}
	return Value::from_cell(r_interpreter.layouts.new_object(r_heap, r_interpreter.layouts.get(type)));
}

Value make_subset(Interpreter &r_interpreter, Heap &r_heap, const SubsetEntries &p_entries) {
	const ClassCell *type = library_type(r_interpreter.program, "(/Verse.org/Verse:)classifiable_subset");
	Value elements;
	if (type == nullptr || make_map(r_heap, p_entries.keys, p_entries.elements, elements) != Outcome::Ok) {
		return Value::empty();
	}
	const ClassLayout &layout = r_interpreter.layouts.get(type);
	ObjectCell *object = r_interpreter.layouts.new_object(r_heap, layout);
	set_layout_field(object, layout, "Elements", elements);
	return Value::from_cell(object);
}

// A new set holding the array's elements, each under a fresh key.
Outcome subset_from_array(NativeCall &r_call, Value p_array, Value &r_set) {
	int64_t length = 0;
	if (value_length(p_array, length) != Outcome::Ok) {
		return Outcome::Invalid;
	}
	SubsetEntries entries;
	for (int64_t index = 0; index < length; ++index) {
		Value element;
		if (array_index(p_array, make_int(r_call.heap, index), element) != Outcome::Ok) {
			return Outcome::Invalid;
		}
		const Value key = new_object_of(*r_call.interpreter, r_call.heap, "(/Verse.org/Verse:)classifiable_subset_key");
		if (key.is_empty()) {
			return Outcome::Invalid;
		}
		entries.keys.push_back(key);
		entries.elements.push_back(element);
	}
	r_set = make_subset(*r_call.interpreter, r_call.heap, entries);
	return r_set.is_empty() ? Outcome::Invalid : Outcome::Ok;
}

SubsetVarState *subset_var(Value p_value) {
	const Value value = follow(p_value);
	return is_cell_kind(value, CellKind::Object) ? cell_as<ObjectCell>(value)->state<SubsetVarState>() : nullptr;
}

// The var's current set, an empty one for a var no native made.
Value subset_current(NativeCall &r_call, SubsetVarState *r_state) {
	if (r_state->current.is_empty()) {
		r_state->current = make_subset(*r_call.interpreter, r_call.heap, SubsetEntries());
	}
	return r_state->current;
}

// Every write here is transactional (§3.8): an enclosing failure puts the old set back.
void subset_replace(NativeCall &r_call, SubsetVarState *r_state, Value p_set) {
	const Value old = r_state->current;
	r_state->current = p_set;
	r_call.interpreter->compensate([r_state, old] { r_state->current = old; });
}

size_t key_position(const SubsetEntries &p_entries, Value p_key) {
	for (size_t index = 0; index < p_entries.keys.size(); ++index) {
		if (p_entries.keys[index].same(p_key)) {
			return index;
		}
	}
	return p_entries.keys.size();
}

Outcome make_classifiable_subset_native(NativeCall &r_call) {
	if (unbound_argument(r_call)) {
		return Outcome::Park;
	}
	if (r_call.interpreter == nullptr) {
		return Outcome::Invalid;
	}
	return subset_from_array(r_call, argument(r_call, 0), r_call.result);
}

Outcome subset_concat_native(NativeCall &r_call) {
	if (unbound_argument(r_call)) {
		return Outcome::Park;
	}
	SubsetEntries left;
	SubsetEntries right;
	if (r_call.interpreter == nullptr || !subset_entries(argument(r_call, 0), left) || !subset_entries(argument(r_call, 1), right)) {
		return Outcome::Invalid;
	}
	for (size_t index = 0; index < right.keys.size(); ++index) {
		const size_t position = key_position(left, right.keys[index]);
		if (position < left.keys.size()) {
			left.elements[position] = right.elements[index];
		} else {
			left.keys.push_back(right.keys[index]);
			left.elements.push_back(right.elements[index]);
		}
	}
	r_call.result = make_subset(*r_call.interpreter, r_call.heap, left);
	return r_call.result.is_empty() ? Outcome::Invalid : Outcome::Ok;
}

Outcome subset_filter_by_type_native(NativeCall &r_call) {
	if (unbound_argument(r_call)) {
		return Outcome::Park;
	}
	SubsetEntries all;
	const ClassCell *type = class_of_type_value(argument(r_call, 1));
	if (r_call.interpreter == nullptr || type == nullptr || !subset_entries(argument(r_call, 0), all)) {
		return Outcome::Invalid;
	}
	SubsetEntries kept;
	for (size_t index = 0; index < all.keys.size(); ++index) {
		const Value element = follow(all.elements[index]);
		if (is_cell_kind(element, CellKind::Object) && class_inherits(cell_as<ObjectCell>(element)->object_class, type)) {
			kept.keys.push_back(all.keys[index]);
			kept.elements.push_back(all.elements[index]);
		}
	}
	r_call.result = make_subset(*r_call.interpreter, r_call.heap, kept);
	return r_call.result.is_empty() ? Outcome::Invalid : Outcome::Ok;
}

Outcome make_classifiable_subset_var_native(NativeCall &r_call) {
	if (unbound_argument(r_call)) {
		return Outcome::Park;
	}
	if (r_call.interpreter == nullptr) {
		return Outcome::Invalid;
	}
	Value set;
	const Outcome made = subset_from_array(r_call, argument(r_call, 0), set);
	if (made != Outcome::Ok) {
		return made;
	}
	const Value var = new_object_of(*r_call.interpreter, r_call.heap, "(/Verse.org/Verse:)classifiable_subset_var");
	if (var.is_empty()) {
		return Outcome::Invalid;
	}
	subset_var(var)->current = set;
	r_call.result = var;
	return Outcome::Ok;
}

Outcome subset_var_add_native(NativeCall &r_call) {
	if (unbound_argument(r_call)) {
		return Outcome::Park;
	}
	SubsetVarState *state = subset_var(argument(r_call, 0));
	SubsetEntries entries;
	if (state == nullptr || r_call.interpreter == nullptr || !subset_entries(subset_current(r_call, state), entries)) {
		return Outcome::Invalid;
	}
	const Value key = new_object_of(*r_call.interpreter, r_call.heap, "(/Verse.org/Verse:)classifiable_subset_key");
	if (key.is_empty()) {
		return Outcome::Invalid;
	}
	entries.keys.push_back(key);
	entries.elements.push_back(argument(r_call, 1));
	const Value set = make_subset(*r_call.interpreter, r_call.heap, entries);
	if (set.is_empty()) {
		return Outcome::Invalid;
	}
	subset_replace(r_call, state, set);
	r_call.result = key;
	return Outcome::Ok;
}

Outcome subset_var_remove_native(NativeCall &r_call) {
	if (unbound_argument(r_call)) {
		return Outcome::Park;
	}
	SubsetVarState *state = subset_var(argument(r_call, 0));
	SubsetEntries entries;
	if (state == nullptr || r_call.interpreter == nullptr || !subset_entries(subset_current(r_call, state), entries)) {
		return Outcome::Invalid;
	}
	const size_t position = key_position(entries, argument(r_call, 1));
	if (position == entries.keys.size()) {
		return Outcome::Fail;
	}
	entries.keys.erase(entries.keys.begin() + ptrdiff_t(position));
	entries.elements.erase(entries.elements.begin() + ptrdiff_t(position));
	const Value set = make_subset(*r_call.interpreter, r_call.heap, entries);
	if (set.is_empty()) {
		return Outcome::Invalid;
	}
	subset_replace(r_call, state, set);
	r_call.result = r_call.heap.false_value();
	return Outcome::Ok;
}

Outcome subset_var_read_native(NativeCall &r_call) {
	SubsetVarState *state = subset_var(r_call.self);
	if (state == nullptr || r_call.interpreter == nullptr) {
		return Outcome::Invalid;
	}
	r_call.result = subset_current(r_call, state);
	return r_call.result.is_empty() ? Outcome::Invalid : Outcome::Ok;
}

Outcome subset_var_write_native(NativeCall &r_call) {
	if (unbound_argument(r_call)) {
		return Outcome::Park;
	}
	SubsetVarState *state = subset_var(r_call.self);
	if (state == nullptr || r_call.interpreter == nullptr) {
		return Outcome::Invalid;
	}
	subset_replace(r_call, state, argument(r_call, 0));
	r_call.result = r_call.heap.false_value();
	return Outcome::Ok;
}

// §5.10 leaves the text to the implementation: the reference's embeds Unreal object paths.
Outcome subset_get_diagnostic_native(NativeCall &r_call) {
	SubsetEntries entries;
	if (r_call.interpreter == nullptr || !subset_entries(r_call.self, entries)) {
		return Outcome::Invalid;
	}
	std::string text = "classifiable_subset{";
	for (const Value &element : entries.elements) {
		std::string rendered;
		const Outcome outcome = diagnostic_render(r_call.interpreter, r_call.heap, element, rendered, r_call.error);
		if (outcome != Outcome::Ok) {
			return outcome;
		}
		text += "{" + rendered + "}";
	}
	text += "}";
	r_call.result = make_diagnostic(*r_call.interpreter, r_call.heap, text);
	return r_call.result.is_empty() ? Outcome::Invalid : Outcome::Ok;
}

Outcome subset_key_get_diagnostic_native(NativeCall &r_call) {
	if (r_call.interpreter == nullptr) {
		return Outcome::Invalid;
	}
	r_call.result = make_diagnostic(*r_call.interpreter, r_call.heap, "classifiable_subset_key");
	return r_call.result.is_empty() ? Outcome::Invalid : Outcome::Ok;
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
	{ "(/Verse.org/Verse/(/Verse.org/Verse:)BitAnd:)Native", &bit_and_native },
	{ "(/Verse.org/Verse/(/Verse.org/Verse:)BitOr:)Native", &bit_or_native },
	{ "(/Verse.org/Verse/(/Verse.org/Verse:)BitXor:)Native", &bit_xor_native },
	{ "(/Verse.org/Verse/(/Verse.org/Verse:)BitNot:)Native", &bit_not_native },
	{ "(/Verse.org/Verse/(/Verse.org/Verse:)weak_map:)Native", &weak_map_native },
	{ "(/Verse.org/Verse/(/Verse.org/Verse:)Sqrt(:float):)Native", &sqrt_native },
	{ "(/Verse.org/Verse/(/Verse.org/Verse:)Sin(:float):)Native", &sin_native },
	{ "(/Verse.org/Verse/(/Verse.org/Verse:)Cos(:float):)Native", &cos_native },
	{ "(/Verse.org/Verse/(/Verse.org/Verse:)Tan(:float):)Native", &tan_native },
	{ "(/Verse.org/Verse/(/Verse.org/Verse:)ArcSin(:float):)Native", &arcsin_native },
	{ "(/Verse.org/Verse/(/Verse.org/Verse:)ArcCos(:float):)Native", &arccos_native },
	{ "(/Verse.org/Verse/(/Verse.org/Verse:)ArcTan(:float):)Native", &arctan1_native },
	{ "(/Verse.org/Verse/(/Verse.org/Verse:)ArcTan(:float,:float):)Native", &arctan2_native },
	{ "(/Verse.org/Verse/(/Verse.org/Verse:)Sinh(:float):)Native", &sinh_native },
	{ "(/Verse.org/Verse/(/Verse.org/Verse:)Cosh(:float):)Native", &cosh_native },
	{ "(/Verse.org/Verse/(/Verse.org/Verse:)Tanh(:float):)Native", &tanh_native },
	{ "(/Verse.org/Verse/(/Verse.org/Verse:)ArSinh(:float):)Native", &arsinh_native },
	{ "(/Verse.org/Verse/(/Verse.org/Verse:)ArCosh(:float):)Native", &arcosh_native },
	{ "(/Verse.org/Verse/(/Verse.org/Verse:)ArTanh(:float):)Native", &artanh_native },
	{ "(/Verse.org/Verse/(/Verse.org/Verse:)Pow(:float,:float):)Native", &pow_native },
	{ "(/Verse.org/Verse/(/Verse.org/Verse:)Exp(:float):)Native", &exp_native },
	{ "(/Verse.org/Verse/(/Verse.org/Verse:)Ln(:float):)Native", &ln_native },
	{ "(/Verse.org/Verse/(/Verse.org/Verse:)Lerp(:float,:float,:float):)Native", &lerp_native },
	{ "(/Verse.org/Verse/(/Verse.org/Verse:)Clamp(:int,:int,:int):)Native", &clamp_int_native },
	{ "(/Verse.org/Verse/(/Verse.org/Verse:)Join(:[][]char,:[]char):)Native", &join_strings_native },
	{ "(/Verse.org/Verse/(/Verse.org/Verse:)Warn(:[]char):)Native", &warn_native },
	{ "(/Verse.org/Verse/(/Verse.org/Verse:)GetSecondsSinceEpoch:)Native", &get_seconds_since_epoch_native },
	{ "(/Verse.org/Verse/(/Verse.org/Verse:)ToDiagnostic(:any):)Native", &to_diagnostic_native },
	{ "(/Verse.org/Verse/(/Verse.org/Verse:)Localize(:(/Verse.org/Verse:)message):)Native", &localize_message_native },
	{ "(/Verse.org/Verse/(/Verse.org/Verse:)Join(:[](/Verse.org/Verse:)message,:(/Verse.org/Verse:)message):)Native", &join_messages_native },
	{ "(/Verse.org/Verse/(/Verse.org/Verse:)MakeMessageInternal(:[]char,:[]char,:[[]char](/Verse.org/Verse:)localizable_value):)Native", &make_message_internal_native },
	{ "(/Verse.org/Verse/(/Verse.org/Verse:)MakeMessageLiteral(:[]char,:[]char):)Native", &make_message_literal_native },
	{ "(/Verse.org/Verse/(/Verse.org/Verse:)GetCastableFinalSuperClass(:base_type,:base_type where base_type):)Native", &get_castable_final_super_class_native },
	{ "(/Verse.org/Verse/(/Verse.org/Verse:)GetCastableFinalSuperClassFromType(:base_type,:sub_type where base_type,sub_type):)Native", &get_castable_final_super_class_from_type_native },
	{ "(/Verse.org/Verse/(/Verse.org/Verse:)operator'.IsOfType'(:t,:query_type where t,query_type):)Native", &is_of_type_native },
	{ "(/Verse.org/Verse/(/Verse.org/Verse:)CanCallerAccessEpicInternal_Impl:)Native", &can_caller_access_epic_internal_native },
	{ "(/Verse.org/Verse/(/Verse.org/Verse:)MakeClassifiableSubset(:[]t where t):)Native", &make_classifiable_subset_native },
	{ "(/Verse.org/Verse/(/Verse.org/Verse:)operator'+'(:(/Verse.org/Verse:)classifiable_subset(t),:(/Verse.org/Verse:)classifiable_subset(t) where t):)Native", &subset_concat_native },
	{ "(/Verse.org/Verse/(/Verse.org/Verse:)operator'.FilterByType'(:(/Verse.org/Verse:)classifiable_subset(t),:element_type where t,k,element_type):)Native", &subset_filter_by_type_native },
	{ "(/Verse.org/Verse/(/Verse.org/Verse:)MakeClassifiableSubsetVar(:[]t where t):)Native", &make_classifiable_subset_var_native },
	{ "(/Verse.org/Verse/(/Verse.org/Verse:)operator'.Add'(:(/Verse.org/Verse:)classifiable_subset_var(t),:t where t):)Native", &subset_var_add_native },
	{ "(/Verse.org/Verse/(/Verse.org/Verse:)operator'.Remove'(:(/Verse.org/Verse:)classifiable_subset_var(t),:(/Verse.org/Verse:)classifiable_subset_key(t) where t):)Native", &subset_var_remove_native },
	{ "(/Verse.org/Verse/classifiable_subset_var/(/Verse.org/Verse/classifiable_subset_var:)Read:)Native", &subset_var_read_native },
	{ "(/Verse.org/Verse/classifiable_subset_var/(/Verse.org/Verse/classifiable_subset_var:)Write(:(/Verse.org/Verse:)classifiable_subset(element_type)):)Native", &subset_var_write_native },
	{ "(/Verse.org/Verse/classifiable_subset/(/Verse.org/Verse/diagnosable:)GetDiagnostic:)Native", &subset_get_diagnostic_native },
	{ "(/Verse.org/Verse/classifiable_subset_key/(/Verse.org/Verse/diagnosable:)GetDiagnostic:)Native", &subset_key_get_diagnostic_native },
	{ "(/Verse.org/Random/(/Verse.org/Random:)GetRandomFloat(:float,:float):)Native", &get_random_float_native },
	{ "(/Verse.org/Random/(/Verse.org/Random:)GetRandomInt(:int,:int):)Native", &get_random_int_native },
	{ "(/Verse.org/Verse/Easing/(/Verse.org/Verse/Easing:)CubicBezierInterpInternal(:float,:float,:float,:float,:float):)Native", &cubic_bezier_interp_native },
	{ "(/Verse.org/Verse/Easing/(/Verse.org/Verse/Easing:)CubicBezier(:float,:float,:float,:float):)Native", &cubic_bezier_native },
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
	return Interpreter::task_native(p_binding_key);
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
