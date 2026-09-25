#include "vm_values.h"

#include <algorithm>

#include "vm_cell.h"
#include "vm_equality.h"
#include "vm_number.h"

namespace vm {

namespace {

constexpr size_t LINEAR_MAP_LIMIT = 8;

// Map keys are concrete, so a placeholder met inside one is a difference, not a binding.
class KeyMeeter : public PlaceholderMeeter {
public:
	Equality meet(Value, Value) override { return Equality::Neq; }
};

bool is_array(Value p_value) {
	return is_cell_kind(p_value, CellKind::Array) || is_cell_kind(p_value, CellKind::MutableArray);
}

bool is_map(Value p_value) {
	return is_cell_kind(p_value, CellKind::Map) || is_cell_kind(p_value, CellKind::MutableMap);
}

bool keys_equal(Value p_left, Value p_right) {
	KeyMeeter meeter;
	return values_equal(p_left, p_right, &meeter) == Equality::Eq;
}

int64_t map_find(const MapCell *p_map, Value p_key, uint64_t p_hash) {
	if (p_map->index.empty()) {
		for (size_t i = 0; i < p_map->entries.size(); ++i) {
			const MapEntry &entry = p_map->entries[i];
			if (entry.hash == p_hash && keys_equal(entry.key, p_key)) {
				return int64_t(i);
			}
		}
		return -1;
	}
	const size_t mask = p_map->index.size() - 1;
	for (size_t slot = size_t(p_hash) & mask; p_map->index[slot] != 0; slot = (slot + 1) & mask) {
		const size_t position = p_map->index[slot] - 1;
		const MapEntry &entry = p_map->entries[position];
		if (entry.hash == p_hash && keys_equal(entry.key, p_key)) {
			return int64_t(position);
		}
	}
	return -1;
}

void index_insert(MapCell *r_map, size_t p_position) {
	const size_t mask = r_map->index.size() - 1;
	size_t slot = size_t(r_map->entries[p_position].hash) & mask;
	while (r_map->index[slot] != 0) {
		slot = (slot + 1) & mask;
	}
	r_map->index[slot] = uint32_t(p_position + 1);
}

void rebuild_index(MapCell *r_map) {
	r_map->index.clear();
	if (r_map->entries.size() <= LINEAR_MAP_LIMIT) {
		return;
	}
	size_t size = 16;
	while (size < r_map->entries.size() * 2) {
		size *= 2;
	}
	r_map->index.assign(size, 0);
	for (size_t i = 0; i < r_map->entries.size(); ++i) {
		index_insert(r_map, i);
	}
}

void map_append(MapCell *r_map, Value p_key, Value p_value, uint64_t p_hash) {
	r_map->entries.push_back(MapEntry{ p_key, p_value, p_hash });
	if (r_map->entries.size() <= LINEAR_MAP_LIMIT) {
		return;
	}
	if (r_map->index.empty() || r_map->entries.size() * 2 > r_map->index.size()) {
		rebuild_index(r_map);
	} else {
		index_insert(r_map, r_map->entries.size() - 1);
	}
}

// The last occurrence of a key wins, with its value and at its position (spec/values.md §8.1):
// walk backwards keeping the first sighting of each key, then reverse.
Outcome build_map(Heap &r_heap, const std::vector<Value> &p_keys, const std::vector<Value> &p_values, bool p_mutable, Value &r_result) {
	if (p_keys.size() != p_values.size()) {
		return Outcome::Invalid;
	}
	for (Value key : p_keys) {
		if (is_unbound_placeholder(follow(key))) {
			r_result = follow(key);
			return Outcome::Park;
		}
	}
	MapCell *map = r_heap.make<MapCell>(p_mutable);
	for (size_t i = p_keys.size(); i-- > 0;) {
		const Value key = follow(p_keys[i]);
		const uint64_t hash = hash_key(key);
		if (map_find(map, key, hash) < 0) {
			map_append(map, key, p_values[i], hash);
		}
	}
	std::reverse(map->entries.begin(), map->entries.end());
	rebuild_index(map);
	r_result = Value::from_cell(map);
	return Outcome::Ok;
}

bool small_index(Value p_index, size_t p_length, size_t &r_position) {
	if (!p_index.is_int32() || p_index.as_int32() < 0 || size_t(p_index.as_int32()) >= p_length) {
		return false;
	}
	r_position = size_t(p_index.as_int32());
	return true;
}

Outcome melt_value(Heap &r_heap, Value p_value, Value &r_result) {
	p_value = follow(read_slot(p_value));
	if (is_unbound_placeholder(p_value)) {
		r_result = p_value;
		return Outcome::Park;
	}
	if (!p_value.is_cell()) {
		r_result = p_value;
		return Outcome::Ok;
	}
	const Cell *cell = p_value.as_cell();
	switch (cell->kind) {
		case CellKind::Array:
		case CellKind::MutableArray: {
			const ArrayCell *source = static_cast<const ArrayCell *>(cell);
			ArrayCell *copy = r_heap.make<ArrayCell>(true);
			if (source->storage == ArrayCell::Storage::Char8) {
				copy->storage = ArrayCell::Storage::Char8;
				copy->bytes = source->bytes;
			} else {
				copy->values.reserve(source->values.size());
				for (Value element : source->values) {
					Value melted;
					const Outcome outcome = melt_value(r_heap, element, melted);
					if (outcome != Outcome::Ok) {
						r_result = melted;
						return outcome;
					}
					copy->values.push_back(melted);
				}
			}
			r_result = Value::from_cell(copy);
			return Outcome::Ok;
		}
		case CellKind::Map:
		case CellKind::MutableMap: {
			const MapCell *source = static_cast<const MapCell *>(cell);
			MapCell *copy = r_heap.make<MapCell>(true);
			copy->entries.reserve(source->entries.size());
			for (const MapEntry &entry : source->entries) {
				Value melted;
				const Outcome outcome = melt_value(r_heap, entry.value, melted);
				if (outcome != Outcome::Ok) {
					r_result = melted;
					return outcome;
				}
				copy->entries.push_back(MapEntry{ entry.key, melted, entry.hash });
			}
			rebuild_index(copy);
			r_result = Value::from_cell(copy);
			return Outcome::Ok;
		}
		case CellKind::Option: {
			Value melted;
			const Outcome outcome = melt_value(r_heap, static_cast<const OptionCell *>(cell)->content, melted);
			if (outcome != Outcome::Ok) {
				r_result = melted;
				return outcome;
			}
			r_result = Value::from_cell(r_heap.make<OptionCell>(melted));
			return Outcome::Ok;
		}
		case CellKind::Object: {
			const ObjectCell *source = static_cast<const ObjectCell *>(cell);
			if (!source->is_struct()) {
				break;
			}
			ObjectCell *copy = r_heap.make<ObjectCell>();
			copy->object_class = source->object_class;
			copy->field_names = source->field_names;
			for (Value field : source->field_values) {
				Value melted;
				const Outcome outcome = melt_value(r_heap, field, melted);
				if (outcome != Outcome::Ok) {
					r_result = melted;
					return outcome;
				}
				copy->field_values.push_back(melted);
			}
			r_result = Value::from_cell(copy);
			return Outcome::Ok;
		}
		default:
			break;
	}
	r_result = p_value;
	return Outcome::Ok;
}

// Below the top level an already immutable array or map is answered as is: only what the `var`
// holds directly is guaranteed melted, and ArrayAdd appends without melting.
Outcome freeze_value(Heap &r_heap, Value p_value, Value &r_result) {
	p_value = follow(read_slot(p_value));
	if (is_unbound_placeholder(p_value)) {
		return Outcome::Invalid;
	}
	if (!p_value.is_cell()) {
		r_result = p_value;
		return Outcome::Ok;
	}
	const Cell *cell = p_value.as_cell();
	switch (cell->kind) {
		case CellKind::MutableArray: {
			const ArrayCell *source = static_cast<const ArrayCell *>(cell);
			ArrayCell *copy = r_heap.make<ArrayCell>(false);
			if (source->storage == ArrayCell::Storage::Char8) {
				copy->storage = ArrayCell::Storage::Char8;
				copy->bytes = source->bytes;
			} else {
				copy->values.reserve(source->values.size());
				for (Value element : source->values) {
					Value frozen;
					const Outcome outcome = freeze_value(r_heap, element, frozen);
					if (outcome != Outcome::Ok) {
						return outcome;
					}
					copy->values.push_back(frozen);
				}
			}
			r_result = Value::from_cell(copy);
			return Outcome::Ok;
		}
		case CellKind::MutableMap: {
			const MapCell *source = static_cast<const MapCell *>(cell);
			MapCell *copy = r_heap.make<MapCell>(false);
			copy->entries.reserve(source->entries.size());
			for (const MapEntry &entry : source->entries) {
				Value frozen;
				const Outcome outcome = freeze_value(r_heap, entry.value, frozen);
				if (outcome != Outcome::Ok) {
					return outcome;
				}
				copy->entries.push_back(MapEntry{ entry.key, frozen, entry.hash });
			}
			rebuild_index(copy);
			r_result = Value::from_cell(copy);
			return Outcome::Ok;
		}
		case CellKind::Option: {
			Value frozen;
			const Outcome outcome = freeze_value(r_heap, static_cast<const OptionCell *>(cell)->content, frozen);
			if (outcome != Outcome::Ok) {
				return outcome;
			}
			r_result = Value::from_cell(r_heap.make<OptionCell>(frozen));
			return Outcome::Ok;
		}
		case CellKind::Object: {
			const ObjectCell *source = static_cast<const ObjectCell *>(cell);
			if (!source->is_struct()) {
				break;
			}
			ObjectCell *copy = r_heap.make<ObjectCell>();
			copy->object_class = source->object_class;
			copy->field_names = source->field_names;
			for (Value field : source->field_values) {
				Value frozen;
				const Outcome outcome = freeze_value(r_heap, field, frozen);
				if (outcome != Outcome::Ok) {
					return outcome;
				}
				copy->field_values.push_back(frozen);
			}
			r_result = Value::from_cell(copy);
			return Outcome::Ok;
		}
		default:
			break;
	}
	r_result = p_value;
	return Outcome::Ok;
}

} // namespace

Value make_string(Heap &r_heap, std::string_view p_bytes) {
	ArrayCell *array = r_heap.make<ArrayCell>(false);
	array->storage = ArrayCell::Storage::Char8;
	array->bytes.assign(p_bytes.data(), p_bytes.size());
	return Value::from_cell(array);
}

bool string_bytes(Value p_value, std::string &r_bytes) {
	p_value = follow(p_value);
	r_bytes.clear();
	if (is_cell_kind(p_value, CellKind::False)) {
		return true;
	}
	if (!is_array(p_value)) {
		return false;
	}
	const ArrayCell *array = cell_as<ArrayCell>(p_value);
	if (array->storage == ArrayCell::Storage::Char8) {
		r_bytes = array->bytes;
		return true;
	}
	for (Value element : array->values) {
		element = follow(read_slot(element));
		if (!element.is_char8()) {
			r_bytes.clear();
			return false;
		}
		r_bytes.push_back(char(element.as_char8()));
	}
	return true;
}

std::string char32_to_utf8(uint32_t p_code_point) {
	std::string bytes;
	if (p_code_point > 0x10FFFF) {
		p_code_point = 0xFFFD;
	}
	if (p_code_point < 0x80) {
		bytes.push_back(char(p_code_point));
	} else if (p_code_point < 0x800) {
		bytes.push_back(char(0xC0 | (p_code_point >> 6)));
		bytes.push_back(char(0x80 | (p_code_point & 0x3F)));
	} else if (p_code_point < 0x10000) {
		bytes.push_back(char(0xE0 | (p_code_point >> 12)));
		bytes.push_back(char(0x80 | ((p_code_point >> 6) & 0x3F)));
		bytes.push_back(char(0x80 | (p_code_point & 0x3F)));
	} else {
		bytes.push_back(char(0xF0 | (p_code_point >> 18)));
		bytes.push_back(char(0x80 | ((p_code_point >> 12) & 0x3F)));
		bytes.push_back(char(0x80 | ((p_code_point >> 6) & 0x3F)));
		bytes.push_back(char(0x80 | (p_code_point & 0x3F)));
	}
	return bytes;
}

Outcome value_to_string(Heap &r_heap, Value p_value, Value &r_result, RuntimeError &r_error) {
	p_value = follow(p_value);
	if (is_unbound_placeholder(p_value)) {
		r_result = p_value;
		return Outcome::Park;
	}
	std::string text;
	if (is_int(p_value)) {
		const Outcome outcome = int_to_string(p_value, text, r_error);
		if (outcome != Outcome::Ok) {
			return outcome;
		}
	} else if (p_value.is_float()) {
		text = float_to_string(p_value.as_float());
	} else if (p_value.is_char8()) {
		text.push_back(char(p_value.as_char8()));
	} else if (p_value.is_char32()) {
		text = char32_to_utf8(p_value.as_char32());
	} else if (is_array(p_value) || is_cell_kind(p_value, CellKind::False)) {
		if (!string_bytes(p_value, text)) {
			return Outcome::Invalid;
		}
		r_result = p_value;
		return Outcome::Ok;
	} else {
		return Outcome::Invalid;
	}
	r_result = make_string(r_heap, text);
	return Outcome::Ok;
}

Value make_array(Heap &r_heap, const std::vector<Value> &p_elements, bool p_mutable) {
	ArrayCell *array = r_heap.make<ArrayCell>(p_mutable);
	for (Value element : p_elements) {
		array->append(element);
	}
	return Value::from_cell(array);
}

Outcome value_length(Value p_container, int64_t &r_length) {
	p_container = follow(p_container);
	if (is_unbound_placeholder(p_container)) {
		return Outcome::Park;
	}
	if (is_array(p_container)) {
		r_length = int64_t(cell_as<ArrayCell>(p_container)->length());
	} else if (is_map(p_container)) {
		r_length = int64_t(cell_as<MapCell>(p_container)->entries.size());
	} else if (is_cell_kind(p_container, CellKind::False)) {
		r_length = 0;
	} else {
		return Outcome::Invalid;
	}
	return Outcome::Ok;
}

Outcome array_index(Value p_array, Value p_index, Value &r_result) {
	p_array = follow(p_array);
	p_index = follow(p_index);
	if (is_unbound_placeholder(p_array) || is_unbound_placeholder(p_index)) {
		r_result = is_unbound_placeholder(p_array) ? p_array : p_index;
		return Outcome::Park;
	}
	if (is_cell_kind(p_array, CellKind::False)) {
		return Outcome::Fail;
	}
	if (!is_array(p_array) || !is_int(p_index)) {
		return Outcome::Invalid;
	}
	const ArrayCell *array = cell_as<ArrayCell>(p_array);
	size_t position = 0;
	if (!small_index(p_index, array->length(), position)) {
		return Outcome::Fail;
	}
	r_result = read_slot(array->get(position));
	return Outcome::Ok;
}

Outcome array_concat(Heap &r_heap, Value p_left, Value p_right, Value &r_result) {
	p_left = follow(p_left);
	p_right = follow(p_right);
	if (is_cell_kind(p_left, CellKind::False)) {
		r_result = p_right;
		return Outcome::Ok;
	}
	if (is_cell_kind(p_right, CellKind::False)) {
		r_result = p_left;
		return Outcome::Ok;
	}
	if (!is_array(p_left) || !is_array(p_right)) {
		return Outcome::Invalid;
	}
	const ArrayCell *left = cell_as<ArrayCell>(p_left);
	const ArrayCell *right = cell_as<ArrayCell>(p_right);
	ArrayCell *result = r_heap.make<ArrayCell>(false);
	if (left->storage == ArrayCell::Storage::Char8 && right->storage == ArrayCell::Storage::Char8) {
		result->storage = ArrayCell::Storage::Char8;
		result->bytes = left->bytes + right->bytes;
	} else {
		for (size_t i = 0; i < left->length(); ++i) {
			result->append(read_slot(left->get(i)));
		}
		for (size_t i = 0; i < right->length(); ++i) {
			result->append(read_slot(right->get(i)));
		}
	}
	r_result = Value::from_cell(result);
	return Outcome::Ok;
}

Outcome array_append(Value p_container, Value p_value) {
	p_container = follow(p_container);
	if (!is_cell_kind(p_container, CellKind::MutableArray)) {
		return Outcome::Invalid;
	}
	cell_as<ArrayCell>(p_container)->append(p_value);
	return Outcome::Ok;
}

Outcome array_make_immutable(Value p_container) {
	p_container = follow(p_container);
	if (!is_cell_kind(p_container, CellKind::MutableArray)) {
		return Outcome::Invalid;
	}
	p_container.as_cell()->kind = CellKind::Array;
	return Outcome::Ok;
}

Outcome array_fast_append(Heap &r_heap, Value p_left, Value p_right, Value &r_parked) {
	p_left = follow(p_left);
	p_right = follow(p_right);
	if (!is_cell_kind(p_left, CellKind::MutableArray) || !is_array(p_right)) {
		return Outcome::Invalid;
	}
	const ArrayCell *right = cell_as<ArrayCell>(p_right);
	std::vector<Value> melted(right->length());
	for (size_t i = 0; i < right->length(); ++i) {
		const Outcome outcome = melt_value(r_heap, right->get(i), melted[i]);
		if (outcome != Outcome::Ok) {
			r_parked = melted[i];
			return outcome;
		}
	}
	ArrayCell *left = cell_as<ArrayCell>(p_left);
	for (Value element : melted) {
		left->append(element);
	}
	return Outcome::Ok;
}

Outcome array_set(Value p_container, Value p_index, Value p_value, Value &r_old) {
	p_container = follow(p_container);
	p_index = follow(p_index);
	if (is_unbound_placeholder(p_index)) {
		r_old = p_index;
		return Outcome::Park;
	}
	if (!is_cell_kind(p_container, CellKind::MutableArray) || !is_int(p_index)) {
		return Outcome::Invalid;
	}
	ArrayCell *array = cell_as<ArrayCell>(p_container);
	size_t position = 0;
	if (!small_index(p_index, array->length(), position)) {
		return Outcome::Fail;
	}
	r_old = array->get(position);
	array->set(position, p_value);
	return Outcome::Ok;
}

Outcome make_map(Heap &r_heap, const std::vector<Value> &p_keys, const std::vector<Value> &p_values, Value &r_result) {
	return build_map(r_heap, p_keys, p_values, false, r_result);
}

Outcome concatenate_maps(Heap &r_heap, Value p_left, Value p_right, Value &r_result) {
	p_left = follow(p_left);
	p_right = follow(p_right);
	if (!is_map(p_left) || !is_map(p_right)) {
		return Outcome::Invalid;
	}
	std::vector<Value> keys;
	std::vector<Value> values;
	for (Value side : { p_left, p_right }) {
		for (const MapEntry &entry : cell_as<MapCell>(side)->entries) {
			keys.push_back(entry.key);
			values.push_back(read_slot(entry.value));
		}
	}
	return build_map(r_heap, keys, values, false, r_result);
}

Outcome map_lookup(Value p_map, Value p_key, Value &r_result) {
	p_map = follow(p_map);
	p_key = follow(p_key);
	if (is_unbound_placeholder(p_map) || is_unbound_placeholder(p_key)) {
		r_result = is_unbound_placeholder(p_map) ? p_map : p_key;
		return Outcome::Park;
	}
	if (!is_map(p_map)) {
		return Outcome::Invalid;
	}
	const MapCell *map = cell_as<MapCell>(p_map);
	const int64_t position = map_find(map, p_key, hash_key(p_key));
	if (position < 0) {
		return Outcome::Fail;
	}
	r_result = read_slot(map->entries[size_t(position)].value);
	return Outcome::Ok;
}

Outcome map_set(Value p_map, Value p_key, Value p_value, bool &r_inserted, Value &r_old) {
	p_map = follow(p_map);
	p_key = follow(p_key);
	if (is_unbound_placeholder(p_key)) {
		r_old = p_key;
		return Outcome::Park;
	}
	if (!is_cell_kind(p_map, CellKind::MutableMap)) {
		return Outcome::Invalid;
	}
	MapCell *map = cell_as<MapCell>(p_map);
	const uint64_t hash = hash_key(p_key);
	const int64_t position = map_find(map, p_key, hash);
	if (position >= 0) {
		r_inserted = false;
		r_old = map->entries[size_t(position)].value;
		map->entries[size_t(position)].value = p_value;
		return Outcome::Ok;
	}
	r_inserted = true;
	r_old = Value::empty();
	map_append(map, p_key, p_value, hash);
	return Outcome::Ok;
}

void map_remove_last(Value p_map) {
	MapCell *map = cell_as<MapCell>(follow(p_map));
	map->entries.pop_back();
	rebuild_index(map);
}

void map_reindex(MapCell *r_map) {
	for (MapEntry &entry : r_map->entries) {
		entry.hash = hash_key(entry.key);
	}
	rebuild_index(r_map);
}

Outcome map_key_at(Value p_map, Value p_index, Value &r_result) {
	p_map = follow(p_map);
	size_t position = 0;
	if (!is_map(p_map) || !small_index(follow(p_index), cell_as<MapCell>(p_map)->entries.size(), position)) {
		return Outcome::Invalid;
	}
	r_result = cell_as<MapCell>(p_map)->entries[position].key;
	return Outcome::Ok;
}

Outcome map_value_at(Value p_map, Value p_index, Value &r_result) {
	p_map = follow(p_map);
	size_t position = 0;
	if (!is_map(p_map) || !small_index(follow(p_index), cell_as<MapCell>(p_map)->entries.size(), position)) {
		return Outcome::Invalid;
	}
	r_result = read_slot(cell_as<MapCell>(p_map)->entries[position].value);
	return Outcome::Ok;
}

Value make_option(Heap &r_heap, Value p_content) {
	return Value::from_cell(r_heap.make<OptionCell>(p_content));
}

Outcome option_query(Heap &r_heap, Value p_option, Value &r_result) {
	p_option = follow(p_option);
	if (is_unbound_placeholder(p_option)) {
		r_result = p_option;
		return Outcome::Park;
	}
	if (is_cell_kind(p_option, CellKind::False)) {
		return Outcome::Fail;
	}
	if (is_cell_kind(p_option, CellKind::True)) {
		r_result = r_heap.false_value();
		return Outcome::Ok;
	}
	if (is_cell_kind(p_option, CellKind::Option)) {
		r_result = cell_as<OptionCell>(p_option)->content;
		return Outcome::Ok;
	}
	return Outcome::Invalid;
}

Outcome melt(Heap &r_heap, Value p_value, Value &r_result) {
	return melt_value(r_heap, p_value, r_result);
}

Outcome freeze(Heap &r_heap, Value p_value, Value &r_result) {
	const Value followed = follow(p_value);
	if (is_cell_kind(followed, CellKind::Array) || is_cell_kind(followed, CellKind::Map)) {
		return Outcome::Invalid;
	}
	return freeze_value(r_heap, followed, r_result);
}

} // namespace vm
