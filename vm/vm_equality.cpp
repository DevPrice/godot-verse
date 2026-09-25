#include "vm_equality.h"

#include <cstring>

#include "vm_cell.h"
#include "vm_number.h"
#include "vm_objects.h"

namespace vm {

namespace {

uint64_t mix(uint64_t p_value) {
	p_value ^= p_value >> 33;
	p_value *= 0xFF51AFD7ED558CCDULL;
	p_value ^= p_value >> 33;
	p_value *= 0xC4CEB9FE1A85EC53ULL;
	p_value ^= p_value >> 33;
	return p_value;
}

uint64_t combine(uint64_t p_seed, uint64_t p_value) {
	return mix(p_seed ^ (p_value + 0x9E3779B97F4A7C15ULL + (p_seed << 6) + (p_seed >> 2)));
}

constexpr uint64_t SEED_EMPTY = 0x1111;
constexpr uint64_t SEED_INT = 0x2222;
constexpr uint64_t SEED_FLOAT = 0x3333;
constexpr uint64_t SEED_CHAR8 = 0x4444;
constexpr uint64_t SEED_CHAR32 = 0x5555;
constexpr uint64_t SEED_ARRAY = 0x6666;
constexpr uint64_t SEED_MAP = 0x7777;
constexpr uint64_t SEED_OPTION = 0x8888;
constexpr uint64_t SEED_RATIONAL = 0x9999;
constexpr uint64_t SEED_STRUCT = 0xAAAA;
constexpr uint64_t SEED_SPECIAL = 0xBBBB;

bool is_array(Value p_value) {
	return is_cell_kind(p_value, CellKind::Array) || is_cell_kind(p_value, CellKind::MutableArray);
}

bool is_map(Value p_value) {
	return is_cell_kind(p_value, CellKind::Map) || is_cell_kind(p_value, CellKind::MutableMap);
}

bool is_empty_container(Value p_value) {
	if (is_array(p_value)) {
		return cell_as<ArrayCell>(p_value)->length() == 0;
	}
	if (is_map(p_value)) {
		return cell_as<MapCell>(p_value)->entries.empty();
	}
	return false;
}

bool is_logic(Value p_value) {
	return is_cell_kind(p_value, CellKind::False) || is_cell_kind(p_value, CellKind::True);
}

uint64_t hash_int(const BigInt &p_value) {
	if (p_value.fits_int64()) {
		return combine(SEED_INT, uint64_t(p_value.to_int64()));
	}
	uint64_t hash = combine(SEED_INT, p_value.negative ? 1 : 0);
	for (uint32_t limb : p_value.limbs) {
		hash = combine(hash, limb);
	}
	return hash;
}

// What p_object holds for p_name: its slot, or else its layout's constant.
bool field_value(const ObjectCell *p_object, const NameCell *p_name, Value &r_value) {
	for (size_t i = 0; i < p_object->field_names.size(); ++i) {
		if (p_object->field_names[i] == p_name) {
			r_value = read_slot(p_object->field_values[i]);
			return true;
		}
	}
	const LayoutField *field = p_object->layout != nullptr ? p_object->layout->find(p_name) : nullptr;
	if (field == nullptr || field->kind != FieldKind::Constant) {
		return false;
	}
	r_value = field->value;
	return true;
}

// spec/objects.md §10.2: field by field and by name, whether each side holds the field in a slot or
// as its class's constant. A name neither side stores is a constant of the one class both share.
Equality struct_equal(const ObjectCell *p_left, const ObjectCell *p_right, PlaceholderMeeter *p_meeter) {
	for (const ObjectCell *named : { p_left, p_right }) {
		for (const NameCell *name : named->field_names) {
			Value left;
			Value right;
			if (!field_value(p_left, name, left) || !field_value(p_right, name, right)) {
				return Equality::Neq;
			}
			const Equality field = values_equal(left, right, p_meeter);
			if (field != Equality::Eq) {
				return field;
			}
		}
	}
	return Equality::Eq;
}

// spec/values.md §11.2 rule 7: both sides are cells, none of the kinds rules 3-6 took.
Equality cell_equal(Value p_left, Value p_right, PlaceholderMeeter *p_meeter) {
	const Cell *left = p_left.as_cell();
	const Cell *right = p_right.as_cell();
	switch (left->kind) {
		case CellKind::Option:
			if (right->kind != CellKind::Option) {
				return Equality::Neq;
			}
			return values_equal(static_cast<const OptionCell *>(left)->content, static_cast<const OptionCell *>(right)->content, p_meeter);
		case CellKind::Array:
		case CellKind::MutableArray: {
			if (!is_array(p_right)) {
				return Equality::Neq;
			}
			const ArrayCell *left_array = static_cast<const ArrayCell *>(left);
			const ArrayCell *right_array = static_cast<const ArrayCell *>(right);
			if (left_array->length() != right_array->length()) {
				return Equality::Neq;
			}
			if (left_array->storage == ArrayCell::Storage::Char8 && right_array->storage == ArrayCell::Storage::Char8) {
				return left_array->bytes == right_array->bytes ? Equality::Eq : Equality::Neq;
			}
			for (size_t i = 0; i < left_array->length(); ++i) {
				const Equality element = values_equal(read_slot(left_array->get(i)), read_slot(right_array->get(i)), p_meeter);
				if (element != Equality::Eq) {
					return element;
				}
			}
			return Equality::Eq;
		}
		case CellKind::Map:
		case CellKind::MutableMap: {
			if (!is_map(p_right)) {
				return Equality::Neq;
			}
			const MapCell *left_map = static_cast<const MapCell *>(left);
			const MapCell *right_map = static_cast<const MapCell *>(right);
			if (left_map->entries.size() != right_map->entries.size()) {
				return Equality::Neq;
			}
			for (size_t i = 0; i < left_map->entries.size(); ++i) {
				Equality answer = values_equal(left_map->entries[i].key, right_map->entries[i].key, p_meeter);
				if (answer != Equality::Eq) {
					return answer;
				}
				answer = values_equal(read_slot(left_map->entries[i].value), read_slot(right_map->entries[i].value), p_meeter);
				if (answer != Equality::Eq) {
					return answer;
				}
			}
			return Equality::Eq;
		}
		case CellKind::Rational: {
			if (right->kind != CellKind::Rational) {
				return Equality::Neq;
			}
			const RationalCell *left_rational = static_cast<const RationalCell *>(left);
			const RationalCell *right_rational = static_cast<const RationalCell *>(right);
			const bool equal = big_compare(left_rational->numerator, right_rational->numerator) == 0 &&
					big_compare(left_rational->denominator, right_rational->denominator) == 0;
			return equal ? Equality::Eq : Equality::Neq;
		}
		case CellKind::Object: {
			const ObjectCell *left_object = static_cast<const ObjectCell *>(left);
			if (right->kind != CellKind::Object) {
				return Equality::Neq;
			}
			const ObjectCell *right_object = static_cast<const ObjectCell *>(right);
			if (left_object->object_class != right_object->object_class) {
				return Equality::Neq;
			}
			if (left_object->is_struct()) {
				return struct_equal(left_object, right_object, p_meeter);
			}
			if (left_object->object_class != nullptr && (left_object->object_class->flags & ClassCell::FLAG_UNIQUE) != 0) {
				return Equality::Neq;
			}
			return Equality::Undecidable;
		}
		case CellKind::Task:
		case CellKind::Semaphore:
			return Equality::Neq;
		default:
			return Equality::Undecidable;
	}
}

} // namespace

Equality values_equal(Value p_left, Value p_right, PlaceholderMeeter *p_meeter) {
	p_left = follow(p_left);
	p_right = follow(p_right);
	if (is_unbound_placeholder(p_left) || is_unbound_placeholder(p_right)) {
		if (p_left.same(p_right)) {
			return Equality::Eq;
		}
		return p_meeter != nullptr ? p_meeter->meet(p_left, p_right) : Equality::Eq;
	}
	if (p_left.same(p_right)) {
		return Equality::Eq;
	}
	if (p_left.is_float() && p_right.is_float()) {
		// Every NaN is the one canonical pattern, so two NaNs were already the same value.
		return p_left.as_float() == p_right.as_float() ? Equality::Eq : Equality::Neq;
	}
	if (is_int(p_left) || is_int(p_right)) {
		const Value integer = is_int(p_left) ? p_left : p_right;
		const Value other = is_int(p_left) ? p_right : p_left;
		if (is_int(other)) {
			if (integer.is_int32() || other.is_int32()) {
				return Equality::Neq;
			}
			const HeapIntCell *left_int = cell_as<HeapIntCell>(integer);
			const HeapIntCell *right_int = cell_as<HeapIntCell>(other);
			if (!left_int->is_wide || !right_int->is_wide) {
				return !left_int->is_wide && !right_int->is_wide && left_int->narrow == right_int->narrow ? Equality::Eq : Equality::Neq;
			}
			return big_compare(left_int->wide, right_int->wide) == 0 ? Equality::Eq : Equality::Neq;
		}
		if (is_rational(other)) {
			const RationalCell *rational = cell_as<RationalCell>(other);
			const bool whole = rational->denominator.limbs.size() == 1 && rational->denominator.limbs[0] == 1;
			return whole && big_compare(rational->numerator, int_value(integer)) == 0 ? Equality::Eq : Equality::Neq;
		}
		return Equality::Neq;
	}
	if (is_empty_container(p_left) || is_empty_container(p_right)) {
		const bool left_empty = is_empty_container(p_left) || is_cell_kind(p_left, CellKind::False);
		const bool right_empty = is_empty_container(p_right) || is_cell_kind(p_right, CellKind::False);
		return left_empty && right_empty ? Equality::Eq : Equality::Neq;
	}
	if (is_logic(p_left) || is_logic(p_right)) {
		return Equality::Neq;
	}
	if (is_cell_kind(p_left, CellKind::Enumerator) || is_cell_kind(p_right, CellKind::Enumerator)) {
		return Equality::Neq;
	}
	if (p_left.is_cell() && p_right.is_cell()) {
		return cell_equal(p_left, p_right, p_meeter);
	}
	return Equality::Neq;
}

uint64_t hash_key(Value p_value) {
	p_value = follow(read_slot(p_value));
	if (p_value.is_float()) {
		double number = p_value.as_float();
		if (number == 0.0) {
			number = 0.0;
		}
		uint64_t raw;
		std::memcpy(&raw, &number, sizeof(raw));
		return combine(SEED_FLOAT, raw);
	}
	if (p_value.is_int32()) {
		return combine(SEED_INT, uint64_t(int64_t(p_value.as_int32())));
	}
	if (p_value.is_char8()) {
		return combine(SEED_CHAR8, p_value.as_char8());
	}
	if (p_value.is_char32()) {
		return combine(SEED_CHAR32, p_value.as_char32());
	}
	if (!p_value.is_cell()) {
		return combine(SEED_SPECIAL, p_value.bits);
	}
	if (is_empty_container(p_value) || is_cell_kind(p_value, CellKind::False)) {
		return mix(SEED_EMPTY);
	}
	const Cell *cell = p_value.as_cell();
	switch (cell->kind) {
		case CellKind::HeapInt: {
			const HeapIntCell *integer = static_cast<const HeapIntCell *>(cell);
			return integer->is_wide ? hash_int(integer->wide) : combine(SEED_INT, uint64_t(integer->narrow));
		}
		case CellKind::Rational: {
			const RationalCell *rational = static_cast<const RationalCell *>(cell);
			if (rational->denominator.limbs.size() == 1 && rational->denominator.limbs[0] == 1) {
				return hash_int(rational->numerator);
			}
			return combine(combine(SEED_RATIONAL, hash_int(rational->numerator)), hash_int(rational->denominator));
		}
		case CellKind::Array:
		case CellKind::MutableArray: {
			const ArrayCell *array = static_cast<const ArrayCell *>(cell);
			uint64_t hash = SEED_ARRAY;
			for (size_t i = 0; i < array->length(); ++i) {
				hash = combine(hash, hash_key(array->get(i)));
			}
			return hash;
		}
		case CellKind::Map:
		case CellKind::MutableMap: {
			uint64_t hash = SEED_MAP;
			for (const MapEntry &entry : static_cast<const MapCell *>(cell)->entries) {
				hash = combine(combine(hash, hash_key(entry.key)), hash_key(entry.value));
			}
			return hash;
		}
		case CellKind::Option:
			return combine(SEED_OPTION, hash_key(static_cast<const OptionCell *>(cell)->content));
		case CellKind::Object: {
			const ObjectCell *object = static_cast<const ObjectCell *>(cell);
			if (!object->is_struct()) {
				break;
			}
			// A sum, so field order -- which differs between layouts of one struct -- does not matter.
			uint64_t fields = 0;
			for (size_t i = 0; i < object->field_names.size(); ++i) {
				fields += combine(uint64_t(reinterpret_cast<uintptr_t>(object->field_names[i])), hash_key(object->field_values[i]));
			}
			return combine(combine(SEED_STRUCT, uint64_t(reinterpret_cast<uintptr_t>(object->object_class))), fields);
		}
		default:
			break;
	}
	return mix(p_value.bits);
}

} // namespace vm
