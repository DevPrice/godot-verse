#include "vm_loader.h"

#include <cstring>
#include <deque>

#include "vbc_ops.gen.h"
#include "vbc_reader.h"
#include "vm_natives.h"
#include "vm_number.h"
#include "vm_values.h"

namespace vm {

namespace {

using vbc::kVbcOpCount;
using vbc::kVbcOps;
using vbc::VbcOp;
using vbc::VbcOperandDesc;
using vbc::VbcOperandKind;

constexpr uint8_t kEndMarker = 0xE5;
constexpr uint64_t kFormatVersion = 1;

constexpr uint64_t kind_bit(CellKind p_kind) {
	return uint64_t(1) << uint8_t(p_kind);
}

constexpr uint64_t kAnyKind = 0;
constexpr uint64_t kPackageKinds = kind_bit(CellKind::Package) | kind_bit(CellKind::BuiltinPackage);
constexpr uint64_t kCalleeKinds = kind_bit(CellKind::Procedure) | kind_bit(CellKind::NativeProcedure);
constexpr uint64_t kArrayKinds = kind_bit(CellKind::Array) | kind_bit(CellKind::MutableArray);

bool is_inline_cache(VbcOp p_op) {
	switch (p_op) {
		case VbcOp::NewObjectICClass:
		case VbcOp::LoadFieldICOffset:
		case VbcOp::LoadFieldICConstant:
		case VbcOp::LoadFieldICFunction:
		case VbcOp::LoadFieldICNativeFunction:
		case VbcOp::LoadFieldICAccessor:
		case VbcOp::CreateFieldICValueObjectConstant:
		case VbcOp::CreateFieldICValueObjectField:
		case VbcOp::CreateFieldICNativeStruct:
		case VbcOp::CreateFieldICUObject:
			return true;
		default:
			return false;
	}
}

// format.md §9: never emitted, and each computes a value whose convention is known only from
// source, so a guess would be silently wrong (spec/ops.md §13, §9.10).
bool is_refused_op(VbcOp p_op) {
	switch (p_op) {
		case VbcOp::Mod:
		case VbcOp::MutableAdd:
		case VbcOp::NewMutableArrayWithCapacity:
		case VbcOp::NewUnionVariant:
		case VbcOp::GetUnionVariantPayload:
		case VbcOp::GetUnionVariantTag:
			return true;
		default:
			return false;
	}
}

// A slot to write once every cell exists. `kinds` is the set of cell kinds the slot accepts, or
// kAnyKind.
struct Fixup {
	void *slot = nullptr;
	uint32_t index = 0;
	uint64_t kinds = kAnyKind;
	void (*apply)(void *p_slot, Cell *p_cell, Value p_value) = nullptr;
};

template <typename T>
void apply_pointer(void *p_slot, Cell *p_cell, Value) {
	*static_cast<const T **>(p_slot) = static_cast<const T *>(p_cell);
}

void apply_value(void *p_slot, Cell *, Value p_value) {
	*static_cast<Value *>(p_slot) = p_value;
}

// A value read before its cell may exist: either the value itself or the cell index it names.
struct PendingValue {
	Value value = Value::uninitialized();
	uint32_t cell = 0;
	bool is_cell = false;
	uint64_t kinds = kAnyKind;
};

struct PendingRational {
	RationalCell *cell = nullptr;
	Value numerator;
	Value denominator;
};

class Loader {
public:
	Loader(Heap &r_heap, const uint8_t *p_data, size_t p_size, const std::string &p_path, Program &r_program, std::string &r_error) :
			heap(r_heap), reader(vbc_reader_make(p_data, p_size)), path(p_path), program(r_program), error(r_error) {}

	bool run() {
		return read_header() && read_strings() && read_cells() && read_packages() && read_well_known() &&
				read_class_index() && read_end() && resolve() && bind_natives() && supply_builtin_package() && finish();
	}

private:
	Heap &heap;
	VbcReader reader;
	const std::string &path;
	Program &program;
	std::string &error;

	std::vector<std::string> strings;
	std::vector<const NameCell *> names;
	std::vector<Cell *> cells;
	std::vector<Fixup> fixups;
	std::deque<PendingRational> rationals;
	std::vector<MapCell *> maps;
	std::vector<NativeProcedureCell *> natives;
	PackageCell *builtin_package = nullptr;

	struct WellKnown {
		uint64_t role = 0;
		uint32_t cell = 0;
	};
	std::vector<WellKnown> well_known;
	std::vector<uint32_t> package_cells;
	struct RawClassEntry {
		uint8_t origin = 0;
		uint64_t name = 0;
		uint32_t cell = 0;
	};
	std::vector<RawClassEntry> raw_classes;

	bool refuse(const std::string &p_sentence) {
		if (error.empty()) {
			error = path + " " + p_sentence;
		}
		return false;
	}

	bool malformed(const std::string &p_what) {
		return refuse("is malformed: " + p_what + ".");
	}

	// Every reader failure is a truncation or a runaway varint; either way the file is refused.
	bool check_reader() {
		if (reader.ok()) {
			return true;
		}
		return refuse("is truncated or malformed: " + reader.error + ".");
	}

	size_t remaining() const { return reader.size - reader.offset; }

	// A list count, refused when the file cannot possibly hold that many entries of at least one
	// byte each, so a corrupt count is a sentence rather than an allocation failure.
	bool read_count(size_t &r_count) {
		const uint64_t count = vbc_read_uv(reader);
		if (!check_reader()) {
			return false;
		}
		if (count > remaining()) {
			return malformed("a list claims " + std::to_string(count) + " entries at offset " + std::to_string(reader.offset) + ", more than the file holds");
		}
		r_count = size_t(count);
		return true;
	}

	bool read_u32(uint32_t &r_value) {
		const uint64_t value = vbc_read_uv(reader);
		if (!check_reader()) {
			return false;
		}
		if (value > UINT32_MAX) {
			return malformed("a number " + std::to_string(value) + " at offset " + std::to_string(reader.offset) + " is out of range");
		}
		r_value = uint32_t(value);
		return true;
	}

	bool read_sid(uint64_t &r_sid) {
		r_sid = vbc_read_uv(reader);
		if (!check_reader()) {
			return false;
		}
		if (r_sid >= strings.size()) {
			return malformed("a string index " + std::to_string(r_sid) + " at offset " + std::to_string(reader.offset) + " is out of range");
		}
		return true;
	}

	bool read_string(std::string &r_text) {
		uint64_t sid;
		if (!read_sid(sid)) {
			return false;
		}
		r_text = strings[size_t(sid)];
		return true;
	}

	const NameCell *name_for(uint64_t p_sid) {
		const NameCell *&name = names[size_t(p_sid)];
		if (name == nullptr) {
			name = heap.intern(strings[size_t(p_sid)]);
		}
		return name;
	}

	bool read_name(const NameCell *&r_name) {
		uint64_t sid;
		if (!read_sid(sid)) {
			return false;
		}
		r_name = name_for(sid);
		return true;
	}

	// A ref: 0 for none, otherwise a cell index plus one.
	bool read_ref(uint32_t &r_ref) {
		const uint64_t ref = vbc_read_uv(reader);
		if (!check_reader()) {
			return false;
		}
		if (ref > cells.size()) {
			return malformed("a cell reference " + std::to_string(ref) + " at offset " + std::to_string(reader.offset) + " is out of range (" + std::to_string(cells.size()) + " cells)");
		}
		r_ref = uint32_t(ref);
		return true;
	}

	template <typename T>
	bool read_pointer(const T *&r_slot, uint64_t p_kinds) {
		uint32_t ref;
		if (!read_ref(ref)) {
			return false;
		}
		r_slot = nullptr;
		if (ref != 0) {
			fixups.push_back(Fixup{ &r_slot, ref - 1, p_kinds, &apply_pointer<T> });
		}
		return true;
	}

	void store(const PendingValue &p_pending, Value &r_slot) {
		if (p_pending.is_cell) {
			fixups.push_back(Fixup{ &r_slot, p_pending.cell, p_pending.kinds, &apply_value });
		} else {
			r_slot = p_pending.value;
		}
	}

	// format.md §3.
	bool read_pending_value(PendingValue &r_value) {
		const uint8_t tag = vbc_read_u8(reader);
		if (!check_reader()) {
			return false;
		}
		r_value = PendingValue();
		switch (tag) {
			case 0:
				return true;
			case 1: {
				const int64_t number = vbc_read_sv(reader);
				if (!check_reader()) {
					return false;
				}
				if (number < INT32_MIN || number > INT32_MAX) {
					return malformed("an int32 value " + std::to_string(number) + " at offset " + std::to_string(reader.offset) + " is out of range");
				}
				r_value.value = Value::from_int32(int32_t(number));
				return true;
			}
			case 2:
				r_value.value = Value::from_float(vbc_read_f64(reader));
				return check_reader();
			case 3:
				r_value.value = Value::from_char8(vbc_read_u8(reader));
				return check_reader();
			case 4: {
				uint32_t code_point;
				if (!read_u32(code_point)) {
					return false;
				}
				r_value.value = Value::from_char32(code_point);
				return true;
			}
			case 5: {
				uint32_t ref;
				if (!read_ref(ref)) {
					return false;
				}
				if (ref == 0) {
					return malformed("a cell value at offset " + std::to_string(reader.offset) + " names no cell");
				}
				r_value.is_cell = true;
				r_value.cell = ref - 1;
				return true;
			}
			default:
				return malformed("a value tag " + std::to_string(tag) + " at offset " + std::to_string(reader.offset) + " is unknown");
		}
	}

	bool read_value(Value &r_slot) {
		PendingValue pending;
		if (!read_pending_value(pending)) {
			return false;
		}
		store(pending, r_slot);
		return true;
	}

	bool read_values(std::vector<Value> &r_values) {
		size_t count;
		if (!read_count(count)) {
			return false;
		}
		r_values.resize(count);
		for (Value &value : r_values) {
			if (!read_value(value)) {
				return false;
			}
		}
		return true;
	}

	bool read_header() {
		const uint8_t magic[4] = { vbc_read_u8(reader), vbc_read_u8(reader), vbc_read_u8(reader), vbc_read_u8(reader) };
		if (!reader.ok() || std::memcmp(magic, "VBC1", 4) != 0) {
			error.clear();
			return refuse("is not a Verse program: it does not begin with VBC1.");
		}
		const uint64_t version = vbc_read_uv(reader);
		if (!check_reader()) {
			return false;
		}
		if (version != kFormatVersion) {
			return refuse("is format version " + std::to_string(version) + "; this runtime reads version " + std::to_string(kFormatVersion) + ". Export the project again.");
		}
		program.abi = vbc_read_uv(reader);
		program.host_id = vbc_read_str(reader);
		program.engine_commit = vbc_read_str(reader);
		const std::string digest = vbc_read_str(reader);
		program.generation = vbc_read_uv(reader);
		if (!check_reader()) {
			return false;
		}
		if (digest != vbc::kVbcOpsSchemaDigest) {
			return refuse("was written for another Verse op set (schema " + digest + "); this runtime reads schema " + vbc::kVbcOpsSchemaDigest + ". Export the project again with a matching build.");
		}
		return true;
	}

	bool read_strings() {
		size_t count;
		if (!read_count(count)) {
			return false;
		}
		strings.resize(count);
		for (std::string &text : strings) {
			text = vbc_read_str(reader);
			if (!check_reader()) {
				return false;
			}
		}
		names.assign(count, nullptr);
		return true;
	}

	bool read_cells() {
		size_t count;
		if (!read_count(count)) {
			return false;
		}
		cells.assign(count, nullptr);
		for (size_t index = 0; index < count; ++index) {
			if (!read_cell(index)) {
				return false;
			}
		}
		program.cell_count = count;
		return true;
	}

	bool read_array(bool p_mutable, size_t p_index) {
		ArrayCell *array = heap.make<ArrayCell>(p_mutable);
		cells[p_index] = array;
		const uint8_t element_kind = vbc_read_u8(reader);
		if (!check_reader()) {
			return false;
		}
		size_t count;
		switch (element_kind) {
			case 0:
			case 1:
				return read_values(array->values);
			case 2:
				if (!read_count(count)) {
					return false;
				}
				array->values.resize(count);
				for (Value &value : array->values) {
					const int64_t number = vbc_read_sv(reader);
					if (!check_reader()) {
						return false;
					}
					if (number < INT32_MIN || number > INT32_MAX) {
						return malformed("an int32 array element at offset " + std::to_string(reader.offset) + " is out of range");
					}
					value = Value::from_int32(int32_t(number));
				}
				return true;
			case 3:
				array->storage = ArrayCell::Storage::Char8;
				array->bytes = vbc_read_str(reader);
				return check_reader();
			case 4:
				if (!read_count(count)) {
					return false;
				}
				array->values.resize(count);
				for (Value &value : array->values) {
					uint32_t code_point;
					if (!read_u32(code_point)) {
						return false;
					}
					value = Value::from_char32(code_point);
				}
				return true;
			default:
				return malformed("array cell " + std::to_string(p_index) + " has an unknown element kind " + std::to_string(element_kind));
		}
	}

	bool read_map(bool p_mutable, size_t p_index) {
		MapCell *map = heap.make<MapCell>(p_mutable);
		cells[p_index] = map;
		maps.push_back(map);
		size_t count;
		if (!read_count(count)) {
			return false;
		}
		map->entries.resize(count);
		for (MapEntry &entry : map->entries) {
			if (!read_value(entry.key) || !read_value(entry.value)) {
				return false;
			}
		}
		return true;
	}

	bool read_heap_int(size_t p_index) {
		const uint8_t sign = vbc_read_u8(reader);
		size_t count;
		if (!check_reader() || !read_count(count)) {
			return false;
		}
		BigInt value;
		value.limbs.assign((count + 3) / 4, 0);
		for (size_t byte = 0; byte < count; ++byte) {
			value.limbs[byte / 4] |= uint32_t(vbc_read_u8(reader)) << (8 * (byte % 4));
		}
		if (!check_reader()) {
			return false;
		}
		value.negative = sign != 0;
		value.trim();
		if (value.is_zero()) {
			value.negative = false;
		}
		cells[p_index] = heap.make<HeapIntCell>(std::move(value));
		return true;
	}

	bool read_class(size_t p_index) {
		ClassCell *class_cell = heap.make<ClassCell>();
		cells[p_index] = class_cell;
		const uint8_t kind = vbc_read_u8(reader);
		if (!check_reader()) {
			return false;
		}
		if (kind > 2) {
			return malformed("class cell " + std::to_string(p_index) + " has an unknown class kind " + std::to_string(kind));
		}
		class_cell->class_kind = ClassKind(kind);
		if (!read_u32(class_cell->flags) || !read_pointer(class_cell->package, kPackageKinds) ||
				!read_string(class_cell->relative_path) || !read_string(class_cell->base_name)) {
			return false;
		}
		class_cell->has_attributes = vbc_read_u8(reader) != 0;
		if (!check_reader()) {
			return false;
		}
		if (class_cell->has_attributes) {
			size_t count;
			if (!read_values(class_cell->attributes) || !read_count(count)) {
				return false;
			}
			class_cell->attribute_indices.resize(count);
			for (uint32_t &attribute_index : class_cell->attribute_indices) {
				if (!read_u32(attribute_index)) {
					return false;
				}
			}
		}
		size_t count;
		if (!read_count(count)) {
			return false;
		}
		class_cell->inherited.resize(count);
		for (const ClassCell *&base : class_cell->inherited) {
			if (!read_pointer(base, kind_bit(CellKind::Class))) {
				return false;
			}
		}
		if (!read_pointer(class_cell->archetype, kind_bit(CellKind::Archetype)) ||
				!read_pointer(class_cell->constructor, kind_bit(CellKind::Function)) ||
				!read_pointer(class_cell->blocks, kind_bit(CellKind::Function))) {
			return false;
		}
		class_cell->native_bound = vbc_read_u8(reader) != 0;
		return check_reader();
	}

	bool read_archetype(size_t p_index) {
		ArchetypeCell *archetype = heap.make<ArchetypeCell>();
		cells[p_index] = archetype;
		size_t count;
		if (!read_pointer(archetype->owner, kind_bit(CellKind::Class)) || !read_pointer(archetype->next, kind_bit(CellKind::Archetype)) ||
				!read_count(count)) {
			return false;
		}
		archetype->entries.resize(count);
		for (ArchetypeEntry &entry : archetype->entries) {
			if (!read_name(entry.name) || !read_pointer(entry.access, kind_bit(CellKind::AccessSpecifier)) ||
					!read_value(entry.type) || !read_value(entry.value)) {
				return false;
			}
			entry.flags = vbc_read_u8(reader);
			if (!check_reader()) {
				return false;
			}
		}
		return true;
	}

	bool read_package(size_t p_index) {
		PackageCell *package = heap.make<PackageCell>();
		cells[p_index] = package;
		size_t count;
		if (!read_name(package->name) || !read_name(package->root_path) || !read_count(count)) {
			return false;
		}
		package->definitions.resize(count);
		for (PackageDefinition &definition : package->definitions) {
			if (!read_name(definition.path) || !read_value(definition.value)) {
				return false;
			}
		}
		return true;
	}

	// One operand's word (vm_cell.h, ProcedureCell). p_constants collects the value_imm, cell and
	// asset-path operands, which become constants after the file's own.
	bool read_operand(VbcOperandKind p_kind, ProcedureCell *p_procedure, size_t p_file_constants, size_t p_op_count,
			std::vector<PendingValue> &r_constants, uint32_t &r_word) {
		const auto constant_word = [&](const PendingValue &p_pending) {
			r_constants.push_back(p_pending);
			r_word = uint32_t(r_constants.size() - 1);
		};
		switch (p_kind) {
			case VbcOperandKind::Register: {
				if (!read_u32(r_word)) {
					return false;
				}
				if (r_word >= p_procedure->register_count) {
					return malformed("register " + std::to_string(r_word) + " is out of range in " + p_procedure->name->text);
				}
				return true;
			}
			case VbcOperandKind::Value: {
				const uint64_t encoded = vbc_read_uv(reader);
				if (!check_reader()) {
					return false;
				}
				if (encoded == 0) {
					r_word = kAbsentOperand;
					return true;
				}
				const uint64_t index = (encoded - 1) >> 1;
				const bool is_constant = ((encoded - 1) & 1) != 0;
				if (is_constant ? index >= p_file_constants : index >= p_procedure->register_count) {
					return malformed(std::string(is_constant ? "constant " : "register ") + std::to_string(index) + " is out of range in " + p_procedure->name->text);
				}
				r_word = uint32_t(index << 1) | (is_constant ? 1u : 0u);
				return true;
			}
			case VbcOperandKind::ValueImm: {
				PendingValue pending;
				if (!read_pending_value(pending)) {
					return false;
				}
				constant_word(pending);
				return true;
			}
			case VbcOperandKind::CellVUniqueString:
			case VbcOperandKind::CellVPackage:
			case VbcOperandKind::CellVArray:
			case VbcOperandKind::CellVArchetype:
			case VbcOperandKind::CellVProcedure: {
				uint32_t ref;
				if (!read_ref(ref)) {
					return false;
				}
				if (ref == 0) {
					r_word = kAbsentOperand;
					return true;
				}
				PendingValue pending;
				pending.is_cell = true;
				pending.cell = ref - 1;
				pending.kinds = p_kind == VbcOperandKind::CellVUniqueString ? kind_bit(CellKind::Name)
						: p_kind == VbcOperandKind::CellVPackage			   ? kPackageKinds
						: p_kind == VbcOperandKind::CellVArray				   ? kArrayKinds
						: p_kind == VbcOperandKind::CellVArchetype			   ? kind_bit(CellKind::Archetype)
																			   : kind_bit(CellKind::Procedure);
				constant_word(pending);
				return true;
			}
			case VbcOperandKind::Label: {
				if (!read_u32(r_word)) {
					return false;
				}
				if (r_word >= p_op_count) {
					return malformed("a label " + std::to_string(r_word) + " is out of range in " + p_procedure->name->text);
				}
				return true;
			}
			case VbcOperandKind::Bool:
				r_word = vbc_read_u8(reader);
				return check_reader();
			case VbcOperandKind::I32: {
				const int64_t number = vbc_read_sv(reader);
				if (!check_reader()) {
					return false;
				}
				if (number < INT32_MIN || number > INT32_MAX) {
					return malformed("an i32 operand is out of range in " + p_procedure->name->text);
				}
				r_word = uint32_t(int32_t(number));
				return true;
			}
			case VbcOperandKind::U32:
			case VbcOperandKind::FailureContextId:
			case VbcOperandKind::EnumClassKind:
			case VbcOperandKind::EnumClassFlags:
				return read_u32(r_word);
			case VbcOperandKind::LiveRange: {
				uint32_t first;
				uint32_t last;
				if (!read_u32(first) || !read_u32(last)) {
					return false;
				}
				if (first > p_op_count || last > p_op_count) {
					return malformed("a live range is out of range in " + p_procedure->name->text);
				}
				r_word = uint32_t(p_procedure->operand_words.size());
				p_procedure->operand_words.push_back(first);
				p_procedure->operand_words.push_back(last);
				return true;
			}
			case VbcOperandKind::AssetPath: {
				uint64_t package_sid;
				uint64_t asset_sid;
				if (!read_sid(package_sid) || !read_sid(asset_sid)) {
					return false;
				}
				r_word = uint32_t(p_procedure->operand_words.size());
				PendingValue pending;
				pending.value = Value::from_cell(name_for(package_sid));
				r_constants.push_back(pending);
				p_procedure->operand_words.push_back(uint32_t(r_constants.size() - 1));
				pending.value = Value::from_cell(name_for(asset_sid));
				r_constants.push_back(pending);
				p_procedure->operand_words.push_back(uint32_t(r_constants.size() - 1));
				return true;
			}
			case VbcOperandKind::U64:
			case VbcOperandKind::ClassRefIc:
				break;
		}
		return malformed("an operand of a cache-only kind is serialized in " + p_procedure->name->text);
	}

	bool read_op(ProcedureCell *p_procedure, size_t p_file_constants, size_t p_op_count, std::vector<PendingValue> &r_constants) {
		const uint64_t opcode = vbc_read_uv(reader);
		if (!check_reader()) {
			return false;
		}
		if (opcode >= uint64_t(kVbcOpCount)) {
			return malformed("procedure " + p_procedure->name->text + " holds opcode " + std::to_string(opcode) + ", which is out of range");
		}
		const VbcOp op = VbcOp(opcode);
		const vbc::VbcOpDesc &desc = kVbcOps[opcode];
		if (is_inline_cache(op)) {
			return malformed("procedure " + p_procedure->name->text + " holds the inline-cache op " + desc.name + ", which is never serialized");
		}
		if (is_refused_op(op)) {
			return refuse("uses the op " + std::string(desc.name) + " in " + p_procedure->name->text + ", which this runtime does not implement.");
		}

		size_t slots = 0;
		for (int32_t operand = 0; operand < desc.operand_count; ++operand) {
			slots += desc.operands[operand].cache ? 0 : 1;
		}
		std::vector<uint32_t> &words = p_procedure->operand_words;
		const size_t base = words.size();
		p_procedure->ops.push_back(DecodedOp{ uint16_t(opcode), uint32_t(base) });
		words.resize(base + slots, kAbsentOperand);

		size_t slot = 0;
		for (int32_t operand = 0; operand < desc.operand_count; ++operand) {
			const VbcOperandDesc &schema = desc.operands[operand];
			if (schema.cache) {
				continue;
			}
			uint32_t word = kAbsentOperand;
			if (schema.variadic) {
				size_t count;
				if (!read_count(count)) {
					return false;
				}
				word = uint32_t(words.size());
				words.push_back(uint32_t(count));
				const size_t first = words.size();
				words.resize(first + count);
				for (size_t item = 0; item < count; ++item) {
					uint32_t item_word;
					if (schema.kind == VbcOperandKind::LiveRange || schema.kind == VbcOperandKind::AssetPath) {
						return malformed("a variadic operand of a two-word kind in " + p_procedure->name->text);
					}
					if (!read_operand(schema.kind, p_procedure, p_file_constants, p_op_count, r_constants, item_word)) {
						return false;
					}
					words[first + item] = item_word;
				}
			} else if (schema.optional) {
				const uint8_t present = vbc_read_u8(reader);
				if (!check_reader()) {
					return false;
				}
				if (present != 0 && !read_operand(schema.kind, p_procedure, p_file_constants, p_op_count, r_constants, word)) {
					return false;
				}
			} else if (!read_operand(schema.kind, p_procedure, p_file_constants, p_op_count, r_constants, word)) {
				return false;
			}
			words[base + slot++] = word;
		}
		return true;
	}

	bool read_procedure(size_t p_index) {
		ProcedureCell *procedure = heap.make<ProcedureCell>();
		cells[p_index] = procedure;
		++program.procedure_count;
		if (!read_name(procedure->name) || !read_string(procedure->file) || !read_u32(procedure->flags) ||
				!read_u32(procedure->register_count) || !read_u32(procedure->positional_count)) {
			return false;
		}
		size_t count;
		if (!read_count(count)) {
			return false;
		}
		procedure->named_parameters.resize(count);
		for (NamedParameter &parameter : procedure->named_parameters) {
			if (!read_name(parameter.name) || !read_u32(parameter.register_index)) {
				return false;
			}
			if (parameter.register_index >= procedure->register_count) {
				return malformed("a named parameter's register is out of range in " + procedure->name->text);
			}
		}

		if (!read_count(count)) {
			return false;
		}
		std::vector<PendingValue> constants(count);
		for (PendingValue &constant : constants) {
			if (!read_pending_value(constant)) {
				return false;
			}
		}
		const size_t file_constants = count;

		size_t op_count;
		if (!read_count(op_count)) {
			return false;
		}
		procedure->ops.reserve(op_count);
		for (size_t op = 0; op < op_count; ++op) {
			if (!read_op(procedure, file_constants, op_count, constants)) {
				return false;
			}
		}
		program.op_count += op_count;

		procedure->constants.resize(constants.size());
		for (size_t constant = 0; constant < constants.size(); ++constant) {
			store(constants[constant], procedure->constants[constant]);
		}

		if (!read_count(count)) {
			return false;
		}
		procedure->unwind_edges.resize(count);
		for (UnwindEdge &edge : procedure->unwind_edges) {
			if (!read_u32(edge.first_op) || !read_u32(edge.last_op) || !read_u32(edge.landing_op)) {
				return false;
			}
			if (edge.first_op > edge.last_op || edge.last_op >= op_count || edge.landing_op >= op_count) {
				return malformed("an unwind edge is out of range in " + procedure->name->text);
			}
		}
		if (!read_count(count)) {
			return false;
		}
		procedure->lines.resize(count);
		for (LineEntry &line : procedure->lines) {
			if (!read_u32(line.op) || !read_u32(line.line)) {
				return false;
			}
			if (line.op >= op_count) {
				return malformed("a location is out of range in " + procedure->name->text);
			}
		}
		if (!read_count(count)) {
			return false;
		}
		procedure->register_names.resize(count);
		for (RegisterName &register_name : procedure->register_names) {
			if (!read_u32(register_name.register_index) || !read_name(register_name.name) ||
					!read_u32(register_name.first_op) || !read_u32(register_name.last_op)) {
				return false;
			}
			if (register_name.register_index >= procedure->register_count) {
				return malformed("a register name is out of range in " + procedure->name->text);
			}
		}
		return true;
	}

	bool read_cell(size_t p_index) {
		const uint8_t kind = vbc_read_u8(reader);
		if (!check_reader()) {
			return false;
		}
		size_t count;
		switch (kind) {
			case 1:
				cells[p_index] = heap.false_value().as_cell();
				return true;
			case 2:
				cells[p_index] = heap.true_value().as_cell();
				return true;
			case 3:
				if (builtin_package == nullptr) {
					builtin_package = heap.make<PackageCell>(true);
				}
				cells[p_index] = builtin_package;
				return true;
			case 4: {
				const NameCell *name;
				if (!read_name(name)) {
					return false;
				}
				cells[p_index] = const_cast<NameCell *>(name);
				return true;
			}
			case 5:
			case 6:
				return read_array(kind == 6, p_index);
			case 7:
			case 8:
				return read_map(kind == 8, p_index);
			case 9: {
				OptionCell *option = heap.make<OptionCell>(Value::uninitialized());
				cells[p_index] = option;
				return read_value(option->content);
			}
			case 10:
				return read_heap_int(p_index);
			case 11: {
				RationalCell *rational = heap.make<RationalCell>(BigInt(), BigInt::from_int64(1));
				cells[p_index] = rational;
				rationals.push_back(PendingRational{ rational, Value::uninitialized(), Value::uninitialized() });
				return read_value(rationals.back().numerator) && read_value(rationals.back().denominator);
			}
			case 12:
				return read_procedure(p_index);
			case 13: {
				NativeProcedureCell *native = heap.make<NativeProcedureCell>();
				cells[p_index] = native;
				natives.push_back(native);
				return read_name(native->binding_key) && read_name(native->decorated_name) && read_u32(native->positional_count);
			}
			case 14: {
				FunctionCell *function = heap.make<FunctionCell>();
				cells[p_index] = function;
				return read_pointer(function->callee, kCalleeKinds) && read_value(function->self) &&
						read_pointer(function->parent, kind_bit(CellKind::Scope));
			}
			case 15: {
				ScopeCell *scope = heap.make<ScopeCell>();
				cells[p_index] = scope;
				return read_pointer(scope->parent, kind_bit(CellKind::Scope)) && read_values(scope->captures);
			}
			case 16:
				return read_class(p_index);
			case 17:
				return read_archetype(p_index);
			case 18: {
				AccessSpecifierCell *access = heap.make<AccessSpecifierCell>();
				cells[p_index] = access;
				access->level = vbc_read_u8(reader);
				if (!check_reader() || !read_count(count)) {
					return false;
				}
				access->scope_paths.resize(count);
				for (std::string &scope_path : access->scope_paths) {
					if (!read_string(scope_path)) {
						return false;
					}
				}
				return true;
			}
			case 19: {
				EnumerationCell *enumeration = heap.make<EnumerationCell>();
				cells[p_index] = enumeration;
				if (!read_name(enumeration->name) || !read_count(count)) {
					return false;
				}
				enumeration->enumerators.resize(count);
				for (const EnumeratorCell *&enumerator : enumeration->enumerators) {
					if (!read_pointer(enumerator, kind_bit(CellKind::Enumerator))) {
						return false;
					}
				}
				return true;
			}
			case 20: {
				EnumeratorCell *enumerator = heap.make<EnumeratorCell>();
				cells[p_index] = enumerator;
				return read_pointer(enumerator->enumeration, kind_bit(CellKind::Enumeration)) && read_name(enumerator->name) &&
						read_u32(enumerator->ordinal);
			}
			case 21:
			case 22:
			case 23:
				return malformed("cell " + std::to_string(p_index) + " is of the reserved union kind " + std::to_string(kind));
			case 24:
				return read_package(p_index);
			case 25: {
				ModuleCell *module = heap.make<ModuleCell>();
				cells[p_index] = module;
				return read_name(module->verse_path) && read_name(module->name);
			}
			case 26: {
				ObjectCell *object = heap.make<ObjectCell>();
				cells[p_index] = object;
				program.value_objects.push_back(object);
				if (!read_pointer(object->object_class, kind_bit(CellKind::Class)) || !read_count(count)) {
					return false;
				}
				object->field_names.resize(count);
				object->field_values.resize(count);
				for (size_t field = 0; field < count; ++field) {
					if (!read_name(object->field_names[field]) || !read_value(object->field_values[field])) {
						return false;
					}
				}
				return true;
			}
			case 27:
			case 28: {
				BoundedTypeCell *bounded = heap.make<BoundedTypeCell>(kind == 28);
				cells[p_index] = bounded;
				return read_value(bounded->lower) && read_value(bounded->upper);
			}
			case 29: {
				TupleTypeCell *tuple = heap.make<TupleTypeCell>();
				cells[p_index] = tuple;
				return read_values(tuple->elements);
			}
			case 30: {
				MapTypeCell *map_type = heap.make<MapTypeCell>();
				cells[p_index] = map_type;
				return read_value(map_type->key_type) && read_value(map_type->value_type);
			}
			case 31: {
				SimpleTypeCell *simple = heap.make<SimpleTypeCell>();
				cells[p_index] = simple;
				simple->code = vbc_read_u8(reader);
				if (!check_reader()) {
					return false;
				}
				size_t components = 0;
				switch (simple->code) {
					case 8:
					case 10:
					case 15:
					case 16:
						components = 1;
						break;
					case 11:
						components = 2;
						break;
					case 9:
					case 12:
					case 14:
						return malformed("simple type cell " + std::to_string(p_index) + " uses the unused code " + std::to_string(simple->code));
					default:
						if (simple->code > 19) {
							return malformed("simple type cell " + std::to_string(p_index) + " has an unknown code " + std::to_string(simple->code));
						}
						break;
				}
				simple->components.resize(components);
				for (Value &component : simple->components) {
					if (!read_value(component)) {
						return false;
					}
				}
				return true;
			}
			case 32: {
				AccessorCell *accessor = heap.make<AccessorCell>();
				cells[p_index] = accessor;
				for (std::vector<const NameCell *> *names_list : { &accessor->getters, &accessor->setters }) {
					if (!read_count(count)) {
						return false;
					}
					names_list->resize(count);
					for (const NameCell *&name : *names_list) {
						if (!read_name(name)) {
							return false;
						}
						if (name->text.empty()) {
							name = nullptr;
						}
					}
				}
				return true;
			}
			case 33:
			case 34:
			case 35: {
				const CellKind element_kind = kind == 33 ? CellKind::ArrayType : kind == 34 ? CellKind::OptionType
																							: CellKind::PointerType;
				ElementTypeCell *element = heap.make<ElementTypeCell>(element_kind);
				cells[p_index] = element;
				return read_value(element->element_type);
			}
			default:
				return malformed("cell " + std::to_string(p_index) + " is of the unknown kind " + std::to_string(kind));
		}
	}

	bool read_packages() {
		size_t count;
		if (!read_count(count)) {
			return false;
		}
		package_cells.resize(count);
		for (uint32_t &package : package_cells) {
			if (!read_ref(package)) {
				return false;
			}
			if (package == 0) {
				return malformed("the package list names no cell");
			}
		}
		return true;
	}

	bool read_well_known() {
		size_t count;
		if (!read_count(count)) {
			return false;
		}
		well_known.resize(count);
		for (WellKnown &entry : well_known) {
			if (!read_sid(entry.role) || !read_ref(entry.cell)) {
				return false;
			}
		}
		return true;
	}

	bool read_class_index() {
		size_t count;
		if (!read_count(count)) {
			return false;
		}
		raw_classes.resize(count);
		for (RawClassEntry &entry : raw_classes) {
			entry.origin = vbc_read_u8(reader);
			if (!check_reader() || !read_sid(entry.name) || !read_ref(entry.cell)) {
				return false;
			}
			if (entry.origin > 2) {
				return malformed("the class index holds an unknown origin " + std::to_string(entry.origin));
			}
		}
		return true;
	}

	bool read_end() {
		const uint8_t marker = vbc_read_u8(reader);
		if (!reader.ok() || marker != kEndMarker) {
			error.clear();
			return refuse("is truncated or malformed: its end marker is missing.");
		}
		if (reader.offset != reader.size) {
			return malformed(std::to_string(reader.size - reader.offset) + " bytes follow the end marker");
		}
		return true;
	}

	Cell *cell_of_kind(uint32_t p_ref, uint64_t p_kinds, const char *p_what) {
		if (p_ref == 0) {
			malformed(std::string(p_what) + " names no cell");
			return nullptr;
		}
		Cell *cell = cells[p_ref - 1];
		if ((p_kinds & kind_bit(cell->kind)) == 0) {
			malformed(std::string(p_what) + " names a " + cell_kind_name(cell->kind) + " cell");
			return nullptr;
		}
		return cell;
	}

	bool resolve() {
		std::vector<Value> values(cells.size());
		for (size_t index = 0; index < cells.size(); ++index) {
			Cell *cell = cells[index];
			values[index] = Value::from_cell(cell);
			if (cell->kind == CellKind::HeapInt) {
				// An integer that fits int32 is always an immediate (vm_cell.h, HeapIntCell).
				const BigInt &number = static_cast<HeapIntCell *>(cell)->value;
				if (number.fits_int32()) {
					values[index] = Value::from_int32(number.to_int32());
				}
			}
		}
		for (const Fixup &fixup : fixups) {
			Cell *cell = cells[fixup.index];
			if (fixup.kinds != kAnyKind && (fixup.kinds & kind_bit(cell->kind)) == 0) {
				return malformed("cell " + std::to_string(fixup.index) + " is a " + cell_kind_name(cell->kind) + " where another kind is required");
			}
			fixup.apply(fixup.slot, cell, values[fixup.index]);
		}
		fixups.clear();

		for (PendingRational &pending : rationals) {
			if (!is_int(pending.numerator) || !is_int(pending.denominator)) {
				return malformed("a rational's numerator or denominator is not an integer");
			}
			BigInt numerator = int_value(pending.numerator);
			BigInt denominator = int_value(pending.denominator);
			if (denominator.is_zero()) {
				return malformed("a rational has a zero denominator");
			}
			if (denominator.negative) {
				numerator = big_neg(numerator);
				denominator = big_neg(denominator);
			}
			const BigInt divisor = big_gcd(numerator, denominator);
			BigInt remainder;
			big_divmod_trunc(numerator, divisor, pending.cell->numerator, remainder);
			big_divmod_trunc(denominator, divisor, pending.cell->denominator, remainder);
		}
		for (MapCell *map : maps) {
			map_reindex(map);
		}
		return true;
	}

	bool bind_natives() {
		for (NativeProcedureCell *native : natives) {
			NativeFn implementation = native_implementation(native->binding_key->text);
			if (implementation == nullptr && native->binding_key->text == kMissingProcedureKey) {
				implementation = &native_missing_procedure;
			}
			native->bound = implementation != nullptr;
			native->implementation = native->bound ? implementation : &native_not_implemented;
			program.unbound_native_count += native->bound ? 0 : 1;
		}
		program.native_count = natives.size();
		return true;
	}

	// spec/modules.md §2 step 4, spec/calls.md §10.1.
	bool supply_builtin_package() {
		if (builtin_package == nullptr) {
			builtin_package = heap.make<PackageCell>(true);
		}
		NativeProcedureCell *native = heap.make<NativeProcedureCell>();
		native->binding_key = heap.intern(kMissingProcedureKey);
		native->decorated_name = heap.intern(kMissingProcedureName);
		native->implementation = &native_missing_procedure;
		native->bound = true;
		FunctionCell *function = heap.make<FunctionCell>();
		function->callee = native;
		function->self = heap.false_value();
		builtin_package->definitions.push_back(PackageDefinition{ native->binding_key, Value::from_cell(native) });
		program.builtin_package = builtin_package;
		program.missing_procedure = function;
		heap.add_permanent_root(Value::from_cell(builtin_package));
		heap.add_permanent_root(Value::from_cell(function));
		return true;
	}

	bool finish() {
		for (uint32_t ref : package_cells) {
			Cell *cell = cell_of_kind(ref, kind_bit(CellKind::Package), "the package list");
			if (cell == nullptr) {
				return false;
			}
			program.packages.push_back(static_cast<const PackageCell *>(cell));
			heap.add_permanent_root(Value::from_cell(cell));
		}

		for (const WellKnown &entry : well_known) {
			const std::string &role = strings[size_t(entry.role)];
			if (role == "task_class") {
				program.task_class = static_cast<const ClassCell *>(cell_of_kind(entry.cell, kind_bit(CellKind::Class), "the task_class role"));
				if (program.task_class == nullptr) {
					return false;
				}
			} else if (role == "accessor_enumerator") {
				program.accessor_enumerator = static_cast<const EnumeratorCell *>(cell_of_kind(entry.cell, kind_bit(CellKind::Enumerator), "the accessor_enumerator role"));
				if (program.accessor_enumerator == nullptr) {
					return false;
				}
			}
		}
		if (program.task_class == nullptr) {
			return refuse("names no task_class definition, which this runtime needs. Export the project again.");
		}
		if (program.accessor_enumerator == nullptr) {
			return refuse("names no accessor_enumerator definition, which this runtime needs. Export the project again.");
		}
		heap.add_permanent_root(Value::from_cell(program.task_class));
		heap.add_permanent_root(Value::from_cell(program.accessor_enumerator));

		program.classes.reserve(raw_classes.size());
		for (const RawClassEntry &raw : raw_classes) {
			const Cell *cell = cell_of_kind(raw.cell, kind_bit(CellKind::Class), "the class index");
			if (cell == nullptr) {
				return false;
			}
			ClassIndexEntry entry;
			entry.origin = ClassOrigin(raw.origin);
			entry.name = name_for(raw.name);
			entry.class_cell = static_cast<const ClassCell *>(cell);
			const size_t position = program.classes.size();
			program.classes.push_back(entry);
			program.classes_by_name[raw.origin].emplace(entry.name->text, position);
			program.classes_by_cell.emplace(entry.class_cell, position);
			heap.add_permanent_root(Value::from_cell(cell));
		}
		return true;
	}
};

} // namespace

const ClassIndexEntry *Program::find_class(ClassOrigin p_origin, std::string_view p_name) const {
	const std::unordered_map<std::string, size_t> &by_name = classes_by_name[uint8_t(p_origin)];
	const auto found = by_name.find(std::string(p_name));
	return found == by_name.end() ? nullptr : &classes[found->second];
}

const ClassIndexEntry *Program::entry_for(const ClassCell *p_class) const {
	const auto found = classes_by_cell.find(p_class);
	return found == classes_by_cell.end() ? nullptr : &classes[found->second];
}

bool load_program(Heap &r_heap, const uint8_t *p_data, size_t p_size, const std::string &p_path, Program &r_program, std::string &r_error) {
	r_error.clear();
	Loader loader(r_heap, p_data, p_size, p_path, r_program, r_error);
	return loader.run();
}

} // namespace vm
