#include "vm_objects.h"

#include <string>

namespace vm {

namespace {

std::string lowercase(const std::string &p_text) {
	std::string lowered = p_text;
	for (char &c : lowered) {
		if (c >= 'A' && c <= 'Z') {
			c = char(c - 'A' + 'a');
		}
	}
	return lowered;
}

} // namespace

void class_body_order(const ClassCell *p_class, std::vector<const ClassCell *> &r_order) {
	if (p_class == nullptr || p_class->archetype == nullptr) {
		return;
	}
	r_order.push_back(p_class);
	const ArchetypeCell *next = p_class->archetype->next;
	if (next != nullptr && next->owner != nullptr) {
		class_body_order(next->owner, r_order);
	}
	for (const ClassCell *inherited : p_class->inherited) {
		if (inherited != nullptr && inherited->class_kind == ClassKind::Interface) {
			class_body_order(inherited, r_order);
		}
	}
}

bool class_inherits(const ClassCell *p_class, const ClassCell *p_ancestor) {
	if (p_class == nullptr) {
		return false;
	}
	if (p_class == p_ancestor) {
		return true;
	}
	for (const ClassCell *inherited : p_class->inherited) {
		if (class_inherits(inherited, p_ancestor)) {
			return true;
		}
	}
	return false;
}

bool is_method_entry(const ArchetypeEntry &p_entry) {
	return p_entry.type.is_uninitialized() && is_cell_kind(p_entry.value, CellKind::Function) &&
			cell_as<FunctionCell>(p_entry.value)->self.is_uninitialized();
}

std::string_view unqualified_name(std::string_view p_name) {
	if (p_name.empty() || p_name[0] != '(') {
		return p_name;
	}
	int depth = 0;
	for (size_t index = 0; index < p_name.size(); ++index) {
		if (p_name[index] == '(') {
			++depth;
		} else if (p_name[index] == ')' && --depth == 0) {
			return p_name.substr(index + 1);
		}
	}
	return p_name;
}

const LayoutField *find_slot_by_unqualified_name(const ClassLayout &p_layout, std::string_view p_name) {
	for (const LayoutField &field : p_layout.fields) {
		if (field.kind == FieldKind::Slot && unqualified_name(field.name->text) == p_name) {
			return &field;
		}
	}
	return nullptr;
}

const ClassLayout &Layouts::get(const ClassCell *p_class) {
	std::unique_ptr<ClassLayout> &layout = layouts[p_class];
	if (layout != nullptr) {
		return *layout;
	}
	layout = std::make_unique<ClassLayout>();
	layout->layout_class = p_class;

	std::vector<const ClassCell *> order;
	class_body_order(p_class, order);
	const bool case_insensitive = (p_class->flags & ClassCell::FLAG_EMULATE_CASE_INSENSITIVE_OVERRIDES) != 0;
	std::unordered_map<std::string, Value> methods_by_lowercase;

	for (const ClassCell *body : order) {
		for (const ArchetypeEntry &entry : body->archetype->entries) {
			if (entry.name == nullptr || layout->by_name.count(entry.name) != 0) {
				continue;
			}
			LayoutField field;
			field.name = entry.name;
			field.value = entry.value;
			field.type = entry.type;
			field.entry_flags = entry.flags;
			if (is_cell_kind(entry.value, CellKind::Accessor)) {
				field.kind = FieldKind::Accessor;
			} else if (is_method_entry(entry)) {
				field.kind = FieldKind::Constant;
				// spec/objects.md §4.4: every class of this engine commit carries the flag.
				if (case_insensitive) {
					const auto inserted = methods_by_lowercase.emplace(lowercase(entry.name->text), entry.value);
					field.value = inserted.first->second;
				}
			} else if (!entry.type.is_uninitialized() || entry.value.is_uninitialized()) {
				field.kind = FieldKind::Slot;
			} else {
				field.kind = FieldKind::Constant;
			}
			if (field.kind != FieldKind::Constant) {
				field.slot = uint32_t(layout->slot_names.size());
				layout->slot_names.push_back(entry.name);
			}
			layout->by_name.emplace(entry.name, uint32_t(layout->fields.size()));
			layout->fields.push_back(field);
		}
	}
	return *layout;
}

ObjectCell *Layouts::new_object(Heap &r_heap, const ClassLayout &p_layout) {
	ObjectCell *object = r_heap.make<ObjectCell>();
	object->object_class = p_layout.layout_class;
	object->layout = &p_layout;
	object->field_names = p_layout.slot_names;
	object->field_values.assign(p_layout.slot_names.size(), Value::empty());
	object->created.assign(p_layout.slot_names.size(), false);
	return object;
}

void Layouts::lay_out_value_object(ObjectCell *r_object) {
	if (r_object->layout != nullptr || r_object->object_class == nullptr) {
		return;
	}
	const ClassLayout &layout = get(r_object->object_class);
	std::vector<Value> values(layout.slot_names.size(), Value::empty());
	for (const LayoutField &field : layout.fields) {
		if (field.kind == FieldKind::Slot && !field.value.is_uninitialized()) {
			values[field.slot] = field.value;
		}
	}
	for (size_t index = 0; index < r_object->field_names.size(); ++index) {
		const LayoutField *field = layout.find(r_object->field_names[index]);
		if (field != nullptr && field->kind != FieldKind::Constant) {
			values[field->slot] = r_object->field_values[index];
		}
	}
	r_object->layout = &layout;
	r_object->field_names = layout.slot_names;
	r_object->field_values = std::move(values);
	r_object->created.assign(layout.slot_names.size(), true);
}

} // namespace vm
