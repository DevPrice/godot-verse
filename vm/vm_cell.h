#pragma once

#include <cstdint>
#include <initializer_list>
#include <memory>
#include <new>
#include <string>
#include <utility>
#include <vector>

#include "vm_bigint.h"
#include "vm_status.h"
#include "vm_value.h"

namespace vm {

// Every heap cell kind. The first block is format.md §4's kinds that have runtime behaviour; the
// second is the runtime's own.
enum class CellKind : uint8_t {
	False,
	True,
	BuiltinPackage,
	Name,
	Array,
	MutableArray,
	Map,
	MutableMap,
	Option,
	HeapInt,
	Rational,
	Procedure,
	NativeProcedure,
	Function,
	Scope,
	Class,
	Archetype,
	AccessSpecifier,
	Enumeration,
	Enumerator,
	Package,
	Module,
	Object,
	IntType,
	FloatType,
	TupleType,
	MapType,
	SimpleType,
	Accessor,
	ArrayType,
	OptionType,
	PointerType,

	Placeholder,
	Ref,
	Frame,
	Task,
	NativeObject,
	AccessorRef,
	SetterChain,
	Semaphore,
	ContentScope,
};

const char *cell_kind_name(CellKind p_kind);

// A cell's run of Values: in the room its cell was allocated with (Heap::make_with_room), or, once
// it outgrows that or when the cell had none, in storage of its own. Pointers into it stay valid
// until it grows.
class ValueArray {
public:
	ValueArray() = default;
	ValueArray(const ValueArray &) = delete;
	ValueArray &operator=(const ValueArray &) = delete;
	~ValueArray() {
		if (owned) {
			::operator delete(items);
		}
	}

	ValueArray &operator=(std::initializer_list<Value> p_values) {
		copy_from(p_values.begin(), p_values.size());
		return *this;
	}
	ValueArray &operator=(const std::vector<Value> &p_values) {
		copy_from(p_values.data(), p_values.size());
		return *this;
	}

	// p_storage, room for p_capacity Values that lives as long as this array, becomes its storage,
	// holding p_capacity empty Values.
	void use_room(Value *p_storage, uint32_t p_capacity) {
		for (uint32_t index = 0; index < p_capacity; ++index) {
			new (p_storage + index) Value();
		}
		items = p_storage;
		count = p_capacity;
		capacity = p_capacity;
	}

	size_t size() const { return count; }
	bool empty() const { return count == 0; }
	Value *data() { return items; }
	const Value *data() const { return items; }
	Value &operator[](size_t p_index) { return items[p_index]; }
	const Value &operator[](size_t p_index) const { return items[p_index]; }
	Value *begin() { return items; }
	Value *end() { return items + count; }
	const Value *begin() const { return items; }
	const Value *end() const { return items + count; }

	void clear() { count = 0; }
	void push_back(Value p_value) {
		reserve(size_t(count) + 1);
		items[count++] = p_value;
	}
	void resize(size_t p_count) { assign_tail(p_count, Value::empty()); }
	void assign(size_t p_count, Value p_value) {
		count = 0;
		assign_tail(p_count, p_value);
	}
	void reserve(size_t p_capacity) {
		if (p_capacity <= capacity) {
			return;
		}
		size_t grown = size_t(capacity) * 2;
		if (grown < p_capacity) {
			grown = p_capacity < 4 ? 4 : p_capacity;
		}
		Value *moved = static_cast<Value *>(::operator new(grown * sizeof(Value)));
		for (uint32_t index = 0; index < count; ++index) {
			new (moved + index) Value(items[index]);
		}
		if (owned) {
			::operator delete(items);
		}
		items = moved;
		capacity = uint32_t(grown);
		owned = true;
	}

private:
	Value *items = nullptr;
	uint32_t count = 0;
	uint32_t capacity = 0;
	bool owned = false;

	void assign_tail(size_t p_count, Value p_value) {
		reserve(p_count);
		for (size_t index = count; index < p_count; ++index) {
			new (items + index) Value(p_value);
		}
		count = uint32_t(p_count);
	}
	void copy_from(const Value *p_values, size_t p_count) {
		count = 0;
		reserve(p_count);
		for (size_t index = 0; index < p_count; ++index) {
			new (items + index) Value(p_values[index]);
		}
		count = uint32_t(p_count);
	}
};

// CreateField's per-slot marks (spec/objects.md §7.3): a word for up to 64 slots, a vector past it.
class SlotMarks {
public:
	bool operator[](size_t p_index) const {
		return large.empty() ? ((small >> p_index) & 1) != 0 : bool(large[p_index]);
	}
	void set(size_t p_index) {
		if (large.empty()) {
			small |= uint64_t(1) << p_index;
		} else {
			large[p_index] = true;
		}
	}
	void assign(size_t p_count, bool p_value) {
		if (p_count <= 64) {
			large.clear();
			small = p_value ? (p_count == 64 ? ~uint64_t(0) : (uint64_t(1) << p_count) - 1) : 0;
		} else {
			large.assign(p_count, p_value);
		}
	}

private:
	uint64_t small = 0;
	std::vector<bool> large;
};

// What a collector hands every cell: each Value and each cell pointer the cell holds. The heap
// never moves a cell, so both are passed by value.
class CellVisitor {
public:
	virtual ~CellVisitor() = default;
	virtual void visit(Value p_value) = 0;
	void visit(const struct Cell *p_cell) {
		if (p_cell != nullptr) {
			visit(Value::from_cell(p_cell));
		}
	}
	void visit(const std::vector<Value> &p_values) {
		for (Value value : p_values) {
			visit(value);
		}
	}
	void visit(const ValueArray &p_values) {
		for (Value value : p_values) {
			visit(value);
		}
	}
};

struct Cell {
	CellKind kind;
	// A tenured cell (Heap::tenure) reads marked from then on, so a weak table's sweep keeps it.
	bool marked = false;
	bool tenured = false;
	// Where the heap got the cell's memory: its own business.
	static constexpr uint32_t kUnpooled = 0xFFFFFFFFu;
	uint8_t size_class = 0;
	uint32_t slab = kUnpooled;
	Cell *next_allocated = nullptr;

	explicit Cell(CellKind p_kind) :
			kind(p_kind) {}
	Cell(const Cell &) = delete;
	Cell &operator=(const Cell &) = delete;
	virtual ~Cell() = default;

	virtual void visit_references(CellVisitor &r_visitor) const = 0;
};

inline bool is_cell_kind(Value p_value, CellKind p_kind) {
	return p_value.is_cell() && p_value.as_cell()->kind == p_kind;
}

template <typename T>
T *cell_as(Value p_value) {
	return static_cast<T *>(p_value.as_cell());
}

// `false` and `true`: the empty option and logic false, and the one option whose content is false
// (spec/values.md §9).
struct LogicCell : Cell {
	explicit LogicCell(bool p_value) :
			Cell(p_value ? CellKind::True : CellKind::False) {}
	void visit_references(CellVisitor &) const override {}
};

// An interned string. Equal contents are always the same cell (Heap::intern).
struct NameCell : Cell {
	std::string text;

	explicit NameCell(std::string p_text) :
			Cell(CellKind::Name), text(std::move(p_text)) {}
	void visit_references(CellVisitor &) const override {}
};

// An array or a mutable array; InPlaceMakeImmutable turns one into the other by changing kind.
// Storage is unobservable (spec/values.md §6.1): an array of chars keeps its bytes packed so a
// string is a byte string, and everything else is general values. format.md's int32 and char32
// element kinds load as general values.
struct ArrayCell : Cell {
	enum class Storage : uint8_t {
		Values,
		Char8,
	};

	Storage storage = Storage::Values;
	std::vector<Value> values;
	std::string bytes;

	explicit ArrayCell(bool p_mutable) :
			Cell(p_mutable ? CellKind::MutableArray : CellKind::Array) {}

	bool is_mutable() const { return kind == CellKind::MutableArray; }
	size_t length() const { return storage == Storage::Char8 ? bytes.size() : values.size(); }
	Value get(size_t p_index) const {
		return storage == Storage::Char8 ? Value::from_char8(uint8_t(bytes[p_index])) : values[p_index];
	}
	void set(size_t p_index, Value p_value);
	void append(Value p_value);
	void truncate(size_t p_length);
	void spread_to_values();

	void visit_references(CellVisitor &r_visitor) const override { r_visitor.visit(values); }
};

struct MapEntry {
	Value key;
	Value value;
	uint64_t hash = 0;
};

// A map or a mutable map, in insertion order (spec/values.md §8.1). `index` is an open-addressed
// table of entry positions plus one, kept only once a map outgrows a linear scan; vm_values.cpp
// owns it.
struct MapCell : Cell {
	std::vector<MapEntry> entries;
	std::vector<uint32_t> index;

	explicit MapCell(bool p_mutable) :
			Cell(p_mutable ? CellKind::MutableMap : CellKind::Map) {}

	bool is_mutable() const { return kind == CellKind::MutableMap; }
	void visit_references(CellVisitor &r_visitor) const override {
		for (const MapEntry &entry : entries) {
			r_visitor.visit(entry.key);
			r_visitor.visit(entry.value);
		}
	}
};

struct OptionCell : Cell {
	Value content;

	explicit OptionCell(Value p_content) :
			Cell(CellKind::Option), content(p_content) {}
	void visit_references(CellVisitor &r_visitor) const override { r_visitor.visit(content); }
};

// Only for integers outside int32: every result that fits is an immediate again, so an int32
// immediate and a heap int are never equal. Likewise a value that fits int64 is always `narrow`
// and `wide` is used only beyond it, so each integer has exactly one representation.
struct HeapIntCell : Cell {
	int64_t narrow = 0;
	bool is_wide = false;
	BigInt wide;

	explicit HeapIntCell(int64_t p_value) :
			Cell(CellKind::HeapInt), narrow(p_value) {}
	explicit HeapIntCell(BigInt p_value) :
			Cell(CellKind::HeapInt) {
		if (p_value.fits_int64()) {
			narrow = p_value.to_int64();
		} else {
			is_wide = true;
			wide = std::move(p_value);
		}
	}
	BigInt value() const { return is_wide ? wide : BigInt::from_int64(narrow); }
	bool fits_int32() const { return !is_wide && narrow >= INT32_MIN && narrow <= INT32_MAX; }
	void visit_references(CellVisitor &) const override {}
};

// Always in lowest terms with a positive denominator (spec/values.md §2.4).
struct RationalCell : Cell {
	BigInt numerator;
	BigInt denominator;

	RationalCell(BigInt p_numerator, BigInt p_denominator) :
			Cell(CellKind::Rational), numerator(std::move(p_numerator)), denominator(std::move(p_denominator)) {}
	void visit_references(CellVisitor &) const override {}
};

struct NamedParameter {
	const NameCell *name = nullptr;
	uint32_t register_index = 0;
};

struct UnwindEdge {
	uint32_t first_op = 0;
	uint32_t last_op = 0;
	uint32_t landing_op = 0;
};

struct LineEntry {
	uint32_t op = 0;
	uint32_t line = 0;
};

struct RegisterName {
	uint32_t register_index = 0;
	const NameCell *name = nullptr;
	uint32_t first_op = 0;
	uint32_t last_op = 0;
};

struct DecodedOp {
	uint16_t opcode = 0;
	uint32_t operands = 0;
};

// An absent operand, in any slot: an optional one not written, a `value` or cell operand the
// compiler left out.
constexpr uint32_t kAbsentOperand = 0xFFFFFFFFu;

// format.md §5. The op stream is decoded once, at load, for a switch loop to execute: op i's
// operands are the words starting at operand_words[ops[i].operands], one word per operand ops.json
// lists for its opcode with cache operands left out, in the schema's order, so operand k of a
// known opcode is one index away. A word is, by the operand's kind:
//   register            the register index
//   value               register r as r << 1, constant c as (c << 1) | 1
//   value_imm, cell:*   a constant index: the loader appends each to `constants`
//   label               an op index, below ops.size()
//   bool, i32, u32, failure_context_id, enum:*   the number itself, an i32 as its bit pattern
//   live_range, asset_path   the index of two more words: first and last op (each at most
//                       ops.size(); (ops.size(), 0) is the empty range), or two constant indices
//                       of name cells, package then asset
//   variadic            the index of a count followed by that many words of the element's kind
// An operand the file leaves absent is kAbsentOperand, whatever its kind.
struct ProcedureCell : Cell {
	const NameCell *name = nullptr;
	std::string file;
	uint32_t flags = 0;
	uint32_t register_count = 0;
	uint32_t positional_count = 0;
	std::vector<NamedParameter> named_parameters;
	std::vector<Value> constants;
	std::vector<DecodedOp> ops;
	std::vector<uint32_t> operand_words;
	std::vector<UnwindEdge> unwind_edges;
	std::vector<LineEntry> lines;
	std::vector<RegisterName> register_names;

	ProcedureCell() :
			Cell(CellKind::Procedure) {}
	void visit_references(CellVisitor &r_visitor) const override {
		r_visitor.visit(name);
		for (const NamedParameter &parameter : named_parameters) {
			r_visitor.visit(parameter.name);
		}
		r_visitor.visit(constants);
		for (const RegisterName &register_name : register_names) {
			r_visitor.visit(register_name.name);
		}
	}
};

struct NativeCall;
typedef Outcome (*NativeFn)(NativeCall &r_call);

// `bound` is false for a native this runtime has no implementation of: `implementation` is then
// the stand-in that raises (spec/natives.md §3.2).
struct NativeProcedureCell : Cell {
	const NameCell *binding_key = nullptr;
	const NameCell *decorated_name = nullptr;
	uint32_t positional_count = 0;
	NativeFn implementation = nullptr;
	bool bound = false;

	NativeProcedureCell() :
			Cell(CellKind::NativeProcedure) {}
	void visit_references(CellVisitor &r_visitor) const override {
		r_visitor.visit(binding_key);
		r_visitor.visit(decorated_name);
	}
};

struct ScopeCell : Cell {
	const ScopeCell *parent = nullptr;
	std::vector<Value> captures;

	ScopeCell() :
			Cell(CellKind::Scope) {}
	void visit_references(CellVisitor &r_visitor) const override {
		r_visitor.visit(parent);
		r_visitor.visit(captures);
	}
};

// Self is uninitialized for a method not yet bound, the false cell for a function with no
// receiver, or the receiver (spec/calls.md §6).
struct FunctionCell : Cell {
	const Cell *callee = nullptr;
	Value self = Value::uninitialized();
	const ScopeCell *parent = nullptr;

	FunctionCell() :
			Cell(CellKind::Function) {}
	void visit_references(CellVisitor &r_visitor) const override {
		r_visitor.visit(callee);
		r_visitor.visit(self);
		r_visitor.visit(parent);
	}
};

struct ArchetypeCell;
struct PackageCell;

enum class ClassKind : uint8_t {
	Class = 0,
	Struct = 1,
	Interface = 2,
};

struct ClassCell : Cell {
	static constexpr uint32_t FLAG_NATIVE_REPRESENTATION = 1;
	static constexpr uint32_t FLAG_UNIQUE = 2048;
	static constexpr uint32_t FLAG_EMULATE_CASE_INSENSITIVE_OVERRIDES = 4096;

	ClassKind class_kind = ClassKind::Class;
	uint32_t flags = 0;
	const PackageCell *package = nullptr;
	std::string relative_path;
	std::string base_name;
	bool has_attributes = false;
	std::vector<Value> attributes;
	std::vector<uint32_t> attribute_indices;
	std::vector<const ClassCell *> inherited;
	const ArchetypeCell *archetype = nullptr;
	const FunctionCell *constructor = nullptr;
	const FunctionCell *blocks = nullptr;
	bool native_bound = false;

	ClassCell() :
			Cell(CellKind::Class) {}
	void visit_references(CellVisitor &r_visitor) const override;
};

struct ArchetypeEntry {
	const NameCell *name = nullptr;
	const Cell *access = nullptr;
	Value type = Value::uninitialized();
	Value value = Value::uninitialized();
	uint8_t flags = 0;
};

struct ArchetypeCell : Cell {
	const ClassCell *owner = nullptr;
	const ArchetypeCell *next = nullptr;
	std::vector<ArchetypeEntry> entries;

	ArchetypeCell() :
			Cell(CellKind::Archetype) {}
	void visit_references(CellVisitor &r_visitor) const override {
		r_visitor.visit(owner);
		r_visitor.visit(next);
		for (const ArchetypeEntry &entry : entries) {
			r_visitor.visit(entry.name);
			r_visitor.visit(entry.access);
			r_visitor.visit(entry.type);
			r_visitor.visit(entry.value);
		}
	}
};

struct AccessSpecifierCell : Cell {
	uint8_t level = 0;
	std::vector<std::string> scope_paths;

	AccessSpecifierCell() :
			Cell(CellKind::AccessSpecifier) {}
	void visit_references(CellVisitor &) const override {}
};

struct EnumeratorCell;

struct EnumerationCell : Cell {
	const NameCell *name = nullptr;
	std::vector<const EnumeratorCell *> enumerators;

	EnumerationCell() :
			Cell(CellKind::Enumeration) {}
	void visit_references(CellVisitor &r_visitor) const override;
};

struct EnumeratorCell : Cell {
	const EnumerationCell *enumeration = nullptr;
	const NameCell *name = nullptr;
	uint32_t ordinal = 0;

	EnumeratorCell() :
			Cell(CellKind::Enumerator) {}
	void visit_references(CellVisitor &r_visitor) const override {
		r_visitor.visit(enumeration);
		r_visitor.visit(name);
	}
};

struct PackageDefinition {
	const NameCell *path = nullptr;
	Value value;
};

// Also the built-in package (format.md §4 kind 3), which has no definitions of its own in the file.
struct PackageCell : Cell {
	const NameCell *name = nullptr;
	const NameCell *root_path = nullptr;
	std::vector<PackageDefinition> definitions;

	explicit PackageCell(bool p_builtin = false) :
			Cell(p_builtin ? CellKind::BuiltinPackage : CellKind::Package) {}
	void visit_references(CellVisitor &r_visitor) const override {
		r_visitor.visit(name);
		r_visitor.visit(root_path);
		for (const PackageDefinition &definition : definitions) {
			r_visitor.visit(definition.path);
			r_visitor.visit(definition.value);
		}
	}
};

struct ModuleCell : Cell {
	const NameCell *verse_path = nullptr;
	const NameCell *name = nullptr;

	ModuleCell() :
			Cell(CellKind::Module) {}
	void visit_references(CellVisitor &r_visitor) const override {
		r_visitor.visit(verse_path);
		r_visitor.visit(name);
	}
};

struct ClassLayout;

// NativeState::tag, one per concrete type across vm/.
enum NativeStateTag : uint32_t {
	kEventStateTag = 1,
	kSubscriptionStateTag = 2,
	kSubsetVarStateTag = 3,
};

// What a native class keeps of an object beyond its Verse fields -- an event's awaiters, a
// classifiable_subset_var's current set (spec/natives.md §5.10, §7). Invisible to Verse: equality,
// freezing and the layout never see it; the collector visits it with its object and it dies with
// it. `tag` names the concrete type, since there is no RTTI to ask.
struct NativeState {
	const uint32_t tag;

	explicit NativeState(uint32_t p_tag) :
			tag(p_tag) {}
	NativeState(const NativeState &) = delete;
	NativeState &operator=(const NativeState &) = delete;
	virtual ~NativeState() = default;
	virtual void visit_references(CellVisitor &r_visitor) const = 0;
};

// A struct value or a VM-level class instance: format.md's `value object`, and what NewObject makes.
// Fields are the object's slots, in its layout's slot order (vm_objects.h); `created` is
// CreateField's per-slot mark. A value object the loader built has no layout until
// lay_out_value_object gives it one, and its fields are the file's. A laid-out object leaves
// `field_names` empty and its layout's slot names stand for them, so read the names through
// object_field_names (vm_objects.h).
struct ObjectCell : Cell {
	const ClassCell *object_class = nullptr;
	std::vector<const NameCell *> field_names;
	ValueArray field_values;
	const ClassLayout *layout = nullptr;
	SlotMarks created;
	std::unique_ptr<NativeState> native_state;

	ObjectCell() :
			Cell(CellKind::Object) {}
	bool is_struct() const { return object_class != nullptr && object_class->class_kind == ClassKind::Struct; }

	// The object's native state of type T (which declares `static constexpr uint32_t kTag` and a
	// default constructor), made on first use. Null only when the object already holds state of
	// another type, which a native reached through the wrong class would be.
	template <typename T>
	T *state() {
		if (native_state == nullptr) {
			native_state = std::make_unique<T>();
		}
		return native_state->tag == T::kTag ? static_cast<T *>(native_state.get()) : nullptr;
	}

	void visit_references(CellVisitor &r_visitor) const override {
		r_visitor.visit(object_class);
		for (const NameCell *name : field_names) {
			r_visitor.visit(name);
		}
		r_visitor.visit(field_values);
		if (native_state != nullptr) {
			native_state->visit_references(r_visitor);
		}
	}
};

// `int type` and `float type`: each bound is a value, or uninitialized for none.
struct BoundedTypeCell : Cell {
	Value lower = Value::uninitialized();
	Value upper = Value::uninitialized();

	explicit BoundedTypeCell(bool p_float) :
			Cell(p_float ? CellKind::FloatType : CellKind::IntType) {}
	void visit_references(CellVisitor &r_visitor) const override {
		r_visitor.visit(lower);
		r_visitor.visit(upper);
	}
};

struct TupleTypeCell : Cell {
	std::vector<Value> elements;

	TupleTypeCell() :
			Cell(CellKind::TupleType) {}
	void visit_references(CellVisitor &r_visitor) const override { r_visitor.visit(elements); }
};

struct MapTypeCell : Cell {
	Value key_type;
	Value value_type;

	MapTypeCell() :
			Cell(CellKind::MapType) {}
	void visit_references(CellVisitor &r_visitor) const override {
		r_visitor.visit(key_type);
		r_visitor.visit(value_type);
	}
};

struct SimpleTypeCell : Cell {
	uint8_t code = 0;
	std::vector<Value> components;

	SimpleTypeCell() :
			Cell(CellKind::SimpleType) {}
	void visit_references(CellVisitor &r_visitor) const override { r_visitor.visit(components); }
};

// `array type`, `option type` and `pointer type`.
struct ElementTypeCell : Cell {
	Value element_type;

	explicit ElementTypeCell(CellKind p_kind) :
			Cell(p_kind) {}
	void visit_references(CellVisitor &r_visitor) const override { r_visitor.visit(element_type); }
};

// Getter names by parameter count minus one, setter names by parameter count minus two; null for an
// absent slot (spec/objects.md §16).
struct AccessorCell : Cell {
	std::vector<const NameCell *> getters;
	std::vector<const NameCell *> setters;

	AccessorCell() :
			Cell(CellKind::Accessor) {}
	void visit_references(CellVisitor &r_visitor) const override {
		for (const NameCell *name : getters) {
			r_visitor.visit(name);
		}
		for (const NameCell *name : setters) {
			r_visitor.visit(name);
		}
	}
};

// A logic variable (spec/unification.md §2). Linked: `target` is another placeholder. Bound:
// `target` is the concrete value. Unbound: `waiters` are the parked ops it wakes, which stage 2
// of leniency (T3.10) fills.
struct PlaceholderCell : Cell {
	enum class State : uint8_t {
		Unbound,
		Linked,
		Bound,
	};

	State state = State::Unbound;
	Value target;
	std::vector<const Cell *> waiters;

	PlaceholderCell() :
			Cell(CellKind::Placeholder) {}
	void visit_references(CellVisitor &r_visitor) const override {
		r_visitor.visit(target);
		for (const Cell *waiter : waiters) {
			r_visitor.visit(waiter);
		}
	}
};

struct TaskCell;
struct FrameCell;

// A task that read a variable while it had an await point (spec/tasks.md §5.7): a write resumes it
// only if it is still suspended at that point, which is the op index `pc` in `frame`.
struct AwaitRegistration {
	TaskCell *task = nullptr;
	FrameCell *frame = nullptr;
	uint32_t pc = 0;
};

// A variable (spec/ops.md §8.3): the cell a `var` holds its content in. A hidden one stands in an
// array element, map value or object field that an awaiting task read (spec/ops.md §3.1), and every
// read of that slot reads through it.
struct RefCell : Cell {
	Value content;
	Value domain = Value::uninitialized();
	bool hidden = false;
	// The variable of a `<native>` member: every write converts to native storage (spec/objects.md
	// §9.2).
	bool native = false;
	// spec/ops.md §3.2: the task whose cancellation ends the live binding that last wrote this.
	Value live_task = Value::uninitialized();
	std::vector<AwaitRegistration> awaiting;

	explicit RefCell(Value p_content) :
			Cell(CellKind::Ref), content(p_content) {}
	void visit_references(CellVisitor &r_visitor) const override;
};

constexpr uint32_t kNoRegister = 0xFFFFFFFFu;

// One activation of a procedure (spec/calls.md §2), on the heap so a Verse call never recurses on
// the native stack. An empty register is fresh. `return_pc` and `return_register` are where the
// caller continues and what its result is unified into: kNoRegister discards it, and a frame with
// no caller returns to whoever started the run.
struct FrameCell : Cell {
	const ProcedureCell *procedure = nullptr;
	ValueArray registers;
	FrameCell *caller = nullptr;
	uint32_t return_pc = 0;
	uint32_t return_register = kNoRegister;
	// The UnifyNativeObject at `setters_pc` has run the first `setters_run` setters of the chain
	// `setters_token`; each returns to the op, which runs the next (spec/objects.md §7.8).
	uint32_t setters_pc = kNoRegister;
	size_t setters_run = 0;
	Value setters_token;

	FrameCell() :
			Cell(CellKind::Frame) {}
	void visit_references(CellVisitor &r_visitor) const override {
		r_visitor.visit(procedure);
		r_visitor.visit(registers);
		r_visitor.visit(caller);
		r_visitor.visit(setters_token);
	}
};

struct TaskCell;
struct ContentScopeCell;

// A native's callback on a task (spec/tasks.md §2): a function and the one cell it acts on -- the
// list the task sits on, say -- so a hook is data the collector can visit.
typedef void (*TaskHookFn)(TaskCell *p_task, Cell *p_target);

struct TaskHook {
	TaskHookFn run = nullptr;
	Cell *target = nullptr;
};

// spec/tasks.md §2. The Verse value of a `task(t)` is the cell itself: its methods are the task class's
// (Program::task_class), bound to it. A resume frame of null with `finished` set is §5.4 step 7's
// finished resume point; `yield_task` null is an empty yield-to point.
struct TaskCell : Cell {
	enum class Phase : uint8_t {
		Active,
		CancelRequested,
		CancelStarted,
		CancelUnwind,
		Canceled,
	};

	Phase phase = Phase::Active;
	bool running = true;
	// An entry task (§4.4), whose root frame ends in Return rather than EndTask.
	bool entry = false;
	bool finished = false;
	bool has_result = false;
	Value result;
	TaskCell *parent = nullptr;
	std::vector<TaskCell *> children;
	std::vector<TaskCell *> awaiters;
	std::vector<TaskCell *> cancelers;
	FrameCell *resume_frame = nullptr;
	uint32_t resume_pc = 0;
	uint32_t resume_slot = kNoRegister;
	TaskCell *yield_task = nullptr;
	FrameCell *yield_frame = nullptr;
	uint32_t yield_pc = 0;
	std::vector<TaskHook> defer_hooks;
	std::vector<TaskHook> finish_hooks;
	ContentScopeCell *group = nullptr;
	// The scope active when the task suspended (§8.1), which its resumption runs under.
	ContentScopeCell *captured_scope = nullptr;
	// §5.7's await state: the await point is (await_frame, await_pc), kNoRegister when none.
	bool await_initializing = false;
	uint32_t await_pc = kNoRegister;
	FrameCell *await_frame = nullptr;

	TaskCell() :
			Cell(CellKind::Task) {}
	void visit_references(CellVisitor &r_visitor) const override;
};

// spec/tasks.md §5.6.
struct SemaphoreCell : Cell {
	int64_t count = 0;
	TaskCell *waiter = nullptr;

	SemaphoreCell() :
			Cell(CellKind::Semaphore) {}
	void visit_references(CellVisitor &r_visitor) const override { r_visitor.visit(waiter); }
};

// spec/tasks.md §8.1: the embedder's content scope, with the task group of the root tasks started
// while it was active. The group holds its tasks strongly (§14 Q7 allows it: only memory differs).
struct ContentScopeCell : Cell {
	bool terminated = false;
	std::vector<TaskCell *> group;

	ContentScopeCell() :
			Cell(CellKind::ContentScope) {}
	void visit_references(CellVisitor &r_visitor) const override {
		for (const TaskCell *task : group) {
			r_visitor.visit(task);
		}
	}
};

// spec/objects.md §16: a receiver and the accessor its member holds, plus one argument per step of
// a deeper path (`T.A[0].B`). Reading it calls the getter; writing it, the setter.
struct AccessorRefCell : Cell {
	Value object;
	const AccessorCell *accessor = nullptr;
	std::vector<Value> path;

	AccessorRefCell() :
			Cell(CellKind::AccessorRef) {}
	void visit_references(CellVisitor &r_visitor) const override {
		r_visitor.visit(object);
		r_visitor.visit(accessor);
		r_visitor.visit(path);
	}
};

// The construction token once InitializeVar has deferred a setter (spec/objects.md §7.6): each
// accessor reference with the value it is to be set to, in the order they were deferred. Immutable;
// deferring another makes a longer chain.
struct SetterChainCell : Cell {
	std::vector<Value> references;
	std::vector<Value> values;

	SetterChainCell() :
			Cell(CellKind::SetterChain) {}
	void visit_references(CellVisitor &r_visitor) const override {
		r_visitor.visit(references);
		r_visitor.visit(values);
	}
};

// What a container slot or field reads as: its content, through a hidden variable if it holds one.
inline Value read_slot(Value p_value) {
	if (is_cell_kind(p_value, CellKind::Ref)) {
		const RefCell *ref = cell_as<RefCell>(p_value);
		if (ref->hidden) {
			return ref->content;
		}
	}
	return p_value;
}

// A value operand never reads as a bound placeholder (spec/unification.md §1): this answers the
// concrete value or the unbound root.
inline Value follow(Value p_value) {
	while (is_cell_kind(p_value, CellKind::Placeholder)) {
		const PlaceholderCell *placeholder = cell_as<PlaceholderCell>(p_value);
		if (placeholder->state == PlaceholderCell::State::Unbound) {
			return p_value;
		}
		p_value = placeholder->target;
	}
	return p_value;
}

inline bool is_unbound_placeholder(Value p_value) {
	return is_cell_kind(p_value, CellKind::Placeholder) &&
			cell_as<PlaceholderCell>(p_value)->state == PlaceholderCell::State::Unbound;
}

} // namespace vm
