#pragma once

#include <cstdint>
#include <memory>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "vm_cell.h"
#include "vm_heap.h"
#include "vm_value.h"

// Class layouts and objects: spec/objects.md §4, §6 and §7.
namespace vm {

enum class FieldKind : uint8_t {
	Slot,
	Constant,
	Accessor,
};

// One name in a layout. A slot and an accessor each have a per-object index, `slot`: a slot's
// content and an accessor's created mark live there. `value` is a constant's or accessor's value,
// and for a slot the first entry's value, which is what a value object the file wrote without that
// field holds.
struct LayoutField {
	const NameCell *name = nullptr;
	FieldKind kind = FieldKind::Slot;
	Value value = Value::uninitialized();
	Value type = Value::uninitialized();
	uint8_t entry_flags = 0;
	uint32_t slot = 0;
};

// Built with objects.md §4.3's native rule for every class, which §4.3 allows: every data member is
// a slot, so the layout is per class and an instantiation expression's archetype changes nothing.
struct ClassLayout {
	const ClassCell *layout_class = nullptr;
	std::vector<LayoutField> fields;
	std::unordered_map<const NameCell *, uint32_t> by_name;
	std::vector<const NameCell *> slot_names;

	const LayoutField *find(const NameCell *p_name) const {
		const auto found = by_name.find(p_name);
		return found == by_name.end() ? nullptr : &fields[found->second];
	}
};

// The classes whose class-body archetypes §4.1 visits for p_class, in visit order: its own body,
// its superclass chain, then on the way back down each level's interfaces. An interface reached
// twice is listed twice.
void class_body_order(const ClassCell *p_class, std::vector<const ClassCell *> &r_order);

// Whether p_class is p_ancestor or inherits it, through superclasses and interfaces (§13).
bool class_inherits(const ClassCell *p_class, const ClassCell *p_ancestor);

// Whether an archetype entry's value is a method: a function awaiting a receiver, on an entry with
// no declared type (§3.3).
bool is_method_entry(const ArchetypeEntry &p_entry);

// A decorated field name without its leading `(<scope>:)` qualifier (§1), which may itself nest
// parentheses: `(/user@localhost/player:)Speed` is `Speed`.
std::string_view unqualified_name(std::string_view p_name);

// The first slot of p_layout, in visit order, whose unqualified name is p_name, or null.
const LayoutField *find_slot_by_unqualified_name(const ClassLayout &p_layout, std::string_view p_name);

class Layouts {
public:
	const ClassLayout &get(const ClassCell *p_class);

	// A new object of p_layout's class, every slot fresh and uncreated (§7.1 NewObject).
	ObjectCell *new_object(Heap &r_heap, const ClassLayout &p_layout);

	// Gives a value object the loader built its class's layout: the file's fields in their slots,
	// every other slot the class body's value, all of them created (§17, kind 26).
	void lay_out_value_object(ObjectCell *r_object);

	// Every layout is kept for the program's life, keyed by its class, so its class and what it
	// copied out of the archetypes are roots: a class freed under a layout would hand its address,
	// and so its layout, to the next class allocated there.
	void visit_references(CellVisitor &r_visitor) const;

private:
	std::unordered_map<const ClassCell *, std::unique_ptr<ClassLayout>> layouts;
};

} // namespace vm
