#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "verse_host_abi.h"
#include "vm_cell.h"
#include "vm_heap.h"
#include "vm_sidecar.h"
#include "vm_status.h"
#include "vm_value.h"

namespace vm {
class GodotBridge;
}

// One live script object the host holds. Its object is a root for as long as the host holds it
// (design §7.3).
struct vh_instance {
	vm::Value object;
	// The instance's content scope (spec/tasks.md §8.1): made at vh_instantiate, active for every
	// call into it, replaced after a raise terminated it, terminated at vh_release_instance.
	vm::Value scope;
	vh_handle handle = 0;
	const vm::SidecarClass *sidecar_class = nullptr;
	// include/verse_host_abi.h, vh_instance_set_field: one-way, at the first vh_instance_call.
	bool sealed = false;
	// godot-natives.md §9.4: the standing connections of its @export_signal events are made once.
	bool events_connected = false;
};

// The Godot side of the runtime (godot-natives.md): the `variant` lanes and their wire form (§5,
// §6), a declared type's wire form (§12.3), object identity (§4), signal and event bindings (§9),
// and the callbacks a Godot Callable names. The natives of Godot.native.verse reach it through
// Interpreter::bridge.
namespace vm {

class Interpreter;
struct Program;
class HostArena;

// One Godot Variant as the `variant` struct's 22 lanes (godot-natives.md §5).
struct VariantLanes {
	int64_t tag = 0;
	int64_t ref = 0;
	int64_t i[4] = {};
	double f[16] = {};
	std::string text;
};

class GodotBridge : public RootSource {
public:
	GodotBridge(Interpreter &r_interpreter, const Sidecar &p_sidecar);
	~GodotBridge() override;
	GodotBridge(const GodotBridge &) = delete;
	GodotBridge &operator=(const GodotBridge &) = delete;

	Interpreter &interpreter;
	Heap &heap;
	const Program &program;
	const Sidecar &sidecar;

	// Resolves the library classes and the lookup tables, once the program is loaded.
	void bind_program();

	// How the bridge says something outside a VM entry went wrong: an error diagnostic.
	std::function<void(const std::string &)> report_error;

	void register_instance(vh_instance *p_instance);
	void forget_instance(vh_instance *p_instance);
	vh_instance *instance_for(int64_t p_handle) const;

	// §5 and §6.
	Value make_variant(const VariantLanes &p_lanes);
	bool read_variant(Value p_variant, VariantLanes &r_lanes);
	bool variant_to_wire(const VariantLanes &p_lanes, vh_arena *p_arena, vh_value &r_wire);
	void wire_to_lanes(const vh_value &p_wire, VariantLanes &r_lanes);

	// §12.3: a value of a declared type to and from the wire. Either may run Verse -- building the
	// object a handle names does -- so both belong inside a VM entry.
	bool wire_to_member(const vh_value &p_wire, const SidecarMemberType &p_type, Value &r_value, std::string &r_why);
	bool member_to_wire(Value p_value, const SidecarMemberType &p_type, vh_arena *p_arena, vh_value &r_wire, std::string &r_why);
	// §12.1: the declared parameters of a method, with N loose arguments packed into one struct
	// parameter when p_pack allows it.
	bool wire_to_arguments(const SidecarMethodTypes *p_types, const vh_value *p_args, int32_t p_count, bool p_pack,
			std::vector<Value> &r_arguments, std::string &r_why);
	const SidecarMethodTypes *method_types(std::string_view p_decorated) const;
	// A package definition by its decorated path, or by that path less one leading qualifier; empty
	// when the program has neither.
	Value definition(const std::string &p_path) const;

	// §4.6: the Verse object a handle becomes. p_fallback, when not null, is the class to build a
	// fresh wrapper as instead of asking Godot (§4.8). Runs Verse, so only inside an entry.
	Outcome object_for_handle(int64_t p_handle, const ClassCell *p_fallback, Value &r_object);
	// The class a mirrored Godot class name crosses as, or null.
	const ClassCell *mirrored_class(std::string_view p_godot_name) const;

	// An object's field by its unqualified name: the slot, or null.
	Value *field_slot(ObjectCell *p_object, std::string_view p_name);
	// The field's value, through a `var`'s variable; empty when there is no such field.
	Value field_value(Value p_object, std::string_view p_name);
	int64_t handle_of(Value p_object);

	// §9.4, at vh_instantiate and vh_release_instance, and the lazy connect at the first call.
	void bind_instance_signals(vh_instance *p_instance);
	void ensure_event_connections(vh_instance *p_instance);
	void release_instance_signals(vh_instance *p_instance);

	// vh_callback_invoke's half: what the callback id names. Fills the arguments for a method, or the
	// event and payload to signal for an await or an event's standing connection. NotFound when the
	// id names nothing callable any more; Noop when delivery is a silent no-op (§9.5).
	enum class Delivery : uint8_t {
		Call,
		Signal,
		Noop,
		NotFound,
		BadArguments,
	};
	struct CallbackTarget {
		Delivery delivery = Delivery::NotFound;
		vh_instance *instance = nullptr;
		Value function;
		std::vector<Value> arguments;
		const SidecarMethodTypes *types = nullptr;
		Value event;
		Value payload;
	};
	// Runs Verse to build the arguments, so only inside an entry.
	void resolve_callback(int64_t p_id, const vh_value *p_args, int32_t p_count, CallbackTarget &r_target);
	// The instance a callback belongs to, before any Verse runs: null when it names nothing.
	bool callback_owner(int64_t p_id, vh_instance *&r_instance) const;
	void release_callback(int64_t p_id);

	// The natives' own state (§8.13-§8.24).
	struct SignalBinding {
		int64_t handle = 0;
		std::string name;
		const SidecarPayloadShape *shape = nullptr;
		int32_t reject = 0;
		std::string reject_detail;
		// An @export_signal event's: the event itself and its standing connection.
		Value event;
		bool connected = false;
		int64_t callback = 0;
		int64_t callable_ref = 0;
	};
	struct Callback {
		enum class Kind : uint8_t {
			Method,
			Await,
			Event,
		};
		Kind kind = Kind::Method;
		int64_t owner = 0;
		Value function;
		// Method: the payload shape an emission is reassembled by, or null to convert by the method's
		// declared parameters; foreign is a Godot Array of the emission's arguments (§9.3).
		const SidecarPayloadShape *shape = nullptr;
		bool foreign = false;
		int64_t token = 0;
		int64_t binding = 0;
	};
	struct AwaitRow {
		Value signal;
		int64_t handle = 0;
		std::string name;
		int64_t binding = 0;
		int64_t callback = 0;
		int64_t callable_ref = 0;
	};
	struct Subscription {
		int64_t handle = 0;
		std::string name;
		int64_t callable_ref = 0;
	};

	SignalBinding *binding(int64_t p_id);
	int64_t event_binding(const Cell *p_event) const;
	int64_t bind_engine_signal(int64_t p_handle, const std::string &p_class, const std::string &p_accessor, const std::string &p_name);
	// A Callable over a bound instance method, as a reference id; 0 when p_function is not one.
	int64_t make_method_callable(Value p_function, const SidecarPayloadShape *p_shape, bool p_foreign);
	bool bound_instance_method(Value p_function, int64_t &r_owner) const;
	int64_t connect(int64_t p_handle, const std::string &p_name, int64_t p_callable_ref, int32_t p_flags, int32_t &r_status);
	int64_t subscribe(int64_t p_handle, const std::string &p_name, int64_t p_callable_ref);
	void cancel_subscription(int64_t p_id);
	int64_t begin_await(Value p_signal, int64_t p_handle, const std::string &p_name, int64_t p_binding, bool p_foreign);
	void end_await(int64_t p_token);
	// §9.1: a payload taken apart into Godot arguments, and put back together from them.
	bool payload_to_wire(Value p_payload, const SidecarPayloadShape &p_shape, vh_arena *p_arena, std::vector<vh_value> &r_args);
	bool payload_from_wire(const SidecarPayloadShape &p_shape, const vh_value *p_args, int32_t p_count, Value &r_payload);
	// §9.3: the emission's arguments as one Godot Array, wrapped.
	bool array_from_wire(const vh_value *p_args, int32_t p_count, Value &r_array);
	Value wrap_reference(int64_t p_tag, int64_t p_ref);

	const ClassCell *variant_class = nullptr;
	const ClassCell *vh_object_class = nullptr;
	const ClassCell *vh_signal_class = nullptr;
	const ClassCell *rid_class = nullptr;
	const ClassCell *godot_array_class = nullptr;
	const ClassCell *dictionary_class = nullptr;
	const ClassCell *callable_class = nullptr;
	const ClassCell *signal_ref_class = nullptr;
	// By vh_variant_tag, for the sixteen math structs.
	const ClassCell *math_classes[VH_VARIANT_MAX] = {};

	void visit_roots(CellVisitor &r_visitor) const override;

private:
	std::unordered_map<int64_t, vh_instance *> instances;
	std::unordered_map<std::string, const ClassCell *> mirrored_by_godot_name;
	std::unordered_map<const ClassCell *, int32_t> math_tag_by_class;
	std::unordered_map<std::string, const EnumerationCell *> enumerations;
	std::unordered_map<std::string, const ClassCell *> classes_by_path;
	std::unordered_map<std::string, Value> definitions;
	std::unordered_map<std::string, const SidecarMethodTypes *, TextHash, std::equal_to<>> types_by_method;
	std::unordered_map<int64_t, const ClassCell *> handle_classes;
	std::map<std::pair<const void *, std::string>, int32_t> slot_cache;
	int32_t variant_slots[23] = {};
	// Strings compare by content, so every variant with no text can hold this one immutable cell.
	Value empty_text;

	int64_t next_id = 1;
	std::unordered_map<int64_t, SignalBinding> bindings;
	std::unordered_map<const Cell *, int64_t> event_bindings;
	std::map<std::pair<int64_t, std::string>, int64_t> engine_bindings;
	std::unordered_map<int64_t, std::vector<int64_t>> instance_bindings;
	std::unordered_map<int64_t, Callback> callbacks;
	std::unordered_map<int64_t, AwaitRow> awaits;
	std::unordered_map<int64_t, Subscription> subscriptions;

	const ClassCell *class_of_handle(int64_t p_handle);
	const ClassCell *class_at_path(const std::string &p_path) const;
	const ClassCell *declared_class(const SidecarMemberType &p_type) const;
	const EnumerationCell *enumeration(const std::string &p_decorated) const;
	int32_t math_tag_of(Value p_value) const;
	bool struct_leaves(Value p_struct, int32_t p_tag, std::vector<double> &r_leaves);
	Value struct_from_leaves(int32_t p_tag, const std::vector<double> &p_leaves, size_t &r_next);
	bool leaves_of_wire(const vh_value &p_wire, std::vector<double> &r_leaves) const;
	bool self_described_to_wire(Value p_value, vh_arena *p_arena, vh_value &r_wire, std::string &r_why);
	bool packed_elements(const vh_value &p_wire, std::vector<vh_value> &r_elements, HostArena &r_arena);
	Value element_from_wire(const vh_value &p_wire, int32_t p_tag);
	bool user_struct_from_fields(const SidecarMemberType &p_type, const vh_value *p_fields, int32_t p_count, Value &r_value, std::string &r_why);
	int64_t new_callback(Callback p_callback, int64_t p_owner, int64_t &r_callable_ref);
};

// The Godot natives (godot-natives.md §8) by binding key, or null.
NativeFn godot_native(std::string_view p_binding_key);

} // namespace vm
