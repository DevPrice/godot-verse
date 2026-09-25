#include "vm_godot.h"

#include <cmath>
#include <cstring>
#include <memory>

#include "vm_interpreter.h"
#include "vm_loader.h"
#include "vm_marshal.h"
#include "vm_natives.h"
#include "vm_number.h"
#include "vm_objects.h"
#include "vm_runtime.h"
#include "vm_values.h"

#include "../host/Private/GodotMathLayout.gen.h"
#include "../src/verse_api_classes.h"

namespace vm {

namespace {

const char *const kNativeInternal = "ErrRuntime_NativeInternal";
const char *const kNativeInternalDescription = "An internal runtime error occurred in native code that was called from Verse. There is no other information available.";

// The order of the `variant` struct's lanes, which make_variant and read_variant index by.
constexpr const char *kVariantLanes[22] = { "Tag", "Ref", "I0", "I1", "I2", "I3", "F0", "F1", "F2", "F3", "F4", "F5", "F6", "F7",
	"F8", "F9", "F10", "F11", "F12", "F13", "F14", "F15" };
constexpr int32_t kLaneText = 22;

Outcome raise(NativeCall &r_call, const std::string &p_message) {
	r_call.error.diagnostic = kNativeInternal;
	r_call.error.description = kNativeInternalDescription;
	r_call.error.message = p_message;
	return Outcome::Error;
}

GodotBridge *bridge_of(NativeCall &r_call) {
	return r_call.interpreter != nullptr ? r_call.interpreter->bridge : nullptr;
}

bool int_argument(NativeCall &r_call, uint32_t p_index, int64_t &r_value) {
	RuntimeError error;
	return int_to_int64(argument(r_call, p_index), r_value, error) == Outcome::Ok;
}

bool is_reference_tag(int64_t p_tag) {
	return p_tag >= VH_VARIANT_CALLABLE && p_tag <= VH_VARIANT_PACKED_VECTOR4_ARRAY && p_tag != VH_VARIANT_OBJECT;
}

bool is_string_tag(int64_t p_tag) {
	return p_tag == VH_VARIANT_STRING || p_tag == VH_VARIANT_STRING_NAME || p_tag == VH_VARIANT_NODE_PATH;
}

const verse_math::layout *math_layout(int64_t p_tag) {
	for (const verse_math::layout &layout : verse_math::layouts) {
		if (layout.variant_tag == p_tag) {
			return &layout;
		}
	}
	return nullptr;
}

const verse_math::layout *math_layout_named(std::string_view p_name) {
	for (const verse_math::layout &layout : verse_math::layouts) {
		if (p_name == layout.verse_name) {
			return &layout;
		}
	}
	return nullptr;
}

// A math type's scalar leaves in order, true for an integer one (godot-natives.md §5).
void leaf_kinds(int64_t p_tag, std::vector<bool> &r_kinds) {
	const verse_math::layout *layout = math_layout(p_tag);
	if (layout == nullptr) {
		return;
	}
	for (int32_t index = 0; index < layout->field_count; ++index) {
		const verse_math::field &field = layout->fields[index];
		if (field.nested_tag != 0) {
			leaf_kinds(field.nested_tag, r_kinds);
		} else {
			r_kinds.push_back(field.is_int);
		}
	}
}

// §6.3.
int64_t wire_int(const vh_value &p_wire) {
	switch (p_wire.Type) {
		case VH_TYPE_LOGIC:
			return p_wire.Logic != 0 ? 1 : 0;
		case VH_TYPE_FLOAT:
			return std::isfinite(p_wire.Float) ? int64_t(p_wire.Float) : 0;
		case VH_TYPE_REF:
			return p_wire.Ref;
		case VH_TYPE_INT:
			return p_wire.Int;
		default:
			return 0;
	}
}

double wire_double(const vh_value &p_wire) {
	switch (p_wire.Type) {
		case VH_TYPE_LOGIC:
			return p_wire.Logic != 0 ? 1.0 : 0.0;
		case VH_TYPE_INT:
			return double(p_wire.Int);
		case VH_TYPE_FLOAT:
			return p_wire.Float;
		default:
			return 0.0;
	}
}

template <typename T>
T *allocate(vh_arena *p_arena, size_t p_count) {
	if (p_count == 0 || p_arena == nullptr) {
		return nullptr;
	}
	T *items = static_cast<T *>(p_arena->Alloc(p_arena, sizeof(T) * p_count, alignof(T)));
	if (items != nullptr) {
		for (size_t index = 0; index < p_count; ++index) {
			items[index] = T{};
		}
	}
	return items;
}

bool string_to_wire(const std::string &p_text, vh_arena *p_arena, vh_value &r_wire) {
	r_wire.Type = VH_TYPE_STRING;
	r_wire.String.Utf8 = "";
	r_wire.String.Len = 0;
	if (p_text.empty()) {
		return true;
	}
	char *text = allocate<char>(p_arena, p_text.size());
	if (text == nullptr) {
		return false;
	}
	std::memcpy(text, p_text.data(), p_text.size());
	r_wire.String.Utf8 = text;
	r_wire.String.Len = int32_t(p_text.size());
	return true;
}

std::string wire_text(const vh_value &p_wire) {
	if (p_wire.Type != VH_TYPE_STRING || p_wire.String.Len <= 0 || p_wire.String.Utf8 == nullptr) {
		return std::string();
	}
	return std::string(p_wire.String.Utf8, size_t(p_wire.String.Len));
}

// §8.4's three sentences, by the status Godot answered.
std::string call_sentence(const char *p_verb, const std::string &p_member, int64_t p_handle, int32_t p_status) {
	const std::string head = std::string(p_verb) + " `" + p_member + "` on Godot object " + std::to_string(p_handle);
	switch (p_status) {
		case VH_CALL_DEAD_OBJECT:
			return head + ", which Godot has already freed. Test IsInstanceValid[...] before reaching through a reference the scene may have dropped.";
		case VH_CALL_BAD_VALUE:
			return head + ", and the value has no representation on the Verse bridge. This is a gap in the type table in tools/gen_verse_api.py.";
		case VH_CALL_BAD_ARITY:
			return head + " with the wrong number of arguments. The generated Verse mirror and this build of Godot disagree; regenerate with tools/gen_verse_api.py.";
		default:
			return head + ", which has no such member. The generated Verse mirror and this build of Godot disagree; regenerate with tools/gen_verse_api.py.";
	}
}

// §8.31's two sentences.
std::string reference_sentence(const char *p_verb, int64_t p_ref) {
	if (p_ref == 0) {
		return std::string(p_verb) + " a Godot container that names nothing. A container built in Verse -- `godot_array{}` and the like -- holds no Godot value; one has to come back from Godot.";
	}
	return std::string(p_verb) + " a Godot container the bridge no longer holds (reference " + std::to_string(p_ref) +
			"). A reference is released when the Verse value holding it is collected, so this is a handle kept past the object that owned it.";
}

// The clause a vh_signal_reject reads as (include/verse_host_abi.h's comments on each code).
std::string reject_reason(int32_t p_reject, const std::string &p_detail) {
	switch (p_reject) {
		case VH_SIGNAL_IS_VAR:
			return "it is declared `var`, and a signal is an identity rather than a value";
		case VH_SIGNAL_NOT_PUBLIC:
			return "it is not `<public>`";
		case VH_SIGNAL_NO_GODOT_OWNER:
			return "its class does not derive from a Godot object, so there is nothing to register it on";
		case VH_SIGNAL_PAYLOAD_UNSUPPORTED:
			return "its payload argument `" + p_detail + "` has no Godot type";
		case VH_SIGNAL_PAYLOAD_NESTED_STRUCT:
			return "its payload field `" + p_detail + "` is itself a struct, and a struct payload decomposes one level only";
		case VH_SIGNAL_NEEDS_ATTRIBUTE:
			return "it carries no `@export_signal`";
		default:
			return "it was refused when the class was analysed";
	}
}

class ScopedArena {
public:
	ScopedArena() :
			arena(std::make_unique<HostArena>()) {}
	vh_arena *get() { return arena.get(); }
	HostArena &host() { return *arena; }

private:
	std::unique_ptr<HostArena> arena;
};

// Wire arguments that outlive the native that built them, for a write deferred to commit.
struct OwnedWire {
	HostArena arena;
	std::vector<vh_value> values;
};

} // namespace

GodotBridge::GodotBridge(Interpreter &r_interpreter, const Sidecar &p_sidecar) :
		interpreter(r_interpreter),
		heap(r_interpreter.heap),
		program(r_interpreter.program),
		sidecar(p_sidecar) {
	heap.add_root_source(this);
}

GodotBridge::~GodotBridge() {
	heap.remove_root_source(this);
}

void GodotBridge::visit_roots(CellVisitor &r_visitor) const {
	for (const auto &entry : bindings) {
		r_visitor.visit(entry.second.event);
	}
	for (const auto &entry : callbacks) {
		r_visitor.visit(entry.second.function);
	}
	for (const auto &entry : awaits) {
		r_visitor.visit(entry.second.signal);
	}
}

void GodotBridge::bind_program() {
	for (const PackageCell *package : program.packages) {
		for (const PackageDefinition &definition : package->definitions) {
			if (definition.path == nullptr) {
				continue;
			}
			definitions.emplace(definition.path->text, definition.value);
			if (is_cell_kind(definition.value, CellKind::Class)) {
				classes_by_path.emplace(definition.path->text, cell_as<ClassCell>(definition.value));
			} else if (is_cell_kind(definition.value, CellKind::Enumeration)) {
				enumerations.emplace(definition.path->text, cell_as<EnumerationCell>(definition.value));
			}
		}
	}
	variant_class = class_at_path("(/Godot.org/Godot:)variant");
	vh_object_class = class_at_path("(/Godot.org/Godot:)vh_object");
	vh_signal_class = class_at_path("(/Godot.org/Godot:)vh_signal");
	rid_class = class_at_path("(/Godot.org/Godot:)rid");
	godot_array_class = class_at_path("(/Godot.org/Godot:)godot_array");
	dictionary_class = class_at_path("(/Godot.org/Godot:)dictionary");
	callable_class = class_at_path("(/Godot.org/Godot:)callable");
	signal_ref_class = class_at_path("(/Godot.org/Godot:)signal_ref");
	for (const verse_math::layout &layout : verse_math::layouts) {
		const ClassCell *math = class_at_path(std::string("(/Godot.org/Godot:)") + layout.verse_name);
		math_classes[layout.variant_tag] = math;
		if (math != nullptr) {
			math_tag_by_class[math] = layout.variant_tag;
		}
	}
	for (const verse_api::class_mapping &mapping : verse_api::classes) {
		const ClassIndexEntry *entry = program.find_class(ClassOrigin::Mirrored, mapping.verse_name);
		if (entry != nullptr) {
			mirrored_by_godot_name.emplace(mapping.godot_name, entry->class_cell);
		}
	}
	for (const SidecarClass &entry : sidecar.classes) {
		for (const auto &method : entry.method_types) {
			types_by_method.emplace(method.first, &method.second);
		}
	}
	if (variant_class != nullptr) {
		const ClassLayout &layout = interpreter.layouts.get(variant_class);
		for (int32_t lane = 0; lane < 23; ++lane) {
			const LayoutField *field = find_slot_by_unqualified_name(layout, lane < 22 ? kVariantLanes[lane] : "Text");
			variant_slots[lane] = field != nullptr ? int32_t(field->slot) : -1;
		}
	}
}

const ClassCell *GodotBridge::class_at_path(const std::string &p_path) const {
	const auto found = classes_by_path.find(p_path);
	return found == classes_by_path.end() ? nullptr : found->second;
}

const ClassCell *GodotBridge::mirrored_class(std::string_view p_godot_name) const {
	const auto found = mirrored_by_godot_name.find(std::string(p_godot_name));
	return found == mirrored_by_godot_name.end() ? nullptr : found->second;
}

const EnumerationCell *GodotBridge::enumeration(const std::string &p_decorated) const {
	const auto found = enumerations.find(p_decorated);
	return found == enumerations.end() ? nullptr : found->second;
}

Value GodotBridge::definition(const std::string &p_path) const {
	auto found = definitions.find(p_path);
	if (found == definitions.end()) {
		found = definitions.find(std::string(unqualified_name(p_path)));
	}
	return found == definitions.end() ? Value::empty() : follow(found->second);
}

const SidecarMethodTypes *GodotBridge::method_types(std::string_view p_decorated) const {
	const auto found = types_by_method.find(std::string(p_decorated));
	return found == types_by_method.end() ? nullptr : found->second;
}

void GodotBridge::register_instance(vh_instance *p_instance) {
	if (p_instance->handle != 0) {
		instances[p_instance->handle] = p_instance;
	}
}

void GodotBridge::forget_instance(vh_instance *p_instance) {
	const auto found = instances.find(p_instance->handle);
	if (found != instances.end() && found->second == p_instance) {
		instances.erase(found);
	}
}

vh_instance *GodotBridge::instance_for(int64_t p_handle) const {
	const auto found = instances.find(p_handle);
	return found == instances.end() ? nullptr : found->second;
}

Value *GodotBridge::field_slot(ObjectCell *p_object, std::string_view p_name) {
	if (p_object == nullptr) {
		return nullptr;
	}
	if (p_object->layout == nullptr) {
		interpreter.layouts.lay_out_value_object(p_object);
		if (p_object->layout == nullptr) {
			return nullptr;
		}
	}
	const std::pair<const void *, std::string> key(p_object->layout, std::string(p_name));
	auto found = slot_cache.find(key);
	if (found == slot_cache.end()) {
		const LayoutField *field = find_slot_by_unqualified_name(*p_object->layout, p_name);
		found = slot_cache.emplace(key, field != nullptr ? int32_t(field->slot) : -1).first;
	}
	if (found->second < 0 || size_t(found->second) >= p_object->field_values.size()) {
		return nullptr;
	}
	return &p_object->field_values[size_t(found->second)];
}

Value GodotBridge::field_value(Value p_object, std::string_view p_name) {
	const Value object = follow(p_object);
	if (!is_cell_kind(object, CellKind::Object)) {
		return Value::empty();
	}
	Value *slot = field_slot(cell_as<ObjectCell>(object), p_name);
	if (slot == nullptr) {
		return Value::empty();
	}
	Value value = follow(read_slot(*slot));
	if (is_cell_kind(value, CellKind::Ref)) {
		value = follow(cell_as<RefCell>(value)->content);
	}
	return value;
}

int64_t GodotBridge::handle_of(Value p_object) {
	int64_t handle = 0;
	RuntimeError error;
	const Value value = field_value(p_object, "Handle");
	if (value.is_empty() || int_to_int64(value, handle, error) != Outcome::Ok) {
		return 0;
	}
	return handle;
}

Value GodotBridge::make_variant(const VariantLanes &p_lanes) {
	if (variant_class == nullptr) {
		return Value::empty();
	}
	const ClassLayout &layout = interpreter.layouts.get(variant_class);
	ObjectCell *object = interpreter.layouts.new_object(heap, layout);
	const auto set = [object](int32_t p_slot, Value p_value) {
		if (p_slot >= 0) {
			object->field_values[size_t(p_slot)] = p_value;
			object->created[size_t(p_slot)] = true;
		}
	};
	set(variant_slots[0], make_int(heap, p_lanes.tag));
	set(variant_slots[1], make_int(heap, p_lanes.ref));
	for (int32_t index = 0; index < 4; ++index) {
		set(variant_slots[2 + index], make_int(heap, p_lanes.i[index]));
	}
	for (int32_t index = 0; index < 16; ++index) {
		set(variant_slots[6 + index], Value::from_float(p_lanes.f[index]));
	}
	set(variant_slots[kLaneText], make_string(heap, p_lanes.text));
	return Value::from_cell(object);
}

bool GodotBridge::read_variant(Value p_variant, VariantLanes &r_lanes) {
	r_lanes = VariantLanes();
	const Value value = follow(p_variant);
	if (!is_cell_kind(value, CellKind::Object)) {
		return false;
	}
	ObjectCell *object = cell_as<ObjectCell>(value);
	if (object->layout == nullptr) {
		interpreter.layouts.lay_out_value_object(object);
	}
	if (object->object_class != variant_class || object->layout == nullptr) {
		return false;
	}
	const auto lane = [object](int32_t p_slot) -> Value {
		if (p_slot < 0 || size_t(p_slot) >= object->field_values.size()) {
			return Value::empty();
		}
		Value held = follow(read_slot(object->field_values[size_t(p_slot)]));
		if (is_cell_kind(held, CellKind::Ref)) {
			held = follow(cell_as<RefCell>(held)->content);
		}
		return held;
	};
	RuntimeError error;
	const auto int_lane = [&](int32_t p_slot, int64_t &r_value) {
		const Value held = lane(p_slot);
		if (!held.is_empty() && is_int(held)) {
			int_to_int64(held, r_value, error);
		}
	};
	int_lane(variant_slots[0], r_lanes.tag);
	int_lane(variant_slots[1], r_lanes.ref);
	for (int32_t index = 0; index < 4; ++index) {
		int_lane(variant_slots[2 + index], r_lanes.i[index]);
	}
	for (int32_t index = 0; index < 16; ++index) {
		const Value held = lane(variant_slots[6 + index]);
		if (held.is_float()) {
			r_lanes.f[index] = held.as_float();
		}
	}
	string_bytes(lane(variant_slots[kLaneText]), r_lanes.text);
	return true;
}

// §6.1.
bool GodotBridge::variant_to_wire(const VariantLanes &p_lanes, vh_arena *p_arena, vh_value &r_wire) {
	r_wire = vh_value{};
	r_wire.VariantTag = int32_t(p_lanes.tag);
	switch (p_lanes.tag) {
		case VH_VARIANT_NIL:
			r_wire.Type = VH_TYPE_VOID;
			return true;
		case VH_VARIANT_BOOL:
			r_wire.Type = VH_TYPE_LOGIC;
			r_wire.Logic = p_lanes.i[0] != 0 ? 1 : 0;
			return true;
		case VH_VARIANT_INT:
		case VH_VARIANT_RID:
			r_wire.Type = VH_TYPE_INT;
			r_wire.Int = p_lanes.i[0];
			return true;
		case VH_VARIANT_FLOAT:
			r_wire.Type = VH_TYPE_FLOAT;
			r_wire.Float = p_lanes.f[0];
			return true;
		case VH_VARIANT_OBJECT:
			r_wire.Type = VH_TYPE_INT;
			r_wire.Int = p_lanes.ref;
			return true;
		default:
			break;
	}
	if (is_string_tag(p_lanes.tag)) {
		if (!string_to_wire(p_lanes.text, p_arena, r_wire)) {
			return false;
		}
		r_wire.VariantTag = int32_t(p_lanes.tag);
		return true;
	}
	if (is_reference_tag(p_lanes.tag)) {
		r_wire.Type = VH_TYPE_REF;
		r_wire.Ref = p_lanes.ref;
		return true;
	}
	std::vector<bool> kinds;
	leaf_kinds(p_lanes.tag, kinds);
	if (kinds.empty()) {
		r_wire.Type = VH_TYPE_VOID;
		return false;
	}
	vh_value *items = allocate<vh_value>(p_arena, kinds.size());
	if (items == nullptr) {
		return false;
	}
	size_t next = 0;
	size_t next_int = 0;
	for (bool is_int_leaf : kinds) {
		if (is_int_leaf && next_int < 4) {
			items[next].Type = VH_TYPE_INT;
			items[next].Int = p_lanes.i[next_int++];
			++next;
		}
	}
	size_t next_float = 0;
	for (bool is_int_leaf : kinds) {
		if (!is_int_leaf && next_float < 16) {
			items[next].Type = VH_TYPE_FLOAT;
			items[next].Float = p_lanes.f[next_float++];
			++next;
		}
	}
	r_wire.Type = VH_TYPE_TUPLE;
	r_wire.Seq.Items = items;
	r_wire.Seq.Count = int32_t(next);
	return true;
}

// §6.2.
void GodotBridge::wire_to_lanes(const vh_value &p_wire, VariantLanes &r_lanes) {
	r_lanes = VariantLanes();
	int64_t tag = p_wire.VariantTag;
	if (tag == VH_VARIANT_NIL) {
		switch (p_wire.Type) {
			case VH_TYPE_LOGIC:
				tag = VH_VARIANT_BOOL;
				break;
			case VH_TYPE_INT:
				tag = VH_VARIANT_INT;
				break;
			case VH_TYPE_FLOAT:
				tag = VH_VARIANT_FLOAT;
				break;
			case VH_TYPE_STRING:
				tag = VH_VARIANT_STRING;
				break;
			default:
				break;
		}
	}
	r_lanes.tag = tag;
	switch (tag) {
		case VH_VARIANT_NIL:
			return;
		case VH_VARIANT_BOOL:
			r_lanes.i[0] = wire_int(p_wire) != 0 ? 1 : 0;
			return;
		case VH_VARIANT_INT:
		case VH_VARIANT_RID:
			r_lanes.i[0] = wire_int(p_wire);
			return;
		case VH_VARIANT_FLOAT:
			r_lanes.f[0] = wire_double(p_wire);
			return;
		case VH_VARIANT_OBJECT:
			r_lanes.ref = wire_int(p_wire);
			return;
		default:
			break;
	}
	if (is_string_tag(tag)) {
		r_lanes.text = wire_text(p_wire);
		return;
	}
	if (is_reference_tag(tag)) {
		r_lanes.ref = p_wire.Type == VH_TYPE_REF ? p_wire.Ref : wire_int(p_wire);
		return;
	}
	if (p_wire.Type != VH_TYPE_TUPLE && p_wire.Type != VH_TYPE_ARRAY) {
		return;
	}
	std::vector<bool> kinds;
	leaf_kinds(tag, kinds);
	int32_t next = 0;
	size_t next_int = 0;
	size_t next_float = 0;
	for (bool is_int_leaf : kinds) {
		if (next >= p_wire.Seq.Count) {
			break;
		}
		const vh_value &item = p_wire.Seq.Items[next++];
		if (is_int_leaf) {
			if (next_int < 4) {
				r_lanes.i[next_int++] = wire_int(item);
			}
		} else if (next_float < 16) {
			r_lanes.f[next_float++] = wire_double(item);
		}
	}
}

int32_t GodotBridge::math_tag_of(Value p_value) const {
	const Value value = follow(p_value);
	if (!is_cell_kind(value, CellKind::Object)) {
		return 0;
	}
	const auto found = math_tag_by_class.find(cell_as<ObjectCell>(value)->object_class);
	return found == math_tag_by_class.end() ? 0 : found->second;
}

bool GodotBridge::struct_leaves(Value p_struct, int32_t p_tag, std::vector<double> &r_leaves) {
	const verse_math::layout *layout = math_layout(p_tag);
	if (layout == nullptr) {
		return false;
	}
	for (int32_t index = 0; index < layout->field_count; ++index) {
		const verse_math::field &field = layout->fields[index];
		const Value value = field_value(p_struct, field.name);
		if (value.is_empty()) {
			return false;
		}
		if (field.nested_tag != 0) {
			if (!struct_leaves(value, field.nested_tag, r_leaves)) {
				return false;
			}
		} else if (value.is_float()) {
			r_leaves.push_back(value.as_float());
		} else if (is_int(value)) {
			r_leaves.push_back(int_to_float(value));
		} else {
			return false;
		}
	}
	return true;
}

Value GodotBridge::struct_from_leaves(int32_t p_tag, const std::vector<double> &p_leaves, size_t &r_next) {
	const verse_math::layout *layout = math_layout(p_tag);
	const ClassCell *math = p_tag > 0 && p_tag < VH_VARIANT_MAX ? math_classes[p_tag] : nullptr;
	if (layout == nullptr || math == nullptr) {
		return Value::empty();
	}
	ObjectCell *object = interpreter.layouts.new_object(heap, interpreter.layouts.get(math));
	for (int32_t index = 0; index < layout->field_count; ++index) {
		const verse_math::field &field = layout->fields[index];
		Value value;
		if (field.nested_tag != 0) {
			value = struct_from_leaves(field.nested_tag, p_leaves, r_next);
			if (value.is_empty()) {
				return Value::empty();
			}
		} else {
			const double leaf = r_next < p_leaves.size() ? p_leaves[r_next] : 0.0;
			++r_next;
			value = field.is_int ? make_int(heap, std::isfinite(leaf) ? int64_t(leaf) : 0) : Value::from_float(leaf);
		}
		Value *slot = field_slot(object, field.name);
		if (slot == nullptr) {
			return Value::empty();
		}
		*slot = value;
		object->created[size_t(slot - object->field_values.data())] = true;
	}
	return Value::from_cell(object);
}

bool GodotBridge::leaves_of_wire(const vh_value &p_wire, std::vector<double> &r_leaves) const {
	if (p_wire.Type != VH_TYPE_TUPLE && p_wire.Type != VH_TYPE_ARRAY) {
		return false;
	}
	for (int32_t index = 0; index < p_wire.Seq.Count; ++index) {
		const vh_value &item = p_wire.Seq.Items[index];
		if (item.Type == VH_TYPE_TUPLE || item.Type == VH_TYPE_ARRAY) {
			if (!leaves_of_wire(item, r_leaves)) {
				return false;
			}
		} else {
			r_leaves.push_back(wire_double(item));
		}
	}
	return true;
}

const ClassCell *GodotBridge::declared_class(const SidecarMemberType &p_type) const {
	if (!p_type.has_ref) {
		return nullptr;
	}
	const ClassOrigin origins[3] = { ClassOrigin::Mirrored, ClassOrigin::Script, ClassOrigin::Binding };
	const ClassOrigin first = p_type.ref_origin == 1 ? ClassOrigin::Mirrored : p_type.ref_origin == 2 ? ClassOrigin::Script : ClassOrigin::Binding;
	if (const ClassIndexEntry *entry = program.find_class(first, p_type.ref)) {
		return entry->class_cell;
	}
	for (ClassOrigin origin : origins) {
		if (const ClassIndexEntry *entry = program.find_class(origin, p_type.ref)) {
			return entry->class_cell;
		}
	}
	return nullptr;
}

Value GodotBridge::wrap_reference(int64_t p_tag, int64_t p_ref) {
	const ClassCell *wrapper = nullptr;
	switch (p_tag) {
		case VH_VARIANT_CALLABLE:
			wrapper = callable_class;
			break;
		case VH_VARIANT_SIGNAL:
			wrapper = signal_ref_class;
			break;
		case VH_VARIANT_DICTIONARY:
			wrapper = dictionary_class;
			break;
		default:
			wrapper = godot_array_class;
			break;
	}
	if (wrapper == nullptr) {
		return Value::empty();
	}
	ObjectCell *object = interpreter.layouts.new_object(heap, interpreter.layouts.get(wrapper));
	for (size_t index = 0; index < object->field_values.size(); ++index) {
		object->field_values[index] = Value::from_int32(0);
		object->created[index] = true;
	}
	Value *slot = field_slot(object, "Ref");
	if (slot == nullptr) {
		return Value::empty();
	}
	*slot = make_int(heap, p_ref);
	interpreter.adopt_ref(object, p_ref);
	return Value::from_cell(object);
}

bool GodotBridge::packed_elements(const vh_value &p_wire, std::vector<vh_value> &r_elements, HostArena &r_arena) {
	if (p_wire.Type == VH_TYPE_ARRAY || p_wire.Type == VH_TYPE_TUPLE) {
		for (int32_t index = 0; index < p_wire.Seq.Count; ++index) {
			r_elements.push_back(p_wire.Seq.Items[index]);
		}
		return true;
	}
	if (p_wire.Type != VH_TYPE_REF) {
		return false;
	}
	const vh_godot_api &api = interpreter.godot;
	if (api.RefContents == nullptr) {
		return false;
	}
	vh_value contents = {};
	const int32_t status = api.RefContents(api.Ctx, p_wire.Ref, &r_arena, &contents);
	if (api.ReleaseRef != nullptr) {
		api.ReleaseRef(api.Ctx, p_wire.Ref);
	}
	if (status != VH_CALL_OK || (contents.Type != VH_TYPE_ARRAY && contents.Type != VH_TYPE_TUPLE)) {
		return false;
	}
	for (int32_t index = 0; index < contents.Seq.Count; ++index) {
		r_elements.push_back(contents.Seq.Items[index]);
	}
	return true;
}

Value GodotBridge::element_from_wire(const vh_value &p_wire, int32_t p_tag) {
	int32_t math = 0;
	if (math_layout(p_wire.VariantTag) != nullptr) {
		math = p_wire.VariantTag;
	} else if (p_tag >= VH_VARIANT_PACKED_VECTOR2_ARRAY) {
		for (const verse_math::layout &layout : verse_math::layouts) {
			if (layout.packed_array_tag == p_tag) {
				math = layout.variant_tag;
			}
		}
	}
	if (math != 0 && (p_wire.Type == VH_TYPE_TUPLE || p_wire.Type == VH_TYPE_ARRAY)) {
		std::vector<double> leaves;
		leaves_of_wire(p_wire, leaves);
		size_t next = 0;
		return struct_from_leaves(math, leaves, next);
	}
	switch (p_wire.Type) {
		case VH_TYPE_INT:
			if (p_tag == VH_VARIANT_PACKED_FLOAT32_ARRAY || p_tag == VH_VARIANT_PACKED_FLOAT64_ARRAY) {
				return Value::from_float(double(p_wire.Int));
			}
			return make_int(heap, p_wire.Int);
		case VH_TYPE_FLOAT:
			return Value::from_float(p_wire.Float);
		case VH_TYPE_LOGIC:
			return heap.logic(p_wire.Logic != 0);
		case VH_TYPE_STRING:
			return make_string(heap, wire_text(p_wire));
		default: {
			Value value;
			if (!wire_to_value(heap, p_wire, p_wire.Type, value)) {
				return Value::empty();
			}
			return value;
		}
	}
}

bool GodotBridge::user_struct_from_fields(const SidecarMemberType &p_type, const vh_value *p_fields, int32_t p_count, Value &r_value, std::string &r_why) {
	const ClassCell *struct_class = class_at_path(p_type.user_struct_name);
	if (struct_class == nullptr || size_t(p_count) != p_type.field_keys.size()) {
		r_why = "a struct argument whose fields do not match " + p_type.user_struct_name;
		return false;
	}
	const ClassLayout &layout = interpreter.layouts.get(struct_class);
	ObjectCell *object = interpreter.layouts.new_object(heap, layout);
	r_value = Value::from_cell(object);
	for (size_t index = 0; index < p_type.field_keys.size(); ++index) {
		Value field;
		if (!wire_to_member(p_fields[index], p_type.field_types[index], field, r_why)) {
			return false;
		}
		bool stored = false;
		for (const LayoutField &entry : layout.fields) {
			if (entry.kind == FieldKind::Slot && entry.name != nullptr && entry.name->text == p_type.field_keys[index]) {
				object->field_values[entry.slot] = field;
				object->created[entry.slot] = true;
				stored = true;
				break;
			}
		}
		if (!stored) {
			Value *slot = field_slot(object, index < p_type.field_names.size() ? p_type.field_names[index] : std::string());
			if (slot == nullptr) {
				r_why = "a struct field " + p_type.field_keys[index] + " its class does not have";
				return false;
			}
			*slot = field;
			object->created[size_t(slot - object->field_values.data())] = true;
		}
	}
	for (size_t index = 0; index < object->field_values.size(); ++index) {
		if (!object->created[index]) {
			const LayoutField *entry = layout.find(object->field_names[index]);
			if (entry != nullptr && !entry->value.is_uninitialized()) {
				object->field_values[index] = entry->value;
				object->created[index] = true;
			}
		}
	}
	return true;
}

// §12.3: what a value of a declared type is on the wire, and back.
bool GodotBridge::wire_to_member(const vh_value &p_wire, const SidecarMemberType &p_type, Value &r_value, std::string &r_why) {
	const SidecarExport &described = p_type.described;
	if (p_type.has_ref) {
		int64_t handle = 0;
		if (p_wire.Type == VH_TYPE_OPTION) {
			handle = p_wire.Option != nullptr ? wire_int(*p_wire.Option) : 0;
		} else if (p_wire.Type == VH_TYPE_INT || p_wire.Type == VH_TYPE_REF || p_wire.Type == VH_TYPE_FLOAT) {
			handle = wire_int(p_wire);
		} else if (p_wire.Type != VH_TYPE_VOID) {
			r_why = "an object argument that is not a handle";
			return false;
		}
		if (handle == 0) {
			if (!p_type.ref_option) {
				r_why = "a null object for a parameter that cannot be empty";
				return false;
			}
			r_value = heap.false_value();
			return true;
		}
		Value object;
		if (object_for_handle(handle, nullptr, object) != Outcome::Ok) {
			r_why = "an object that could not be built";
			return false;
		}
		const ClassCell *declared = declared_class(p_type);
		if (declared != nullptr) {
			const ObjectCell *cell = is_cell_kind(object, CellKind::Object) ? cell_as<ObjectCell>(object) : nullptr;
			if (cell == nullptr || !class_inherits(cell->object_class, declared)) {
				r_why = "an object that is not a " + p_type.ref;
				return false;
			}
		}
		r_value = p_type.ref_option ? make_option(heap, object) : object;
		return true;
	}
	if (described.tag == VH_VARIANT_RID && rid_class != nullptr && described.type != VH_TYPE_VARIANT) {
		ObjectCell *object = interpreter.layouts.new_object(heap, interpreter.layouts.get(rid_class));
		Value *slot = field_slot(object, "Id");
		if (slot == nullptr) {
			r_why = "a rid with no Id";
			return false;
		}
		*slot = make_int(heap, wire_int(p_wire));
		object->created[size_t(slot - object->field_values.data())] = true;
		r_value = Value::from_cell(object);
		return true;
	}
	if (!p_type.struct_name.empty()) {
		const verse_math::layout *layout = math_layout_named(p_type.struct_name);
		std::vector<double> leaves;
		if (layout == nullptr || !leaves_of_wire(p_wire, leaves)) {
			r_why = "a " + p_type.struct_name + " argument that is not its components";
			return false;
		}
		size_t next = 0;
		r_value = struct_from_leaves(layout->variant_tag, leaves, next);
		return !r_value.is_empty();
	}
	if (p_type.has_enum) {
		const int64_t ordinal = wire_int(p_wire);
		const EnumerationCell *enumerated = enumeration(p_type.enum_name);
		if (enumerated == nullptr || ordinal < 0 || ordinal >= int64_t(enumerated->enumerators.size()) ||
				(p_type.enumerators > 0 && ordinal >= p_type.enumerators)) {
			r_why = "an enumerator ordinal outside " + p_type.enum_name;
			return false;
		}
		r_value = Value::from_cell(enumerated->enumerators[size_t(ordinal)]);
		return true;
	}
	if (p_type.has_user_struct) {
		if (p_wire.Type != VH_TYPE_TUPLE && p_wire.Type != VH_TYPE_ARRAY) {
			r_why = "a struct argument that is not its fields";
			return false;
		}
		return user_struct_from_fields(p_type, p_wire.Seq.Items, p_wire.Seq.Count, r_value, r_why);
	}
	switch (described.type) {
		case VH_TYPE_VARIANT: {
			VariantLanes lanes;
			wire_to_lanes(p_wire, lanes);
			r_value = make_variant(lanes);
			return !r_value.is_empty();
		}
		case VH_TYPE_REF: {
			if (p_wire.Type != VH_TYPE_REF && p_wire.Type != VH_TYPE_INT) {
				r_why = "a reference argument that is not a reference";
				return false;
			}
			r_value = wrap_reference(described.tag, p_wire.Type == VH_TYPE_REF ? p_wire.Ref : p_wire.Int);
			return !r_value.is_empty();
		}
		case VH_TYPE_ARRAY: {
			if (p_wire.Type == VH_TYPE_STRING && described.tag == VH_VARIANT_STRING) {
				break;
			}
			ScopedArena arena;
			std::vector<vh_value> elements;
			if (!packed_elements(p_wire, elements, arena.host())) {
				r_why = "an array argument that is not a sequence";
				return false;
			}
			std::vector<Value> values;
			values.reserve(elements.size());
			const int32_t element_tag = described.element_tag != 0 ? described.element_tag : described.tag;
			for (const vh_value &element : elements) {
				const Value value = element_from_wire(element, element_tag);
				if (value.is_empty()) {
					r_why = "an array element with no Verse spelling";
					return false;
				}
				values.push_back(value);
			}
			r_value = make_array(heap, values, false);
			return true;
		}
		default:
			break;
	}
	if (described.type == VH_TYPE_FLOAT && p_wire.Type == VH_TYPE_LOGIC) {
		r_value = Value::from_float(p_wire.Logic != 0 ? 1.0 : 0.0);
		return true;
	}
	if (described.type == VH_TYPE_INT && p_wire.Type == VH_TYPE_FLOAT) {
		r_value = make_int(heap, wire_int(p_wire));
		return true;
	}
	if (!wire_to_value(heap, p_wire, described.type, r_value)) {
		r_why = "an argument of wire type " + std::to_string(p_wire.Type) + " for a parameter of type " + std::to_string(described.type);
		return false;
	}
	return true;
}

bool GodotBridge::self_described_to_wire(Value p_value, vh_arena *p_arena, vh_value &r_wire, std::string &r_why) {
	r_wire = vh_value{};
	const Value value = follow(read_slot(p_value));
	if (const int32_t tag = math_tag_of(value)) {
		std::vector<double> leaves;
		std::vector<bool> kinds;
		leaf_kinds(tag, kinds);
		if (!struct_leaves(value, tag, leaves)) {
			r_why = "a struct whose fields cannot be read";
			return false;
		}
		vh_value *items = allocate<vh_value>(p_arena, leaves.size());
		if (items == nullptr && !leaves.empty()) {
			r_why = "the arena is full";
			return false;
		}
		for (size_t index = 0; index < leaves.size(); ++index) {
			if (index < kinds.size() && kinds[index]) {
				items[index].Type = VH_TYPE_INT;
				items[index].Int = int64_t(leaves[index]);
			} else {
				items[index].Type = VH_TYPE_FLOAT;
				items[index].Float = leaves[index];
			}
		}
		r_wire.Type = VH_TYPE_TUPLE;
		r_wire.VariantTag = tag;
		r_wire.Seq.Items = items;
		r_wire.Seq.Count = int32_t(leaves.size());
		return true;
	}
	if (is_cell_kind(value, CellKind::Object)) {
		const ObjectCell *object = cell_as<ObjectCell>(value);
		if (object->object_class == variant_class && variant_class != nullptr) {
			VariantLanes lanes;
			read_variant(value, lanes);
			return variant_to_wire(lanes, p_arena, r_wire);
		}
		if (object->object_class == rid_class && rid_class != nullptr) {
			RuntimeError error;
			int64_t id = 0;
			int_to_int64(field_value(value, "Id"), id, error);
			r_wire.Type = VH_TYPE_INT;
			r_wire.VariantTag = VH_VARIANT_RID;
			r_wire.Int = id;
			return true;
		}
		if (vh_object_class != nullptr && class_inherits(object->object_class, vh_object_class)) {
			r_wire.Type = VH_TYPE_INT;
			r_wire.VariantTag = VH_VARIANT_OBJECT;
			r_wire.Int = handle_of(value);
			return true;
		}
		const Value ref = field_value(value, "Ref");
		if (!ref.is_empty() && is_int(ref)) {
			RuntimeError error;
			int64_t id = 0;
			int_to_int64(ref, id, error);
			r_wire.Type = VH_TYPE_REF;
			r_wire.Ref = id;
			return true;
		}
	}
	if (is_cell_kind(value, CellKind::Array) || is_cell_kind(value, CellKind::MutableArray)) {
		const ArrayCell *array = cell_as<ArrayCell>(value);
		if (array->storage == ArrayCell::Storage::Char8) {
			std::string bytes;
			string_bytes(value, bytes);
			return string_to_wire(bytes, p_arena, r_wire);
		}
		vh_value *items = allocate<vh_value>(p_arena, array->length());
		if (items == nullptr && array->length() != 0) {
			r_why = "the arena is full";
			return false;
		}
		for (size_t index = 0; index < array->length(); ++index) {
			if (!self_described_to_wire(array->get(index), p_arena, items[index], r_why)) {
				return false;
			}
		}
		r_wire.Type = VH_TYPE_ARRAY;
		r_wire.Seq.Items = items;
		r_wire.Seq.Count = int32_t(array->length());
		return true;
	}
	if (is_cell_kind(value, CellKind::Option)) {
		vh_value *content = allocate<vh_value>(p_arena, 1);
		if (content == nullptr) {
			r_why = "the arena is full";
			return false;
		}
		if (!self_described_to_wire(cell_as<OptionCell>(value)->content, p_arena, *content, r_why)) {
			return false;
		}
		r_wire.Type = VH_TYPE_OPTION;
		r_wire.Option = content;
		return true;
	}
	if (is_cell_kind(value, CellKind::Map) || is_cell_kind(value, CellKind::MutableMap)) {
		const MapCell *map = cell_as<MapCell>(value);
		vh_pair *pairs = allocate<vh_pair>(p_arena, map->entries.size());
		if (pairs == nullptr && !map->entries.empty()) {
			r_why = "the arena is full";
			return false;
		}
		for (size_t index = 0; index < map->entries.size(); ++index) {
			if (!self_described_to_wire(map->entries[index].key, p_arena, pairs[index].Key, r_why) ||
					!self_described_to_wire(map->entries[index].value, p_arena, pairs[index].Value, r_why)) {
				return false;
			}
		}
		r_wire.Type = VH_TYPE_MAP;
		r_wire.Map.Pairs = pairs;
		r_wire.Map.Count = int32_t(map->entries.size());
		return true;
	}
	if (is_int(value)) {
		return value_to_wire(value, VH_TYPE_INT, 0, p_arena, r_wire, r_why);
	}
	if (value.is_float()) {
		return value_to_wire(value, VH_TYPE_FLOAT, 0, p_arena, r_wire, r_why);
	}
	if (value.is_char8() || value.is_char32()) {
		return value_to_wire(value, VH_TYPE_CHAR, 0, p_arena, r_wire, r_why);
	}
	if (is_cell_kind(value, CellKind::False) || is_cell_kind(value, CellKind::True)) {
		return value_to_wire(value, VH_TYPE_LOGIC, 0, p_arena, r_wire, r_why);
	}
	if (is_cell_kind(value, CellKind::Enumerator)) {
		r_wire.Type = VH_TYPE_INT;
		r_wire.Int = cell_as<EnumeratorCell>(value)->ordinal;
		return true;
	}
	r_why = std::string("a ") + (value.is_cell() ? cell_kind_name(value.as_cell()->kind) : "value") + ", which has no Godot spelling";
	return false;
}

bool GodotBridge::member_to_wire(Value p_value, const SidecarMemberType &p_type, vh_arena *p_arena, vh_value &r_wire, std::string &r_why) {
	r_wire = vh_value{};
	const SidecarExport &described = p_type.described;
	Value value = follow(read_slot(p_value));
	if (is_cell_kind(value, CellKind::Ref)) {
		value = follow(cell_as<RefCell>(value)->content);
	}
	if (p_type.has_ref) {
		r_wire.Type = VH_TYPE_INT;
		r_wire.VariantTag = VH_VARIANT_OBJECT;
		if (is_cell_kind(value, CellKind::False)) {
			return true;
		}
		if (is_cell_kind(value, CellKind::Option)) {
			value = follow(cell_as<OptionCell>(value)->content);
		}
		r_wire.Int = handle_of(value);
		return true;
	}
	if (described.tag == VH_VARIANT_RID && described.type != VH_TYPE_VARIANT) {
		RuntimeError error;
		int64_t id = 0;
		const Value held = is_int(value) ? value : field_value(value, "Id");
		int_to_int64(held, id, error);
		r_wire.Type = VH_TYPE_INT;
		r_wire.VariantTag = VH_VARIANT_RID;
		r_wire.Int = id;
		return true;
	}
	if (!p_type.struct_name.empty() || math_tag_of(value) != 0) {
		if (!self_described_to_wire(value, p_arena, r_wire, r_why)) {
			return false;
		}
		if (described.tag != 0) {
			r_wire.VariantTag = described.tag;
		}
		return true;
	}
	if (p_type.has_enum) {
		r_wire.Type = VH_TYPE_INT;
		r_wire.VariantTag = described.tag;
		r_wire.Int = is_cell_kind(value, CellKind::Enumerator) ? cell_as<EnumeratorCell>(value)->ordinal : 0;
		return true;
	}
	if (p_type.has_user_struct) {
		vh_value *items = allocate<vh_value>(p_arena, p_type.field_keys.size());
		for (size_t index = 0; index < p_type.field_keys.size(); ++index) {
			const std::string name = index < p_type.field_names.size() ? p_type.field_names[index] : std::string();
			if (!member_to_wire(field_value(value, name), p_type.field_types[index], p_arena, items[index], r_why)) {
				return false;
			}
		}
		r_wire.Type = VH_TYPE_TUPLE;
		r_wire.Seq.Items = items;
		r_wire.Seq.Count = int32_t(p_type.field_keys.size());
		return true;
	}
	switch (described.type) {
		case VH_TYPE_VARIANT: {
			VariantLanes lanes;
			if (!read_variant(value, lanes)) {
				r_why = "a variant result that is not a variant";
				return false;
			}
			return variant_to_wire(lanes, p_arena, r_wire);
		}
		case VH_TYPE_REF: {
			RuntimeError error;
			int64_t id = 0;
			int_to_int64(field_value(value, "Ref"), id, error);
			r_wire.Type = VH_TYPE_REF;
			r_wire.VariantTag = described.tag;
			r_wire.Ref = id;
			return true;
		}
		case VH_TYPE_ARRAY:
			if (described.tag != VH_VARIANT_STRING) {
				if (!self_described_to_wire(value, p_arena, r_wire, r_why)) {
					return false;
				}
				if (r_wire.Type == VH_TYPE_VOID || is_cell_kind(value, CellKind::False)) {
					r_wire.Type = VH_TYPE_ARRAY;
				}
				r_wire.VariantTag = described.tag;
				return true;
			}
			break;
		default:
			break;
	}
	return value_to_wire(value, described.type, described.tag, p_arena, r_wire, r_why);
}

bool GodotBridge::wire_to_arguments(const SidecarMethodTypes *p_types, const vh_value *p_args, int32_t p_count, bool p_pack,
		std::vector<Value> &r_arguments, std::string &r_why) {
	r_arguments.clear();
	if (p_types == nullptr) {
		for (int32_t index = 0; index < p_count; ++index) {
			Value value;
			if (!wire_to_value(heap, p_args[index], p_args[index].Type, value)) {
				r_why = "an argument with no Verse spelling";
				return false;
			}
			r_arguments.push_back(value);
		}
		return true;
	}
	if (p_pack && p_types->params.size() == 1 && p_types->params[0].has_user_struct && p_count > 1 &&
			size_t(p_count) == p_types->params[0].field_keys.size()) {
		Value packed;
		if (!user_struct_from_fields(p_types->params[0], p_args, p_count, packed, r_why)) {
			return false;
		}
		r_arguments.push_back(packed);
		return true;
	}
	if (size_t(p_count) > p_types->params.size()) {
		r_why = "more arguments than the method has parameters";
		return false;
	}
	for (int32_t index = 0; index < p_count; ++index) {
		Value value;
		if (!wire_to_member(p_args[index], p_types->params[size_t(index)], value, r_why)) {
			return false;
		}
		r_arguments.push_back(value);
	}
	return true;
}

const ClassCell *GodotBridge::class_of_handle(int64_t p_handle) {
	const auto cached = handle_classes.find(p_handle);
	if (cached != handle_classes.end()) {
		return cached->second;
	}
	const vh_godot_api &api = interpreter.godot;
	ScopedArena arena;
	const ClassCell *resolved = nullptr;
	if (api.GetScriptClassOf != nullptr) {
		vh_value name = {};
		if (api.GetScriptClassOf(api.Ctx, p_handle, arena.get(), &name) == VH_CALL_OK) {
			const std::string script = wire_text(name);
			for (const SidecarBinding &binding : sidecar.bindings) {
				if (!binding.script.empty() && binding.script == script) {
					if (const ClassIndexEntry *entry = program.find_class(ClassOrigin::Binding, binding.verse)) {
						resolved = entry->class_cell;
					}
					break;
				}
			}
		}
	}
	bool dead = false;
	if (resolved == nullptr && api.GetClassOf != nullptr) {
		vh_value name = {};
		const int32_t status = api.GetClassOf(api.Ctx, p_handle, arena.get(), &name);
		if (status == VH_CALL_OK) {
			const std::string godot_name = wire_text(name);
			for (const SidecarBinding &binding : sidecar.bindings) {
				if (!binding.godot.empty() && binding.godot == godot_name) {
					if (const ClassIndexEntry *entry = program.find_class(ClassOrigin::Binding, binding.verse)) {
						resolved = entry->class_cell;
					}
					break;
				}
			}
			if (resolved == nullptr) {
				resolved = mirrored_class(godot_name);
			}
		} else {
			dead = true;
		}
	}
	if (!dead) {
		handle_classes.emplace(p_handle, resolved);
	}
	return resolved;
}

Outcome GodotBridge::object_for_handle(int64_t p_handle, const ClassCell *p_fallback, Value &r_object) {
	if (p_handle != 0) {
		if (vh_instance *instance = instance_for(p_handle)) {
			r_object = instance->object;
			return Outcome::Ok;
		}
		const auto minted = interpreter.minted_peers.find(p_handle);
		if (minted != interpreter.minted_peers.end()) {
			r_object = Value::from_cell(minted->second);
			return Outcome::Ok;
		}
	}
	const ClassCell *wrapper = p_fallback;
	if (wrapper == nullptr && p_handle != 0) {
		wrapper = class_of_handle(p_handle);
	}
	int64_t handle = p_handle;
	if (wrapper == nullptr) {
		wrapper = vh_object_class;
		handle = 0;
	}
	if (wrapper == nullptr) {
		return Outcome::Invalid;
	}
	return interpreter.construct(wrapper, handle, r_object);
}

GodotBridge::SignalBinding *GodotBridge::binding(int64_t p_id) {
	const auto found = bindings.find(p_id);
	return found == bindings.end() ? nullptr : &found->second;
}

int64_t GodotBridge::event_binding(const Cell *p_event) const {
	const auto found = event_bindings.find(p_event);
	return found == event_bindings.end() ? 0 : found->second;
}

void GodotBridge::bind_instance_signals(vh_instance *p_instance) {
	const SidecarClass *entry = p_instance->sidecar_class;
	if (entry == nullptr) {
		return;
	}
	for (const SidecarSignal &signal : entry->signals) {
		const Value member = field_value(p_instance->object, signal.name);
		if (!is_cell_kind(member, CellKind::Object)) {
			continue;
		}
		SignalBinding row;
		row.handle = p_instance->handle;
		row.name = signal.name;
		row.reject = signal.reject;
		row.reject_detail = signal.reject_detail;
		for (const auto &shape : entry->signal_types) {
			if (shape.first == signal.name) {
				row.shape = &shape.second;
				break;
			}
		}
		ObjectCell *object = cell_as<ObjectCell>(member);
		const int64_t id = next_id++;
		if (vh_signal_class != nullptr && class_inherits(object->object_class, vh_signal_class)) {
			Value *slot = field_slot(object, "Id");
			if (slot == nullptr) {
				continue;
			}
			*slot = make_int(heap, id);
		} else {
			row.event = member;
			event_bindings[object] = id;
		}
		bindings.emplace(id, row);
		instance_bindings[p_instance->handle].push_back(id);
	}
}

void GodotBridge::ensure_event_connections(vh_instance *p_instance) {
	if (p_instance->events_connected) {
		return;
	}
	p_instance->events_connected = true;
	const auto owned = instance_bindings.find(p_instance->handle);
	if (owned == instance_bindings.end()) {
		return;
	}
	for (int64_t id : owned->second) {
		SignalBinding *row = binding(id);
		if (row == nullptr || row->event.is_empty() || row->connected || row->reject != VH_SIGNAL_OK) {
			continue;
		}
		Callback callback;
		callback.kind = Callback::Kind::Event;
		callback.binding = id;
		int64_t callable_ref = 0;
		const int64_t callback_id = new_callback(callback, row->handle, callable_ref);
		int32_t status = VH_CALL_OK;
		if (callable_ref != 0) {
			connect(row->handle, row->name, callable_ref, 0, status);
		}
		if (callable_ref == 0 || status != VH_CALL_OK) {
			if (report_error) {
				report_error("The signal `" + row->name + "` was registered but could not be connected, so awaiting it would never resume.");
			}
			continue;
		}
		row->connected = true;
		row->callback = callback_id;
		row->callable_ref = callable_ref;
	}
}

void GodotBridge::release_instance_signals(vh_instance *p_instance) {
	const auto owned = instance_bindings.find(p_instance->handle);
	if (owned == instance_bindings.end()) {
		return;
	}
	const vh_godot_api &api = interpreter.godot;
	for (int64_t id : owned->second) {
		SignalBinding *row = binding(id);
		if (row == nullptr) {
			continue;
		}
		if (row->callable_ref != 0 && api.ReleaseRef != nullptr) {
			api.ReleaseRef(api.Ctx, row->callable_ref);
		}
		if (row->callback != 0) {
			callbacks.erase(row->callback);
		}
		if (!row->event.is_empty()) {
			event_bindings.erase(row->event.as_cell());
		}
		bindings.erase(id);
	}
	instance_bindings.erase(owned);
}

int64_t GodotBridge::bind_engine_signal(int64_t p_handle, const std::string &p_class, const std::string &p_accessor, const std::string &p_name) {
	const std::pair<int64_t, std::string> key(p_handle, p_name);
	const auto found = engine_bindings.find(key);
	if (found != engine_bindings.end()) {
		return found->second;
	}
	SignalBinding row;
	row.handle = p_handle;
	row.name = p_name;
	const auto shape = sidecar.engine_signal_keys.find(p_class + "." + p_accessor);
	if (shape != sidecar.engine_signal_keys.end() && shape->second < sidecar.engine_signal_shapes.size()) {
		row.shape = &sidecar.engine_signal_shapes[shape->second];
	}
	const int64_t id = next_id++;
	bindings.emplace(id, row);
	engine_bindings.emplace(key, id);
	return id;
}

int64_t GodotBridge::new_callback(Callback p_callback, int64_t p_owner, int64_t &r_callable_ref) {
	const int64_t id = next_id++;
	p_callback.owner = p_owner;
	callbacks.emplace(id, p_callback);
	const vh_godot_api &api = interpreter.godot;
	r_callable_ref = api.MakeCallable != nullptr ? api.MakeCallable(api.Ctx, id, p_owner) : 0;
	if (r_callable_ref == 0) {
		callbacks.erase(id);
	}
	return id;
}

bool GodotBridge::bound_instance_method(Value p_function, int64_t &r_owner) const {
	const Value function = follow(p_function);
	if (!is_cell_kind(function, CellKind::Function)) {
		return false;
	}
	const FunctionCell *cell = cell_as<FunctionCell>(function);
	const Value self = follow(cell->self);
	if (!is_cell_kind(self, CellKind::Object)) {
		return false;
	}
	for (const auto &entry : instances) {
		if (entry.second->object.same(self)) {
			r_owner = entry.first;
			return true;
		}
	}
	return false;
}

int64_t GodotBridge::make_method_callable(Value p_function, const SidecarPayloadShape *p_shape, bool p_foreign) {
	int64_t owner = 0;
	if (!bound_instance_method(p_function, owner)) {
		return 0;
	}
	Callback callback;
	callback.kind = Callback::Kind::Method;
	callback.function = follow(p_function);
	callback.shape = p_shape;
	callback.foreign = p_foreign;
	int64_t callable_ref = 0;
	new_callback(callback, owner, callable_ref);
	return callable_ref;
}

int64_t GodotBridge::connect(int64_t p_handle, const std::string &p_name, int64_t p_callable_ref, int32_t p_flags, int32_t &r_status) {
	const vh_godot_api &api = interpreter.godot;
	if (api.ConnectSignal == nullptr) {
		r_status = VH_CALL_NO_SUCH_MEMBER;
		return 0;
	}
	vh_value target = {};
	target.Type = VH_TYPE_REF;
	target.VariantTag = VH_VARIANT_CALLABLE;
	target.Ref = p_callable_ref;
	r_status = api.ConnectSignal(api.Ctx, p_handle, p_name.data(), int32_t(p_name.size()), &target, p_flags);
	return r_status == VH_CALL_OK ? 1 : 0;
}

int64_t GodotBridge::subscribe(int64_t p_handle, const std::string &p_name, int64_t p_callable_ref) {
	int32_t status = VH_CALL_OK;
	connect(p_handle, p_name, p_callable_ref, 0, status);
	const int64_t id = next_id++;
	Subscription row;
	row.handle = p_handle;
	row.name = p_name;
	row.callable_ref = p_callable_ref;
	subscriptions.emplace(id, row);
	interpreter.compensate([this, id] { cancel_subscription(id); });
	return id;
}

void GodotBridge::cancel_subscription(int64_t p_id) {
	const auto found = subscriptions.find(p_id);
	if (found == subscriptions.end()) {
		return;
	}
	const Subscription row = found->second;
	subscriptions.erase(found);
	const vh_godot_api &api = interpreter.godot;
	if (api.DisconnectSignal != nullptr) {
		vh_value target = {};
		target.Type = VH_TYPE_REF;
		target.VariantTag = VH_VARIANT_CALLABLE;
		target.Ref = row.callable_ref;
		api.DisconnectSignal(api.Ctx, row.handle, row.name.data(), int32_t(row.name.size()), &target);
	}
	if (api.ReleaseRef != nullptr) {
		api.ReleaseRef(api.Ctx, row.callable_ref);
	}
}

namespace {

// What ends a wait whose task was cancelled or terminated before its signal came (§9.2 step 3):
// a hook on the awaiting task, carried by a native-state object so it is data the collector sees.
struct AwaitHookState : NativeState {
	static constexpr uint32_t kTag = 4;
	GodotBridge *bridge = nullptr;
	int64_t token = 0;

	AwaitHookState() :
			NativeState(kTag) {}
	void visit_references(CellVisitor &) const override {}
};

void end_await_hook(TaskCell *, Cell *p_target) {
	ObjectCell *carrier = static_cast<ObjectCell *>(p_target);
	AwaitHookState *state = carrier->state<AwaitHookState>();
	if (state != nullptr && state->bridge != nullptr) {
		state->bridge->end_await(state->token);
	}
}

} // namespace

int64_t GodotBridge::begin_await(Value p_signal, int64_t p_handle, const std::string &p_name, int64_t p_binding, bool p_foreign) {
	Callback callback;
	callback.kind = Callback::Kind::Await;
	callback.foreign = p_foreign;
	const int64_t token = next_id++;
	callback.token = token;
	int64_t callable_ref = 0;
	const int64_t callback_id = new_callback(callback, p_handle, callable_ref);
	if (callable_ref == 0) {
		return 0;
	}
	int32_t status = VH_CALL_OK;
	connect(p_handle, p_name, callable_ref, VH_CONNECT_ONE_SHOT, status);
	if (status != VH_CALL_OK) {
		callbacks.erase(callback_id);
		const vh_godot_api &api = interpreter.godot;
		if (api.ReleaseRef != nullptr) {
			api.ReleaseRef(api.Ctx, callable_ref);
		}
		return 0;
	}
	AwaitRow row;
	row.signal = follow(p_signal);
	row.handle = p_handle;
	row.name = p_name;
	row.binding = p_binding;
	row.callback = callback_id;
	row.callable_ref = callable_ref;
	awaits.emplace(token, row);
	if (TaskCell *waiter = interpreter.current_task()) {
		ObjectCell *carrier = heap.make<ObjectCell>();
		AwaitHookState *state = carrier->state<AwaitHookState>();
		state->bridge = this;
		state->token = token;
		waiter->defer_hooks.push_back(TaskHook{ &end_await_hook, carrier });
	}
	return token;
}

void GodotBridge::end_await(int64_t p_token) {
	const auto found = awaits.find(p_token);
	if (found == awaits.end()) {
		return;
	}
	const AwaitRow row = found->second;
	awaits.erase(found);
	const vh_godot_api &api = interpreter.godot;
	if (api.DisconnectSignal != nullptr) {
		vh_value target = {};
		target.Type = VH_TYPE_REF;
		target.VariantTag = VH_VARIANT_CALLABLE;
		target.Ref = row.callable_ref;
		api.DisconnectSignal(api.Ctx, row.handle, row.name.data(), int32_t(row.name.size()), &target);
	}
	if (api.ReleaseRef != nullptr) {
		api.ReleaseRef(api.Ctx, row.callable_ref);
	}
}

bool GodotBridge::payload_to_wire(Value p_payload, const SidecarPayloadShape &p_shape, vh_arena *p_arena, std::vector<vh_value> &r_args) {
	r_args.clear();
	std::string why;
	const Value payload = follow(p_payload);
	switch (p_shape.kind) {
		case 0: {
			vh_value wire = {};
			const SidecarMemberType &type = p_shape.args.empty() ? p_shape.whole : p_shape.args[0].type;
			if (!member_to_wire(payload, type, p_arena, wire, why)) {
				return false;
			}
			r_args.push_back(wire);
			return true;
		}
		case 1: {
			if (p_shape.args.empty()) {
				return true;
			}
			if (!is_cell_kind(payload, CellKind::Array) && !is_cell_kind(payload, CellKind::MutableArray)) {
				return false;
			}
			const ArrayCell *tuple = cell_as<ArrayCell>(payload);
			if (tuple->length() != p_shape.args.size()) {
				return false;
			}
			for (size_t index = 0; index < p_shape.args.size(); ++index) {
				vh_value wire = {};
				if (!member_to_wire(tuple->get(index), p_shape.args[index].type, p_arena, wire, why)) {
					return false;
				}
				r_args.push_back(wire);
			}
			return true;
		}
		default: {
			for (const SidecarPayloadArg &arg : p_shape.args) {
				Value field;
				if (is_cell_kind(payload, CellKind::Object)) {
					ObjectCell *object = cell_as<ObjectCell>(payload);
					if (object->layout == nullptr) {
						interpreter.layouts.lay_out_value_object(object);
					}
					if (object->layout != nullptr) {
						for (const LayoutField &entry : object->layout->fields) {
							if (entry.kind == FieldKind::Slot && entry.name != nullptr && entry.name->text == arg.key) {
								field = follow(read_slot(object->field_values[entry.slot]));
								break;
							}
						}
					}
				}
				if (field.is_empty()) {
					field = field_value(payload, arg.name);
				}
				if (field.is_empty()) {
					return false;
				}
				vh_value wire = {};
				if (!member_to_wire(field, arg.type, p_arena, wire, why)) {
					return false;
				}
				r_args.push_back(wire);
			}
			return true;
		}
	}
}

bool GodotBridge::payload_from_wire(const SidecarPayloadShape &p_shape, const vh_value *p_args, int32_t p_count, Value &r_payload) {
	std::string why;
	switch (p_shape.kind) {
		case 0: {
			if (p_count != 1) {
				return false;
			}
			const SidecarMemberType &type = p_shape.args.empty() ? p_shape.whole : p_shape.args[0].type;
			return wire_to_member(p_args[0], type, r_payload, why);
		}
		case 1: {
			if (size_t(p_count) != p_shape.args.size()) {
				return false;
			}
			std::vector<Value> elements;
			for (int32_t index = 0; index < p_count; ++index) {
				Value element;
				if (!wire_to_member(p_args[index], p_shape.args[size_t(index)].type, element, why)) {
					return false;
				}
				elements.push_back(element);
			}
			r_payload = make_array(heap, elements, false);
			return true;
		}
		default: {
			if (p_shape.whole.has_user_struct) {
				return user_struct_from_fields(p_shape.whole, p_args, p_count, r_payload, why);
			}
			if (p_shape.whole.described.tag == VH_VARIANT_RID && p_count == 1) {
				return wire_to_member(p_args[0], p_shape.whole, r_payload, why);
			}
			const verse_math::layout *layout = math_layout(p_shape.whole.described.tag);
			if (layout != nullptr) {
				std::vector<double> leaves;
				for (int32_t index = 0; index < p_count; ++index) {
					if (!leaves_of_wire(p_args[index], leaves)) {
						leaves.push_back(wire_double(p_args[index]));
					}
				}
				size_t next = 0;
				r_payload = struct_from_leaves(layout->variant_tag, leaves, next);
				return !r_payload.is_empty();
			}
			return false;
		}
	}
}

bool GodotBridge::array_from_wire(const vh_value *p_args, int32_t p_count, Value &r_array) {
	const vh_godot_api &api = interpreter.godot;
	if (api.NewRef == nullptr) {
		return false;
	}
	const int64_t ref = api.NewRef(api.Ctx, VH_VARIANT_ARRAY);
	if (ref == 0) {
		return false;
	}
	for (int32_t index = 0; index < p_count; ++index) {
		vh_value key = {};
		key.Type = VH_TYPE_INT;
		key.VariantTag = VH_VARIANT_INT;
		key.Int = index;
		if (api.RefSet != nullptr) {
			api.RefSet(api.Ctx, ref, &key, &p_args[index]);
		}
	}
	r_array = wrap_reference(VH_VARIANT_ARRAY, ref);
	return !r_array.is_empty();
}

bool GodotBridge::callback_owner(int64_t p_id, vh_instance *&r_instance) const {
	const auto found = callbacks.find(p_id);
	if (found == callbacks.end()) {
		r_instance = nullptr;
		return false;
	}
	r_instance = instance_for(found->second.owner);
	return true;
}

void GodotBridge::resolve_callback(int64_t p_id, const vh_value *p_args, int32_t p_count, CallbackTarget &r_target) {
	r_target = CallbackTarget();
	const auto found = callbacks.find(p_id);
	if (found == callbacks.end()) {
		r_target.delivery = Delivery::NotFound;
		return;
	}
	const Callback callback = found->second;
	r_target.instance = instance_for(callback.owner);
	std::string why;
	switch (callback.kind) {
		case Callback::Kind::Method: {
			if (r_target.instance == nullptr) {
				r_target.delivery = Delivery::NotFound;
				return;
			}
			r_target.function = callback.function;
			if (callback.foreign) {
				Value array;
				if (!array_from_wire(p_args, p_count, array)) {
					r_target.delivery = Delivery::BadArguments;
					return;
				}
				r_target.arguments.push_back(array);
			} else if (callback.shape != nullptr) {
				Value payload;
				if (!payload_from_wire(*callback.shape, p_args, p_count, payload)) {
					r_target.delivery = Delivery::BadArguments;
					return;
				}
				r_target.arguments.push_back(payload);
			} else {
				const FunctionCell *function = cell_as<FunctionCell>(callback.function);
				const ProcedureCell *procedure = function->callee != nullptr && function->callee->kind == CellKind::Procedure
						? static_cast<const ProcedureCell *>(function->callee)
						: nullptr;
				r_target.types = procedure != nullptr && procedure->name != nullptr ? method_types(procedure->name->text) : nullptr;
				if (!wire_to_arguments(r_target.types, p_args, p_count, true, r_target.arguments, why)) {
					r_target.delivery = Delivery::BadArguments;
					return;
				}
			}
			r_target.delivery = Delivery::Call;
			return;
		}
		case Callback::Kind::Await: {
			const auto row = awaits.find(callback.token);
			if (row == awaits.end()) {
				r_target.delivery = Delivery::Noop;
				return;
			}
			Value payload;
			const SignalBinding *bound = row->second.binding != 0 ? binding(row->second.binding) : nullptr;
			bool built = false;
			if (bound != nullptr && bound->shape != nullptr) {
				built = payload_from_wire(*bound->shape, p_args, p_count, payload);
			} else {
				built = array_from_wire(p_args, p_count, payload);
			}
			if (!built) {
				r_target.delivery = Delivery::BadArguments;
				return;
			}
			r_target.event = field_value(row->second.signal, "Ev");
			r_target.payload = payload;
			r_target.delivery = r_target.event.is_empty() ? Delivery::Noop : Delivery::Signal;
			return;
		}
		case Callback::Kind::Event: {
			const SignalBinding *bound = binding(callback.binding);
			if (bound == nullptr || bound->event.is_empty()) {
				r_target.delivery = Delivery::Noop;
				return;
			}
			Value payload;
			if (bound->shape == nullptr || !payload_from_wire(*bound->shape, p_args, p_count, payload)) {
				r_target.delivery = Delivery::BadArguments;
				return;
			}
			r_target.event = bound->event;
			r_target.payload = payload;
			r_target.delivery = Delivery::Signal;
			return;
		}
	}
}

void GodotBridge::release_callback(int64_t p_id) {
	callbacks.erase(p_id);
}

namespace {

// `[]variant` into wire values, each through §6.1.
bool variant_arguments(GodotBridge &r_bridge, Value p_array, OwnedWire &r_wire) {
	const Value array = follow(p_array);
	if (is_cell_kind(array, CellKind::False)) {
		return true;
	}
	if (!is_cell_kind(array, CellKind::Array) && !is_cell_kind(array, CellKind::MutableArray)) {
		return false;
	}
	const ArrayCell *cell = cell_as<ArrayCell>(array);
	r_wire.values.assign(cell->length(), vh_value{});
	for (size_t index = 0; index < cell->length(); ++index) {
		VariantLanes lanes;
		if (!r_bridge.read_variant(cell->get(index), lanes) || !r_bridge.variant_to_wire(lanes, &r_wire.arena, r_wire.values[index])) {
			return false;
		}
	}
	return true;
}

Value variant_result(GodotBridge &r_bridge, const vh_value &p_wire) {
	VariantLanes lanes;
	r_bridge.wire_to_lanes(p_wire, lanes);
	return r_bridge.make_variant(lanes);
}

Outcome is_valid_native(NativeCall &r_call) {
	if (unbound_argument(r_call)) {
		return Outcome::Park;
	}
	GodotBridge *bridge = bridge_of(r_call);
	int64_t handle = 0;
	if (bridge == nullptr || !int_argument(r_call, 0, handle)) {
		return Outcome::Invalid;
	}
	const vh_godot_api &api = bridge->interpreter.godot;
	r_call.result = r_call.heap.logic(api.IsValid != nullptr && api.IsValid(api.Ctx, handle) != 0);
	return Outcome::Ok;
}

Outcome type_mismatch_native(NativeCall &r_call) {
	if (unbound_argument(r_call)) {
		return Outcome::Park;
	}
	GodotBridge *bridge = bridge_of(r_call);
	std::string expected;
	VariantLanes lanes;
	if (bridge == nullptr || !string_bytes(argument(r_call, 0), expected) || !bridge->read_variant(argument(r_call, 1), lanes)) {
		return Outcome::Invalid;
	}
	return raise(r_call, "Godot returned a value tagged " + std::to_string(lanes.tag) + " where the Verse bridge expected `" + expected +
					"`. The type table in tools/gen_verse_api.py and this build of Godot disagree.");
}

// §8.4 and §8.7: the same call behind both specifiers.
Outcome call_value_native(NativeCall &r_call) {
	if (unbound_argument(r_call)) {
		return Outcome::Park;
	}
	GodotBridge *bridge = bridge_of(r_call);
	int64_t handle = 0;
	std::string method;
	OwnedWire args;
	if (bridge == nullptr || !int_argument(r_call, 0, handle) || !string_bytes(argument(r_call, 1), method) ||
			!variant_arguments(*bridge, argument(r_call, 2), args)) {
		return Outcome::Invalid;
	}
	const vh_godot_api &api = bridge->interpreter.godot;
	if (api.CallMethod == nullptr) {
		return raise(r_call, "Called `" + method + "` on Godot object " + std::to_string(handle) + ", but this embedder cannot call Godot methods.");
	}
	ScopedArena arena;
	vh_value result = {};
	const int32_t status = api.CallMethod(api.Ctx, handle, method.data(), int32_t(method.size()), args.values.data(), int32_t(args.values.size()),
			arena.get(), &result);
	if (status != VH_CALL_OK) {
		return raise(r_call, call_sentence("Called", method, handle, status));
	}
	r_call.result = variant_result(*bridge, result);
	return Outcome::Ok;
}

Outcome get_value_native(NativeCall &r_call) {
	if (unbound_argument(r_call)) {
		return Outcome::Park;
	}
	GodotBridge *bridge = bridge_of(r_call);
	int64_t handle = 0;
	std::string property;
	if (bridge == nullptr || !int_argument(r_call, 0, handle) || !string_bytes(argument(r_call, 1), property)) {
		return Outcome::Invalid;
	}
	const vh_godot_api &api = bridge->interpreter.godot;
	if (api.GetProperty == nullptr) {
		return Outcome::Fail;
	}
	ScopedArena arena;
	vh_value result = {};
	const int32_t status = api.GetProperty(api.Ctx, handle, property.data(), int32_t(property.size()), arena.get(), &result);
	if (status == VH_CALL_NO_SUCH_MEMBER) {
		return Outcome::Fail;
	}
	if (status != VH_CALL_OK) {
		return raise(r_call, call_sentence("Read", property, handle, status));
	}
	r_call.result = variant_result(*bridge, result);
	return Outcome::Ok;
}

// §8.8 and §8.9: the liveness test now, the write at the root commit.
Outcome call_void_native(NativeCall &r_call) {
	if (unbound_argument(r_call)) {
		return Outcome::Park;
	}
	GodotBridge *bridge = bridge_of(r_call);
	int64_t handle = 0;
	std::string method;
	std::shared_ptr<OwnedWire> args = std::make_shared<OwnedWire>();
	if (bridge == nullptr || !int_argument(r_call, 0, handle) || !string_bytes(argument(r_call, 1), method) ||
			!variant_arguments(*bridge, argument(r_call, 2), *args)) {
		return Outcome::Invalid;
	}
	const vh_godot_api api = bridge->interpreter.godot;
	if (api.IsValid != nullptr && api.IsValid(api.Ctx, handle) == 0) {
		return raise(r_call, call_sentence("Called", method, handle, VH_CALL_DEAD_OBJECT));
	}
	r_call.result = r_call.heap.false_value();
	if (api.CallMethod == nullptr) {
		return Outcome::Ok;
	}
	bridge->interpreter.defer([api, handle, method, args] {
		HostArena arena;
		vh_value ignored = {};
		api.CallMethod(api.Ctx, handle, method.data(), int32_t(method.size()), args->values.data(), int32_t(args->values.size()), &arena, &ignored);
	});
	return Outcome::Ok;
}

Outcome set_value_native(NativeCall &r_call) {
	if (unbound_argument(r_call)) {
		return Outcome::Park;
	}
	GodotBridge *bridge = bridge_of(r_call);
	int64_t handle = 0;
	std::string property;
	VariantLanes lanes;
	std::shared_ptr<OwnedWire> value = std::make_shared<OwnedWire>();
	value->values.assign(1, vh_value{});
	if (bridge == nullptr || !int_argument(r_call, 0, handle) || !string_bytes(argument(r_call, 1), property) ||
			!bridge->read_variant(argument(r_call, 2), lanes) || !bridge->variant_to_wire(lanes, &value->arena, value->values[0])) {
		return Outcome::Invalid;
	}
	const vh_godot_api api = bridge->interpreter.godot;
	if (api.IsValid != nullptr && api.IsValid(api.Ctx, handle) == 0) {
		return raise(r_call, call_sentence("Wrote", property, handle, VH_CALL_DEAD_OBJECT));
	}
	r_call.result = r_call.heap.false_value();
	if (api.SetProperty == nullptr) {
		return Outcome::Ok;
	}
	bridge->interpreter.defer([api, handle, property, value] {
		api.SetProperty(api.Ctx, handle, property.data(), int32_t(property.size()), &value->values[0]);
	});
	return Outcome::Ok;
}

// §8.6.
Outcome variant_from_any_native(NativeCall &r_call) {
	if (unbound_argument(r_call)) {
		return Outcome::Park;
	}
	GodotBridge *bridge = bridge_of(r_call);
	if (bridge == nullptr) {
		return Outcome::Invalid;
	}
	const Value value = follow(argument(r_call, 0));
	VariantLanes lanes;
	RuntimeError error;
	if (is_cell_kind(value, CellKind::Object) && bridge->vh_object_class != nullptr &&
			class_inherits(cell_as<ObjectCell>(value)->object_class, bridge->vh_object_class)) {
		lanes.tag = VH_VARIANT_OBJECT;
		lanes.ref = bridge->handle_of(value);
	} else if (is_int(value)) {
		lanes.tag = VH_VARIANT_INT;
		if (int_to_int64(value, lanes.i[0], error) != Outcome::Ok) {
			return Outcome::Fail;
		}
	} else if (value.is_float()) {
		lanes.tag = VH_VARIANT_FLOAT;
		lanes.f[0] = value.as_float();
	} else if ((is_cell_kind(value, CellKind::Array) || is_cell_kind(value, CellKind::MutableArray)) &&
			cell_as<ArrayCell>(value)->storage == ArrayCell::Storage::Char8) {
		lanes.tag = VH_VARIANT_STRING;
		string_bytes(value, lanes.text);
	} else if ((is_cell_kind(value, CellKind::Array) || is_cell_kind(value, CellKind::MutableArray)) && cell_as<ArrayCell>(value)->length() != 0 &&
			string_bytes(value, lanes.text)) {
		lanes.tag = VH_VARIANT_STRING;
	} else if (is_cell_kind(value, CellKind::Object) && bridge->rid_class != nullptr && cell_as<ObjectCell>(value)->object_class == bridge->rid_class) {
		lanes.tag = VH_VARIANT_RID;
		int_to_int64(bridge->field_value(value, "Id"), lanes.i[0], error);
	} else if (is_cell_kind(value, CellKind::Object)) {
		ScopedArena arena;
		vh_value wire = {};
		std::string why;
		const ObjectCell *object = cell_as<ObjectCell>(value);
		bool math = false;
		for (int32_t tag = 1; tag < VH_VARIANT_MAX; ++tag) {
			if (bridge->math_classes[tag] != nullptr && bridge->math_classes[tag] == object->object_class) {
				math = true;
			}
		}
		if (!math) {
			return Outcome::Fail;
		}
		SidecarMemberType type;
		if (!bridge->member_to_wire(value, type, arena.get(), wire, why)) {
			return Outcome::Fail;
		}
		bridge->wire_to_lanes(wire, lanes);
	} else if (is_cell_kind(value, CellKind::True) || is_cell_kind(value, CellKind::False)) {
		lanes.tag = VH_VARIANT_BOOL;
		lanes.i[0] = is_cell_kind(value, CellKind::True) ? 1 : 0;
	} else {
		return Outcome::Fail;
	}
	r_call.result = bridge->make_variant(lanes);
	return r_call.result.is_empty() ? Outcome::Invalid : Outcome::Ok;
}

Outcome singleton_object_native(NativeCall &r_call) {
	if (unbound_argument(r_call)) {
		return Outcome::Park;
	}
	GodotBridge *bridge = bridge_of(r_call);
	std::string name;
	if (bridge == nullptr || !string_bytes(argument(r_call, 0), name)) {
		return Outcome::Invalid;
	}
	const vh_godot_api &api = bridge->interpreter.godot;
	const int64_t handle = api.GetSingleton != nullptr ? api.GetSingleton(api.Ctx, name.data(), int32_t(name.size())) : 0;
	const ClassCell *fallback = handle != 0 ? bridge->mirrored_class(name) : nullptr;
	return bridge->object_for_handle(handle, handle != 0 && fallback == nullptr ? bridge->vh_object_class : fallback, r_call.result);
}

Outcome object_of_native(NativeCall &r_call) {
	if (unbound_argument(r_call)) {
		return Outcome::Park;
	}
	GodotBridge *bridge = bridge_of(r_call);
	int64_t handle = 0;
	if (bridge == nullptr || !int_argument(r_call, 0, handle)) {
		return Outcome::Invalid;
	}
	return bridge->object_for_handle(handle, nullptr, r_call.result);
}

Outcome callable_from_native(NativeCall &r_call) {
	if (unbound_argument(r_call)) {
		return Outcome::Park;
	}
	GodotBridge *bridge = bridge_of(r_call);
	if (bridge == nullptr) {
		return Outcome::Invalid;
	}
	r_call.result = make_int(r_call.heap, bridge->make_method_callable(argument(r_call, 0), nullptr, false));
	return Outcome::Ok;
}

// Emission on a binding row, shared by §8.14 and §8.17.
Outcome emit_binding(NativeCall &r_call, GodotBridge &r_bridge, const GodotBridge::SignalBinding &p_row, Value p_payload) {
	if (p_row.reject != VH_SIGNAL_OK) {
		return raise(r_call, "The signal `" + p_row.name + "` was never registered with Godot: " + reject_reason(p_row.reject, p_row.reject_detail) +
						". Nothing was emitted.");
	}
	ScopedArena arena;
	std::vector<vh_value> args;
	if (p_row.shape == nullptr || !r_bridge.payload_to_wire(p_payload, *p_row.shape, arena.get(), args)) {
		return raise(r_call, "The payload of signal `" + p_row.name + "` has no representation on the Godot wire, so nothing was emitted.");
	}
	const vh_godot_api &api = r_bridge.interpreter.godot;
	if (api.EmitSignal != nullptr) {
		api.EmitSignal(api.Ctx, p_row.handle, p_row.name.data(), int32_t(p_row.name.size()), args.data(), int32_t(args.size()));
	}
	r_call.result = r_call.heap.false_value();
	return Outcome::Ok;
}

Outcome subscribe_binding(NativeCall &r_call, GodotBridge &r_bridge, const GodotBridge::SignalBinding &p_row, Value p_callback) {
	if (p_row.reject != VH_SIGNAL_OK) {
		return raise(r_call, "Cannot subscribe to `" + p_row.name + "`: " + reject_reason(p_row.reject, p_row.reject_detail));
	}
	const int64_t callable_ref = r_bridge.make_method_callable(p_callback, p_row.shape, false);
	if (callable_ref == 0) {
		r_call.result = Value::from_int32(0);
		return Outcome::Ok;
	}
	r_call.result = make_int(r_call.heap, r_bridge.subscribe(p_row.handle, p_row.name, callable_ref));
	return Outcome::Ok;
}

Outcome signal_emit_native(NativeCall &r_call) {
	if (unbound_argument(r_call)) {
		return Outcome::Park;
	}
	GodotBridge *bridge = bridge_of(r_call);
	int64_t id = 0;
	if (bridge == nullptr || !int_argument(r_call, 0, id)) {
		return Outcome::Invalid;
	}
	const GodotBridge::SignalBinding *row = id != 0 ? bridge->binding(id) : nullptr;
	if (row == nullptr) {
		return raise(r_call, "A signal was emitted through an unbound `signal`. One a script built for itself rather than declared as a member of a class Godot instantiated names nothing, the way `godot_array{}` does.");
	}
	const GodotBridge::SignalBinding copy = *row;
	return emit_binding(r_call, *bridge, copy, argument(r_call, 1));
}

Outcome signal_subscribe_native(NativeCall &r_call) {
	if (unbound_argument(r_call)) {
		return Outcome::Park;
	}
	GodotBridge *bridge = bridge_of(r_call);
	int64_t id = 0;
	if (bridge == nullptr || !int_argument(r_call, 0, id)) {
		return Outcome::Invalid;
	}
	const GodotBridge::SignalBinding *row = id != 0 ? bridge->binding(id) : nullptr;
	if (row == nullptr) {
		return raise(r_call, "Subscribe was called on an unbound `signal`, which names nothing.");
	}
	const GodotBridge::SignalBinding copy = *row;
	return subscribe_binding(r_call, *bridge, copy, argument(r_call, 1));
}

Outcome signal_cancel_native(NativeCall &r_call) {
	if (unbound_argument(r_call)) {
		return Outcome::Park;
	}
	GodotBridge *bridge = bridge_of(r_call);
	int64_t id = 0;
	if (bridge == nullptr || !int_argument(r_call, 0, id)) {
		return Outcome::Invalid;
	}
	bridge->cancel_subscription(id);
	r_call.result = r_call.heap.false_value();
	return Outcome::Ok;
}

Outcome event_emit_native(NativeCall &r_call) {
	if (unbound_argument(r_call)) {
		return Outcome::Park;
	}
	GodotBridge *bridge = bridge_of(r_call);
	if (bridge == nullptr) {
		return Outcome::Invalid;
	}
	const Value event = follow(argument(r_call, 0));
	const int64_t id = event.is_cell() ? bridge->event_binding(event.as_cell()) : 0;
	const GodotBridge::SignalBinding *row = id != 0 ? bridge->binding(id) : nullptr;
	if (row == nullptr) {
		return raise(r_call, "Emit was called on an `event` that is not an `@export_signal` member of a class Godot instantiated, so it names no Godot signal. An event a script builds for itself is a Verse event and nothing more -- `Signal` is how tasks are resumed through one.");
	}
	const GodotBridge::SignalBinding copy = *row;
	return emit_binding(r_call, *bridge, copy, argument(r_call, 1));
}

Outcome event_subscribe_native(NativeCall &r_call) {
	if (unbound_argument(r_call)) {
		return Outcome::Park;
	}
	GodotBridge *bridge = bridge_of(r_call);
	if (bridge == nullptr) {
		return Outcome::Invalid;
	}
	const Value event = follow(argument(r_call, 0));
	const int64_t id = event.is_cell() ? bridge->event_binding(event.as_cell()) : 0;
	const GodotBridge::SignalBinding *row = id != 0 ? bridge->binding(id) : nullptr;
	if (row == nullptr) {
		return raise(r_call, "Subscribe was called on an `event` that is not an `@export_signal` member of a class Godot instantiated, so it names no Godot signal.");
	}
	const GodotBridge::SignalBinding copy = *row;
	return subscribe_binding(r_call, *bridge, copy, argument(r_call, 1));
}

Outcome signal_bind_native(NativeCall &r_call) {
	if (unbound_argument(r_call)) {
		return Outcome::Park;
	}
	GodotBridge *bridge = bridge_of(r_call);
	int64_t handle = 0;
	std::string class_name;
	std::string accessor;
	std::string name;
	if (bridge == nullptr || !int_argument(r_call, 0, handle) || !string_bytes(argument(r_call, 1), class_name) ||
			!string_bytes(argument(r_call, 2), accessor) || !string_bytes(argument(r_call, 3), name)) {
		return Outcome::Invalid;
	}
	r_call.result = make_int(r_call.heap, bridge->bind_engine_signal(handle, class_name, accessor, name));
	return Outcome::Ok;
}

Outcome signal_await_native(NativeCall &r_call) {
	if (unbound_argument(r_call)) {
		return Outcome::Park;
	}
	GodotBridge *bridge = bridge_of(r_call);
	if (bridge == nullptr) {
		return Outcome::Invalid;
	}
	const Value signal = follow(argument(r_call, 0));
	int64_t id = 0;
	RuntimeError error;
	int_to_int64(bridge->field_value(signal, "Id"), id, error);
	const GodotBridge::SignalBinding *row = id != 0 ? bridge->binding(id) : nullptr;
	if (row == nullptr) {
		return raise(r_call, "Await was called on an unbound `signal`, which names nothing and so will never be emitted.");
	}
	if (row->reject != VH_SIGNAL_OK) {
		return raise(r_call, "Cannot await `" + row->name + "`: " + reject_reason(row->reject, row->reject_detail));
	}
	const int64_t handle = row->handle;
	const std::string name = row->name;
	r_call.result = make_int(r_call.heap, bridge->begin_await(signal, handle, name, id, false));
	return Outcome::Ok;
}

Outcome signal_await_end_native(NativeCall &r_call) {
	if (unbound_argument(r_call)) {
		return Outcome::Park;
	}
	GodotBridge *bridge = bridge_of(r_call);
	int64_t token = 0;
	if (bridge == nullptr || !int_argument(r_call, 0, token)) {
		return Outcome::Invalid;
	}
	bridge->end_await(token);
	r_call.result = r_call.heap.false_value();
	return Outcome::Ok;
}

bool signal_target(GodotBridge &r_bridge, int64_t p_ref, int64_t &r_handle, std::string &r_name) {
	const vh_godot_api &api = r_bridge.interpreter.godot;
	if (api.SignalTarget == nullptr) {
		return false;
	}
	vh_handle handle = 0;
	const char *name = nullptr;
	if (api.SignalTarget(api.Ctx, p_ref, &handle, &name) != VH_CALL_OK || name == nullptr) {
		return false;
	}
	r_handle = handle;
	r_name = name;
	return true;
}

Outcome signal_ref_await_native(NativeCall &r_call) {
	if (unbound_argument(r_call)) {
		return Outcome::Park;
	}
	GodotBridge *bridge = bridge_of(r_call);
	int64_t ref = 0;
	if (bridge == nullptr || !int_argument(r_call, 0, ref)) {
		return Outcome::Invalid;
	}
	int64_t handle = 0;
	std::string name;
	if (!signal_target(*bridge, ref, handle, name)) {
		return raise(r_call, "Await was called on a Signal value that names no object and signal.");
	}
	r_call.result = make_int(r_call.heap, bridge->begin_await(argument(r_call, 1), handle, name, 0, true));
	return Outcome::Ok;
}

Outcome signal_ref_subscribe_native(NativeCall &r_call) {
	if (unbound_argument(r_call)) {
		return Outcome::Park;
	}
	GodotBridge *bridge = bridge_of(r_call);
	int64_t ref = 0;
	if (bridge == nullptr || !int_argument(r_call, 0, ref)) {
		return Outcome::Invalid;
	}
	int64_t handle = 0;
	std::string name;
	if (!signal_target(*bridge, ref, handle, name)) {
		return raise(r_call, "Subscribe was called on a Signal value that names no object and signal.");
	}
	const int64_t callable_ref = bridge->make_method_callable(argument(r_call, 1), nullptr, true);
	if (callable_ref == 0) {
		return raise(r_call, "Subscribe was given a Verse function that is not a method bound to a live script instance, which is the only shape a Godot Callable can carry without outliving what it names.");
	}
	r_call.result = make_int(r_call.heap, bridge->subscribe(handle, name, callable_ref));
	return Outcome::Ok;
}

Outcome signal_ref_for_native(NativeCall &r_call) {
	if (unbound_argument(r_call)) {
		return Outcome::Park;
	}
	GodotBridge *bridge = bridge_of(r_call);
	int64_t handle = 0;
	std::string name;
	if (bridge == nullptr || !int_argument(r_call, 0, handle) || !string_bytes(argument(r_call, 1), name)) {
		return Outcome::Invalid;
	}
	const vh_godot_api &api = bridge->interpreter.godot;
	const int64_t ref = api.MakeSignalRef != nullptr ? api.MakeSignalRef(api.Ctx, handle, name.data(), int32_t(name.size())) : 0;
	r_call.result = make_int(r_call.heap, ref);
	return Outcome::Ok;
}

// §8.26 to §8.28: a missing callback answers Nil rather than raising.
Outcome call_static_native(NativeCall &r_call) {
	if (unbound_argument(r_call)) {
		return Outcome::Park;
	}
	GodotBridge *bridge = bridge_of(r_call);
	std::string class_name;
	std::string method;
	OwnedWire args;
	if (bridge == nullptr || !string_bytes(argument(r_call, 0), class_name) || !string_bytes(argument(r_call, 1), method) ||
			!variant_arguments(*bridge, argument(r_call, 2), args)) {
		return Outcome::Invalid;
	}
	const vh_godot_api &api = bridge->interpreter.godot;
	ScopedArena arena;
	vh_value result = {};
	if (api.CallStatic != nullptr) {
		const int32_t status = api.CallStatic(api.Ctx, class_name.data(), int32_t(class_name.size()), method.data(), int32_t(method.size()),
				args.values.data(), int32_t(args.values.size()), arena.get(), &result);
		if (status != VH_CALL_OK) {
			return raise(r_call, call_sentence("Called static", method, 0, status));
		}
	}
	r_call.result = variant_result(*bridge, result);
	return Outcome::Ok;
}

Outcome call_utility_native(NativeCall &r_call) {
	if (unbound_argument(r_call)) {
		return Outcome::Park;
	}
	GodotBridge *bridge = bridge_of(r_call);
	std::string name;
	OwnedWire args;
	if (bridge == nullptr || !string_bytes(argument(r_call, 0), name) || !variant_arguments(*bridge, argument(r_call, 1), args)) {
		return Outcome::Invalid;
	}
	const vh_godot_api &api = bridge->interpreter.godot;
	ScopedArena arena;
	vh_value result = {};
	if (api.CallUtility != nullptr) {
		const int32_t status = api.CallUtility(api.Ctx, name.data(), int32_t(name.size()), args.values.data(), int32_t(args.values.size()), arena.get(), &result);
		if (status != VH_CALL_OK) {
			return raise(r_call, call_sentence("Called", name, 0, status));
		}
	}
	r_call.result = variant_result(*bridge, result);
	return Outcome::Ok;
}

// §8.29: the godot_ref now owns its id, released by the sweep that finds it unreachable.
Outcome adopt_ref_native(NativeCall &r_call) {
	if (unbound_argument(r_call)) {
		return Outcome::Park;
	}
	GodotBridge *bridge = bridge_of(r_call);
	const Value object = follow(argument(r_call, 0));
	if (bridge == nullptr || !is_cell_kind(object, CellKind::Object)) {
		return Outcome::Invalid;
	}
	int64_t ref = 0;
	RuntimeError error;
	int_to_int64(bridge->field_value(object, "Ref"), ref, error);
	bridge->interpreter.adopt_ref(object.as_cell(), ref);
	r_call.result = r_call.heap.false_value();
	return Outcome::Ok;
}

Outcome ref_get_native(NativeCall &r_call) {
	if (unbound_argument(r_call)) {
		return Outcome::Park;
	}
	GodotBridge *bridge = bridge_of(r_call);
	int64_t ref = 0;
	VariantLanes key_lanes;
	ScopedArena arena;
	vh_value key = {};
	if (bridge == nullptr || !int_argument(r_call, 0, ref) || !bridge->read_variant(argument(r_call, 1), key_lanes) ||
			!bridge->variant_to_wire(key_lanes, arena.get(), key)) {
		return Outcome::Invalid;
	}
	if (ref == 0) {
		return raise(r_call, reference_sentence("Read", ref));
	}
	const vh_godot_api &api = bridge->interpreter.godot;
	if (api.RefGet == nullptr) {
		return Outcome::Fail;
	}
	vh_value result = {};
	const int32_t status = api.RefGet(api.Ctx, ref, &key, arena.get(), &result);
	if (status == VH_CALL_NO_SUCH_MEMBER) {
		return Outcome::Fail;
	}
	if (status != VH_CALL_OK) {
		return raise(r_call, reference_sentence("Read", ref));
	}
	r_call.result = variant_result(*bridge, result);
	return Outcome::Ok;
}

Outcome ref_set_native(NativeCall &r_call) {
	if (unbound_argument(r_call)) {
		return Outcome::Park;
	}
	GodotBridge *bridge = bridge_of(r_call);
	int64_t ref = 0;
	VariantLanes key_lanes;
	VariantLanes value_lanes;
	ScopedArena arena;
	vh_value key = {};
	vh_value value = {};
	if (bridge == nullptr || !int_argument(r_call, 0, ref) || !bridge->read_variant(argument(r_call, 1), key_lanes) ||
			!bridge->read_variant(argument(r_call, 2), value_lanes) || !bridge->variant_to_wire(key_lanes, arena.get(), key) ||
			!bridge->variant_to_wire(value_lanes, arena.get(), value)) {
		return Outcome::Invalid;
	}
	if (ref == 0) {
		return raise(r_call, reference_sentence("Wrote", ref));
	}
	const vh_godot_api &api = bridge->interpreter.godot;
	r_call.result = r_call.heap.false_value();
	if (api.RefSet == nullptr) {
		return Outcome::Ok;
	}
	const int32_t status = api.RefSet(api.Ctx, ref, &key, &value);
	if (status != VH_CALL_OK && status != VH_CALL_NO_SUCH_MEMBER && status != VH_CALL_BAD_VALUE) {
		return raise(r_call, reference_sentence("Wrote", ref));
	}
	return Outcome::Ok;
}

Outcome ref_size_native(NativeCall &r_call) {
	if (unbound_argument(r_call)) {
		return Outcome::Park;
	}
	GodotBridge *bridge = bridge_of(r_call);
	int64_t ref = 0;
	if (bridge == nullptr || !int_argument(r_call, 0, ref)) {
		return Outcome::Invalid;
	}
	const vh_godot_api &api = bridge->interpreter.godot;
	if (api.RefSize == nullptr) {
		r_call.result = Value::from_int32(0);
		return Outcome::Ok;
	}
	if (ref == 0) {
		return raise(r_call, reference_sentence("Sized", ref));
	}
	int64_t size = 0;
	if (api.RefSize(api.Ctx, ref, &size) != VH_CALL_OK) {
		return raise(r_call, reference_sentence("Sized", ref));
	}
	r_call.result = make_int(r_call.heap, size);
	return Outcome::Ok;
}

Outcome ref_new_native(NativeCall &r_call) {
	if (unbound_argument(r_call)) {
		return Outcome::Park;
	}
	GodotBridge *bridge = bridge_of(r_call);
	int64_t tag = 0;
	if (bridge == nullptr || !int_argument(r_call, 0, tag)) {
		return Outcome::Invalid;
	}
	const vh_godot_api &api = bridge->interpreter.godot;
	const int64_t ref = api.NewRef != nullptr ? api.NewRef(api.Ctx, int32_t(tag)) : 0;
	r_call.result = make_int(r_call.heap, ref);
	return Outcome::Ok;
}

Outcome ref_new_default_native(NativeCall &r_call) {
	GodotBridge *bridge = bridge_of(r_call);
	if (bridge != nullptr && bridge->interpreter.mint_suppressed) {
		if (unbound_argument(r_call)) {
			return Outcome::Park;
		}
		r_call.result = Value::from_int32(0);
		return Outcome::Ok;
	}
	return ref_new_native(r_call);
}

// §8.35 to §8.38.
Outcome ref_invoke(NativeCall &r_call, bool p_answer) {
	if (unbound_argument(r_call)) {
		return Outcome::Park;
	}
	GodotBridge *bridge = bridge_of(r_call);
	int64_t ref = 0;
	OwnedWire args;
	if (bridge == nullptr || !int_argument(r_call, 0, ref) || !variant_arguments(*bridge, argument(r_call, 1), args)) {
		return Outcome::Invalid;
	}
	if (ref == 0) {
		return raise(r_call, reference_sentence("Called", ref));
	}
	const vh_godot_api &api = bridge->interpreter.godot;
	ScopedArena arena;
	vh_value result = {};
	if (api.InvokeCallable != nullptr) {
		if (api.InvokeCallable(api.Ctx, ref, args.values.data(), int32_t(args.values.size()), arena.get(), &result) != VH_CALL_OK) {
			return raise(r_call, reference_sentence("Called", ref));
		}
	}
	r_call.result = p_answer ? variant_result(*bridge, result) : r_call.heap.false_value();
	return Outcome::Ok;
}

Outcome ref_invoke_native(NativeCall &r_call) {
	return ref_invoke(r_call, true);
}

Outcome ref_invoke_void_native(NativeCall &r_call) {
	return ref_invoke(r_call, false);
}

Outcome ref_call(NativeCall &r_call, bool p_answer) {
	if (unbound_argument(r_call)) {
		return Outcome::Park;
	}
	GodotBridge *bridge = bridge_of(r_call);
	int64_t ref = 0;
	std::string method;
	OwnedWire args;
	if (bridge == nullptr || !int_argument(r_call, 0, ref) || !string_bytes(argument(r_call, 1), method) ||
			!variant_arguments(*bridge, argument(r_call, 2), args)) {
		return Outcome::Invalid;
	}
	if (ref == 0) {
		return raise(r_call, reference_sentence("Called a method on", ref));
	}
	const vh_godot_api &api = bridge->interpreter.godot;
	ScopedArena arena;
	vh_value result = {};
	if (api.RefCall != nullptr) {
		const int32_t status = api.RefCall(api.Ctx, ref, method.data(), int32_t(method.size()), args.values.data(), int32_t(args.values.size()),
				arena.get(), &result);
		if (status == VH_CALL_NO_SUCH_MEMBER) {
			return raise(r_call, "Godot has no method `" + method + "` on the value reference " + std::to_string(ref) + " names.");
		}
		if (status != VH_CALL_OK) {
			return raise(r_call, reference_sentence("Called a method on", ref));
		}
	}
	r_call.result = p_answer ? variant_result(*bridge, result) : r_call.heap.false_value();
	return Outcome::Ok;
}

Outcome ref_call_native(NativeCall &r_call) {
	return ref_call(r_call, true);
}

Outcome ref_call_void_native(NativeCall &r_call) {
	return ref_call(r_call, false);
}

enum class BulkShape : uint8_t {
	Ints,
	Floats,
	Strings,
	Values,
};

void flatten_floats(const vh_value &p_wire, std::vector<Value> &r_values) {
	if (p_wire.Type == VH_TYPE_TUPLE || p_wire.Type == VH_TYPE_ARRAY) {
		for (int32_t index = 0; index < p_wire.Seq.Count; ++index) {
			flatten_floats(p_wire.Seq.Items[index], r_values);
		}
		return;
	}
	r_values.push_back(Value::from_float(wire_double(p_wire)));
}

// §8.39 to §8.42: an empty array whenever the container cannot be read.
Outcome ref_contents(NativeCall &r_call, BulkShape p_shape) {
	if (unbound_argument(r_call)) {
		return Outcome::Park;
	}
	GodotBridge *bridge = bridge_of(r_call);
	int64_t ref = 0;
	if (bridge == nullptr || !int_argument(r_call, 0, ref)) {
		return Outcome::Invalid;
	}
	const vh_godot_api &api = bridge->interpreter.godot;
	std::vector<Value> values;
	ScopedArena arena;
	vh_value contents = {};
	if (ref != 0 && api.RefContents != nullptr && api.RefContents(api.Ctx, ref, arena.get(), &contents) == VH_CALL_OK &&
			(contents.Type == VH_TYPE_ARRAY || contents.Type == VH_TYPE_TUPLE)) {
		for (int32_t index = 0; index < contents.Seq.Count; ++index) {
			const vh_value &element = contents.Seq.Items[index];
			switch (p_shape) {
				case BulkShape::Ints:
					values.push_back(make_int(r_call.heap, wire_int(element)));
					break;
				case BulkShape::Floats:
					flatten_floats(element, values);
					break;
				case BulkShape::Strings:
					values.push_back(make_string(r_call.heap, element.Type == VH_TYPE_STRING ? wire_text(element) : std::string()));
					break;
				case BulkShape::Values:
					values.push_back(variant_result(*bridge, element));
					break;
			}
		}
	}
	r_call.result = make_array(r_call.heap, values, false);
	return Outcome::Ok;
}

Outcome ref_ints_native(NativeCall &r_call) {
	return ref_contents(r_call, BulkShape::Ints);
}

Outcome ref_floats_native(NativeCall &r_call) {
	return ref_contents(r_call, BulkShape::Floats);
}

Outcome ref_strings_native(NativeCall &r_call) {
	return ref_contents(r_call, BulkShape::Strings);
}

Outcome ref_values_native(NativeCall &r_call) {
	return ref_contents(r_call, BulkShape::Values);
}

// §8.43 to §8.46.
Outcome ref_from(NativeCall &r_call, BulkShape p_shape) {
	if (unbound_argument(r_call)) {
		return Outcome::Park;
	}
	GodotBridge *bridge = bridge_of(r_call);
	int64_t tag = 0;
	const Value values = follow(argument(r_call, 1));
	if (bridge == nullptr || !int_argument(r_call, 0, tag)) {
		return Outcome::Invalid;
	}
	const vh_godot_api &api = bridge->interpreter.godot;
	const int64_t ref = api.NewRef != nullptr ? api.NewRef(api.Ctx, int32_t(tag)) : 0;
	r_call.result = make_int(r_call.heap, ref);
	if (ref == 0 || api.RefSet == nullptr || (!is_cell_kind(values, CellKind::Array) && !is_cell_kind(values, CellKind::MutableArray))) {
		return Outcome::Ok;
	}
	const ArrayCell *array = cell_as<ArrayCell>(values);
	for (size_t index = 0; index < array->length(); ++index) {
		ScopedArena arena;
		vh_value key = {};
		key.Type = VH_TYPE_INT;
		key.VariantTag = VH_VARIANT_INT;
		key.Int = int64_t(index);
		vh_value element = {};
		const Value item = follow(array->get(index));
		RuntimeError error;
		switch (p_shape) {
			case BulkShape::Ints:
				element.Type = VH_TYPE_INT;
				element.VariantTag = VH_VARIANT_INT;
				int_to_int64(item, element.Int, error);
				break;
			case BulkShape::Floats:
				element.Type = VH_TYPE_FLOAT;
				element.VariantTag = VH_VARIANT_FLOAT;
				element.Float = item.is_float() ? item.as_float() : 0.0;
				break;
			case BulkShape::Strings: {
				std::string text;
				string_bytes(item, text);
				string_to_wire(text, arena.get(), element);
				element.VariantTag = VH_VARIANT_STRING;
				break;
			}
			case BulkShape::Values: {
				VariantLanes lanes;
				bridge->read_variant(item, lanes);
				bridge->variant_to_wire(lanes, arena.get(), element);
				break;
			}
		}
		api.RefSet(api.Ctx, ref, &key, &element);
	}
	return Outcome::Ok;
}

Outcome ref_from_ints_native(NativeCall &r_call) {
	return ref_from(r_call, BulkShape::Ints);
}

Outcome ref_from_floats_native(NativeCall &r_call) {
	return ref_from(r_call, BulkShape::Floats);
}

Outcome ref_from_strings_native(NativeCall &r_call) {
	return ref_from(r_call, BulkShape::Strings);
}

Outcome ref_from_values_native(NativeCall &r_call) {
	return ref_from(r_call, BulkShape::Values);
}

struct GodotBinding {
	const char *key;
	NativeFn implementation;
};

constexpr GodotBinding kGodotNatives[] = {
	{ "(/Godot.org/Godot/(/Godot.org/Godot:)VhIsValid(:int):)Native", &is_valid_native },
	{ "(/Godot.org/Godot/(/Godot.org/Godot:)VhTypeMismatch(:[]char,:(/Godot.org/Godot:)variant):)Native", &type_mismatch_native },
	{ "(/Godot.org/Godot/(/Godot.org/Godot:)VhCallValue(:int,:[]char,:[](/Godot.org/Godot:)variant):)Native", &call_value_native },
	{ "(/Godot.org/Godot/(/Godot.org/Godot:)VhCallValueConst(:int,:[]char,:[](/Godot.org/Godot:)variant):)Native", &call_value_native },
	{ "(/Godot.org/Godot/(/Godot.org/Godot:)VhGetValue(:int,:[]char):)Native", &get_value_native },
	{ "(/Godot.org/Godot/(/Godot.org/Godot:)VhVariantFromAny(:any):)Native", &variant_from_any_native },
	{ "(/Godot.org/Godot/(/Godot.org/Godot:)VhCallVoid(:int,:[]char,:[](/Godot.org/Godot:)variant):)Native", &call_void_native },
	{ "(/Godot.org/Godot/(/Godot.org/Godot:)VhSetValue(:int,:[]char,:(/Godot.org/Godot:)variant):)Native", &set_value_native },
	{ "(/Godot.org/Godot/(/Godot.org/Godot:)VhSingletonObject(:[]char):)Native", &singleton_object_native },
	{ "(/Godot.org/Godot/(/Godot.org/Godot:)VhObjectOf(:int):)Native", &object_of_native },
	{ "(/Godot.org/Godot/(/Godot.org/Godot:)VhCallableFrom(:any):)Native", &callable_from_native },
	{ "(/Godot.org/Godot/(/Godot.org/Godot:)VhSignalEmit(:int,:any):)Native", &signal_emit_native },
	{ "(/Godot.org/Godot/(/Godot.org/Godot:)VhSignalSubscribe(:int,:any):)Native", &signal_subscribe_native },
	{ "(/Godot.org/Godot/(/Godot.org/Godot:)VhSignalCancel(:int):)Native", &signal_cancel_native },
	{ "(/Godot.org/Godot/(/Godot.org/Godot:)VhEventEmit(:any,:any):)Native", &event_emit_native },
	{ "(/Godot.org/Godot/(/Godot.org/Godot:)VhEventSubscribe(:any,:any):)Native", &event_subscribe_native },
	{ "(/Godot.org/Godot/(/Godot.org/Godot:)VhSignalBind(:int,:[]char,:[]char,:[]char):)Native", &signal_bind_native },
	{ "(/Godot.org/Godot/(/Godot.org/Godot:)VhSignalAwait(:(/Godot.org/Godot:)vh_signal):)Native", &signal_await_native },
	{ "(/Godot.org/Godot/(/Godot.org/Godot:)VhSignalAwaitEnd(:int):)Native", &signal_await_end_native },
	{ "(/Godot.org/Godot/(/Godot.org/Godot:)VhSignalRefAwait(:int,:(/Godot.org/Godot:)vh_signal):)Native", &signal_ref_await_native },
	{ "(/Godot.org/Godot/(/Godot.org/Godot:)VhSignalRefSubscribe(:int,:any):)Native", &signal_ref_subscribe_native },
	{ "(/Godot.org/Godot/(/Godot.org/Godot:)VhSignalRefFor(:int,:[]char):)Native", &signal_ref_for_native },
	{ "(/Godot.org/Godot/(/Godot.org/Godot:)VhCallStatic(:[]char,:[]char,:[](/Godot.org/Godot:)variant):)Native", &call_static_native },
	{ "(/Godot.org/Godot/(/Godot.org/Godot:)VhCallUtility(:[]char,:[](/Godot.org/Godot:)variant):)Native", &call_utility_native },
	{ "(/Godot.org/Godot/(/Godot.org/Godot:)VhCallUtilityConst(:[]char,:[](/Godot.org/Godot:)variant):)Native", &call_utility_native },
	{ "(/Godot.org/Godot/(/Godot.org/Godot:)VhAdoptRef(:(/Godot.org/Godot:)godot_ref):)Native", &adopt_ref_native },
	{ "(/Godot.org/Godot/(/Godot.org/Godot:)VhRefGet(:int,:(/Godot.org/Godot:)variant):)Native", &ref_get_native },
	{ "(/Godot.org/Godot/(/Godot.org/Godot:)VhRefSet(:int,:(/Godot.org/Godot:)variant,:(/Godot.org/Godot:)variant):)Native", &ref_set_native },
	{ "(/Godot.org/Godot/(/Godot.org/Godot:)VhRefSize(:int):)Native", &ref_size_native },
	{ "(/Godot.org/Godot/(/Godot.org/Godot:)VhRefNew(:int):)Native", &ref_new_native },
	{ "(/Godot.org/Godot/(/Godot.org/Godot:)VhRefNewDefault(:int):)Native", &ref_new_default_native },
	{ "(/Godot.org/Godot/(/Godot.org/Godot:)VhRefInvoke(:int,:[](/Godot.org/Godot:)variant):)Native", &ref_invoke_native },
	{ "(/Godot.org/Godot/(/Godot.org/Godot:)VhRefInvokeVoid(:int,:[](/Godot.org/Godot:)variant):)Native", &ref_invoke_void_native },
	{ "(/Godot.org/Godot/(/Godot.org/Godot:)VhRefCall(:int,:[]char,:[](/Godot.org/Godot:)variant):)Native", &ref_call_native },
	{ "(/Godot.org/Godot/(/Godot.org/Godot:)VhRefCallVoid(:int,:[]char,:[](/Godot.org/Godot:)variant):)Native", &ref_call_void_native },
	{ "(/Godot.org/Godot/(/Godot.org/Godot:)VhRefInts(:int):)Native", &ref_ints_native },
	{ "(/Godot.org/Godot/(/Godot.org/Godot:)VhRefFloats(:int):)Native", &ref_floats_native },
	{ "(/Godot.org/Godot/(/Godot.org/Godot:)VhRefStrings(:int):)Native", &ref_strings_native },
	{ "(/Godot.org/Godot/(/Godot.org/Godot:)VhRefValues(:int):)Native", &ref_values_native },
	{ "(/Godot.org/Godot/(/Godot.org/Godot:)VhRefFromInts(:int,:[]int):)Native", &ref_from_ints_native },
	{ "(/Godot.org/Godot/(/Godot.org/Godot:)VhRefFromFloats(:int,:[]float):)Native", &ref_from_floats_native },
	{ "(/Godot.org/Godot/(/Godot.org/Godot:)VhRefFromStrings(:int,:[][]char):)Native", &ref_from_strings_native },
	{ "(/Godot.org/Godot/(/Godot.org/Godot:)VhRefFromValues(:int,:[](/Godot.org/Godot:)variant):)Native", &ref_from_values_native },
};

} // namespace

NativeFn godot_native(std::string_view p_binding_key) {
	for (const GodotBinding &binding : kGodotNatives) {
		if (p_binding_key == binding.key) {
			return binding.implementation;
		}
	}
	return nullptr;
}

} // namespace vm
