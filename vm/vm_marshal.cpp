#include "vm_marshal.h"

#include <cstring>
#include <vector>

#include "vm_cell.h"
#include "vm_number.h"
#include "vm_values.h"

namespace vm {

namespace {

// The wire's Char is a code point and does not say whether the parameter was `char` or `char32`;
// an ASCII code point is the one both spell alike.
Value char_from_wire(uint32_t p_code_point) {
	return p_code_point < 0x80 ? Value::from_char8(uint8_t(p_code_point)) : Value::from_char32(p_code_point);
}

bool element_to_value(Heap &r_heap, const vh_value &p_wire, Value &r_value) {
	return wire_to_value(r_heap, p_wire, p_wire.Type, r_value);
}

template <typename T>
T *allocate(vh_arena *p_arena, size_t p_count) {
	if (p_count == 0) {
		return nullptr;
	}
	return static_cast<T *>(p_arena->Alloc(p_arena, sizeof(T) * p_count, alignof(T)));
}

bool self_described_to_wire(Value p_value, vh_arena *p_arena, vh_value &r_wire, std::string &r_why);

bool sequence_to_wire(Value p_value, int32_t p_type, vh_arena *p_arena, vh_value &r_wire, std::string &r_why) {
	r_wire.Type = p_type;
	if (is_cell_kind(p_value, CellKind::False)) {
		return true;
	}
	if (!is_cell_kind(p_value, CellKind::Array) && !is_cell_kind(p_value, CellKind::MutableArray)) {
		r_why = "an array result that is not an array";
		return false;
	}
	const ArrayCell *array = cell_as<ArrayCell>(p_value);
	vh_value *items = allocate<vh_value>(p_arena, array->length());
	if (array->length() != 0 && items == nullptr) {
		r_why = "the arena is full";
		return false;
	}
	for (size_t index = 0; index < array->length(); ++index) {
		items[index] = vh_value{};
		if (!self_described_to_wire(follow(read_slot(array->get(index))), p_arena, items[index], r_why)) {
			return false;
		}
	}
	r_wire.Seq.Items = items;
	r_wire.Seq.Count = int32_t(array->length());
	return true;
}

bool string_to_wire(Value p_value, vh_arena *p_arena, vh_value &r_wire, std::string &r_why) {
	std::string bytes;
	if (!string_bytes(p_value, bytes)) {
		r_why = "a string result that is not a string";
		return false;
	}
	char *text = allocate<char>(p_arena, bytes.size());
	if (!bytes.empty() && text == nullptr) {
		r_why = "the arena is full";
		return false;
	}
	if (!bytes.empty()) {
		std::memcpy(text, bytes.data(), bytes.size());
	}
	r_wire.Type = VH_TYPE_STRING;
	r_wire.String.Utf8 = text;
	r_wire.String.Len = int32_t(bytes.size());
	return true;
}

bool self_described_to_wire(Value p_value, vh_arena *p_arena, vh_value &r_wire, std::string &r_why) {
	if (is_int(p_value)) {
		return value_to_wire(p_value, VH_TYPE_INT, 0, p_arena, r_wire, r_why);
	}
	if (p_value.is_float()) {
		return value_to_wire(p_value, VH_TYPE_FLOAT, 0, p_arena, r_wire, r_why);
	}
	if (p_value.is_char8() || p_value.is_char32()) {
		return value_to_wire(p_value, VH_TYPE_CHAR, 0, p_arena, r_wire, r_why);
	}
	if (is_cell_kind(p_value, CellKind::False) || is_cell_kind(p_value, CellKind::True)) {
		return value_to_wire(p_value, VH_TYPE_LOGIC, 0, p_arena, r_wire, r_why);
	}
	if (is_cell_kind(p_value, CellKind::Option)) {
		return value_to_wire(p_value, VH_TYPE_OPTION, 0, p_arena, r_wire, r_why);
	}
	if (is_cell_kind(p_value, CellKind::Array) || is_cell_kind(p_value, CellKind::MutableArray)) {
		const ArrayCell *array = cell_as<ArrayCell>(p_value);
		if (array->storage == ArrayCell::Storage::Char8 && array->length() != 0) {
			return string_to_wire(p_value, p_arena, r_wire, r_why);
		}
		return sequence_to_wire(p_value, VH_TYPE_ARRAY, p_arena, r_wire, r_why);
	}
	if (is_cell_kind(p_value, CellKind::Map) || is_cell_kind(p_value, CellKind::MutableMap)) {
		return value_to_wire(p_value, VH_TYPE_MAP, 0, p_arena, r_wire, r_why);
	}
	r_why = std::string("a ") + (p_value.is_cell() ? cell_kind_name(p_value.as_cell()->kind) : "value") + " inside a result, which this runtime cannot carry yet (T5.1)";
	return false;
}

} // namespace

bool wire_to_value(Heap &r_heap, const vh_value &p_wire, int32_t p_type, Value &r_value) {
	switch (p_type) {
		case VH_TYPE_LOGIC:
			if (p_wire.Type != VH_TYPE_LOGIC) {
				return false;
			}
			r_value = r_heap.logic(p_wire.Logic != 0);
			return true;
		case VH_TYPE_INT:
			if (p_wire.Type != VH_TYPE_INT) {
				return false;
			}
			r_value = make_int(r_heap, p_wire.Int);
			return true;
		case VH_TYPE_FLOAT:
			if (p_wire.Type == VH_TYPE_FLOAT) {
				r_value = Value::from_float(p_wire.Float);
				return true;
			}
			if (p_wire.Type == VH_TYPE_INT) {
				r_value = Value::from_float(double(p_wire.Int));
				return true;
			}
			return false;
		case VH_TYPE_CHAR:
			if (p_wire.Type != VH_TYPE_CHAR) {
				return false;
			}
			r_value = char_from_wire(p_wire.Char);
			return true;
		case VH_TYPE_STRING:
			if (p_wire.Type != VH_TYPE_STRING || (p_wire.String.Len > 0 && p_wire.String.Utf8 == nullptr) || p_wire.String.Len < 0) {
				return false;
			}
			r_value = make_string(r_heap, std::string_view(p_wire.String.Utf8 != nullptr ? p_wire.String.Utf8 : "", size_t(p_wire.String.Len)));
			return true;
		case VH_TYPE_ARRAY:
		case VH_TYPE_TUPLE: {
			if ((p_wire.Type != VH_TYPE_ARRAY && p_wire.Type != VH_TYPE_TUPLE) || p_wire.Seq.Count < 0) {
				return false;
			}
			std::vector<Value> elements;
			for (int32_t index = 0; index < p_wire.Seq.Count; ++index) {
				Value element;
				if (!element_to_value(r_heap, p_wire.Seq.Items[index], element)) {
					return false;
				}
				elements.push_back(element);
			}
			r_value = make_array(r_heap, elements, false);
			return true;
		}
		case VH_TYPE_OPTION: {
			if (p_wire.Type != VH_TYPE_OPTION) {
				return false;
			}
			if (p_wire.Option == nullptr) {
				r_value = r_heap.false_value();
				return true;
			}
			Value content;
			if (!element_to_value(r_heap, *p_wire.Option, content)) {
				return false;
			}
			r_value = make_option(r_heap, content);
			return true;
		}
		case VH_TYPE_MAP: {
			if (p_wire.Type != VH_TYPE_MAP || p_wire.Map.Count < 0) {
				return false;
			}
			std::vector<Value> keys;
			std::vector<Value> values;
			for (int32_t index = 0; index < p_wire.Map.Count; ++index) {
				Value key;
				Value value;
				if (!element_to_value(r_heap, p_wire.Map.Pairs[index].Key, key) || !element_to_value(r_heap, p_wire.Map.Pairs[index].Value, value)) {
					return false;
				}
				keys.push_back(key);
				values.push_back(value);
			}
			return make_map(r_heap, keys, values, r_value) == Outcome::Ok;
		}
		default:
			return false;
	}
}

bool value_to_wire(Value p_value, int32_t p_type, int32_t p_tag, vh_arena *p_arena, vh_value &r_wire, std::string &r_why) {
	r_wire = vh_value{};
	r_wire.VariantTag = p_tag;
	p_value = follow(read_slot(p_value));
	switch (p_type) {
		case VH_TYPE_VOID:
			r_wire.Type = VH_TYPE_VOID;
			return true;
		case VH_TYPE_LOGIC:
			r_wire.Type = VH_TYPE_LOGIC;
			r_wire.Logic = is_cell_kind(p_value, CellKind::True) ? 1 : 0;
			return true;
		case VH_TYPE_INT: {
			RuntimeError error;
			int64_t number = 0;
			if (int_to_int64(p_value, number, error) != Outcome::Ok) {
				r_why = error.message.empty() ? std::string("an int result that is not an integer") : error.message;
				return false;
			}
			r_wire.Type = VH_TYPE_INT;
			r_wire.Int = number;
			return true;
		}
		case VH_TYPE_FLOAT:
			if (!p_value.is_float()) {
				r_why = "a float result that is not a float";
				return false;
			}
			r_wire.Type = VH_TYPE_FLOAT;
			r_wire.Float = p_value.as_float();
			return true;
		case VH_TYPE_CHAR:
			if (!p_value.is_char8() && !p_value.is_char32()) {
				r_why = "a char result that is not a char";
				return false;
			}
			r_wire.Type = VH_TYPE_CHAR;
			r_wire.Char = p_value.is_char8() ? p_value.as_char8() : p_value.as_char32();
			return true;
		case VH_TYPE_STRING:
			if (!string_to_wire(p_value, p_arena, r_wire, r_why)) {
				return false;
			}
			r_wire.VariantTag = p_tag;
			return true;
		case VH_TYPE_ARRAY:
		case VH_TYPE_TUPLE:
			if (!sequence_to_wire(p_value, p_type, p_arena, r_wire, r_why)) {
				return false;
			}
			r_wire.VariantTag = p_tag;
			return true;
		case VH_TYPE_OPTION: {
			r_wire.Type = VH_TYPE_OPTION;
			if (is_cell_kind(p_value, CellKind::False)) {
				return true;
			}
			vh_value *content = allocate<vh_value>(p_arena, 1);
			if (content == nullptr) {
				r_why = "the arena is full";
				return false;
			}
			*content = vh_value{};
			const Value inner = is_cell_kind(p_value, CellKind::True) ? Value() : follow(cell_as<OptionCell>(p_value)->content);
			if (is_cell_kind(p_value, CellKind::True)) {
				content->Type = VH_TYPE_LOGIC;
			} else if (!self_described_to_wire(inner, p_arena, *content, r_why)) {
				return false;
			}
			r_wire.Option = content;
			return true;
		}
		case VH_TYPE_MAP: {
			r_wire.Type = VH_TYPE_MAP;
			if (!is_cell_kind(p_value, CellKind::Map) && !is_cell_kind(p_value, CellKind::MutableMap)) {
				return is_cell_kind(p_value, CellKind::False);
			}
			const MapCell *map = cell_as<MapCell>(p_value);
			vh_pair *pairs = allocate<vh_pair>(p_arena, map->entries.size());
			if (!map->entries.empty() && pairs == nullptr) {
				r_why = "the arena is full";
				return false;
			}
			for (size_t index = 0; index < map->entries.size(); ++index) {
				pairs[index] = vh_pair{};
				if (!self_described_to_wire(follow(map->entries[index].key), p_arena, pairs[index].Key, r_why) ||
						!self_described_to_wire(follow(read_slot(map->entries[index].value)), p_arena, pairs[index].Value, r_why)) {
					return false;
				}
			}
			r_wire.Map.Pairs = pairs;
			r_wire.Map.Count = int32_t(map->entries.size());
			return true;
		}
		default:
			break;
	}
	r_why = "a result of vh_type " + std::to_string(p_type) + ", which this runtime cannot carry yet (T5.1)";
	return false;
}

} // namespace vm
