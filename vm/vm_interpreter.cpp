#include "vm_interpreter.h"

#include <cmath>
#include <utility>

#include "vbc_ops.gen.h"
#include "vm_equality.h"
#include "vm_natives.h"
#include "vm_number.h"
#include "vm_values.h"

namespace vm {

namespace {

using vbc::VbcOp;

const char *const kInternal = "ErrRuntime_Internal";
const char *const kInternalDescription = "An internal runtime error occurred. There is no other information available.";
const char *const kGlobalVariable = "ErrRuntime_UnimplementedGlobalVariable";
const char *const kGlobalVariableDescription = "Allocating a global var is not yet implemented.";

// spec/objects.md §7.1: the construction token before any setter has been deferred.
constexpr int32_t kConstructionMarker = 12774014;

int32_t line_of(const ProcedureCell *p_procedure, uint32_t p_op) {
	int32_t line = 0;
	uint32_t best = 0;
	bool found = false;
	for (const LineEntry &entry : p_procedure->lines) {
		if (entry.op <= p_op && (!found || entry.op >= best)) {
			best = entry.op;
			line = int32_t(entry.line);
			found = true;
		}
	}
	return line;
}

// `<OpName> in <procedure name>, <file>:<line>`, spec/failure.md §9.4's tail.
std::string location_of(const FrameCell *p_frame, uint32_t p_pc) {
	if (p_frame == nullptr) {
		return "the entry into the VM";
	}
	const ProcedureCell *procedure = p_frame->procedure;
	const char *op_name = p_pc < procedure->ops.size() ? vbc::kVbcOps[procedure->ops[p_pc].opcode].name : "(past the last op)";
	return std::string(op_name) + " in " + (procedure->name != nullptr ? procedure->name->text : std::string("?")) + ", " + procedure->file + ":" +
			std::to_string(line_of(procedure, p_pc));
}

bool is_unbound(Value p_value) {
	return is_unbound_placeholder(p_value);
}

void bind_placeholder(Value p_placeholder, Value p_value) {
	PlaceholderCell *placeholder = cell_as<PlaceholderCell>(p_placeholder);
	placeholder->state = is_unbound_placeholder(p_value) ? PlaceholderCell::State::Linked : PlaceholderCell::State::Bound;
	placeholder->target = p_value;
}

// spec/unification.md §3.1: a comparison meeting an unbound placeholder binds it and goes on.
class Binder : public PlaceholderMeeter {
public:
	Equality meet(Value p_left, Value p_right) override {
		if (is_unbound_placeholder(p_left)) {
			bind_placeholder(p_left, p_right);
		} else {
			bind_placeholder(p_right, p_left);
		}
		return Equality::Eq;
	}
};

class Recorder : public PlaceholderMeeter {
public:
	bool met = false;
	Equality meet(Value, Value) override {
		met = true;
		return Equality::Eq;
	}
};

bool is_array_value(Value p_value) {
	return is_cell_kind(p_value, CellKind::Array) || is_cell_kind(p_value, CellKind::MutableArray);
}

bool is_map_value(Value p_value) {
	return is_cell_kind(p_value, CellKind::Map) || is_cell_kind(p_value, CellKind::MutableMap);
}

bool is_type_cell(Value p_value) {
	if (!p_value.is_cell()) {
		return false;
	}
	switch (p_value.as_cell()->kind) {
		case CellKind::Class:
		case CellKind::IntType:
		case CellKind::FloatType:
		case CellKind::TupleType:
		case CellKind::MapType:
		case CellKind::SimpleType:
		case CellKind::ArrayType:
		case CellKind::OptionType:
		case CellKind::PointerType:
			return true;
		default:
			return false;
	}
}

bool is_whole(Value p_value) {
	if (is_int(p_value)) {
		return true;
	}
	if (!is_rational(p_value)) {
		return false;
	}
	const BigInt &denominator = cell_as<RationalCell>(p_value)->denominator;
	return denominator.limbs.size() == 1 && denominator.limbs[0] == 1 && !denominator.negative;
}

} // namespace

std::string RaisedError::message_line() const {
	std::string line = error.diagnostic + ": " + error.description;
	if (!error.message.empty()) {
		line += " (" + error.message + ")";
	}
	return line;
}

Interpreter::Interpreter(Heap &r_heap, const Program &p_program) :
		heap(r_heap), program(p_program) {}

TaskCell *Interpreter::current_task() const {
	return entry_tasks.empty() ? nullptr : cell_as<TaskCell>(entry_tasks.back());
}

void Interpreter::set_frame(FrameCell *p_frame) {
	frame = p_frame;
	if (TaskCell *task = current_task()) {
		task->frame = p_frame;
	}
}

void Interpreter::begin_entry() {
	if (entry_tasks.empty()) {
		effects.clear();
		contexts.clear();
		raised = RaisedError();
	}
	TaskCell *task = heap.make<TaskCell>();
	entry_tasks.push_back(Value::from_cell(task));
	heap.add_handle_root(&entry_tasks.back());
}

void Interpreter::end_entry(bool p_commit) {
	if (entry_tasks.empty()) {
		return;
	}
	heap.remove_handle_root(&entry_tasks.back());
	entry_tasks.pop_back();
	if (!entry_tasks.empty()) {
		return;
	}
	std::vector<std::function<void()>> pending;
	pending.swap(effects);
	contexts.clear();
	// T3.5: the root commit discards the undo log; an abort replays it first.
	if (p_commit) {
		for (const std::function<void()> &effect : pending) {
			effect();
		}
	}
}

void Interpreter::defer(std::function<void()> p_effect) {
	effects.push_back(std::move(p_effect));
}

Value Interpreter::constant(uint32_t p_index) const {
	return frame->procedure->constants[p_index];
}

Value Interpreter::read(uint32_t p_word) {
	if (p_word == kAbsentOperand) {
		return Value::uninitialized();
	}
	const uint32_t index = p_word >> 1;
	if ((p_word & 1) != 0) {
		return follow(frame->procedure->constants[index]);
	}
	Value &slot = frame->registers[index];
	if (slot.is_empty()) {
		slot = Value::from_cell(heap.make<PlaceholderCell>());
	}
	return follow(slot);
}

uint32_t Interpreter::variadic_count(uint32_t p_word) const {
	return frame->procedure->operand_words[p_word];
}

const uint32_t *Interpreter::variadic_items(uint32_t p_word) const {
	return frame->procedure->operand_words.data() + p_word + 1;
}

Interpreter::Step Interpreter::unify_slot(Value &r_slot, Value p_value) {
	p_value = follow(p_value);
	if (r_slot.is_empty()) {
		r_slot = p_value;
		return Step::Next;
	}
	const Value current = follow(r_slot);
	if (is_unbound(current)) {
		if (!current.same(p_value)) {
			bind_placeholder(current, p_value);
		}
		return Step::Next;
	}
	Binder binder;
	switch (values_equal(current, p_value, &binder)) {
		case Equality::Eq:
			return Step::Next;
		case Equality::Neq:
			return Step::Fail;
		case Equality::Undecidable:
			return invariant("an undecidable unification");
		case Equality::Error:
			break;
	}
	return invariant("a unification whose comparison raised");
}

Interpreter::Step Interpreter::unify_register(uint32_t p_register, Value p_value) {
	if (p_register == kNoRegister || p_register == kAbsentOperand) {
		return Step::Next;
	}
	return unify_slot(frame->registers[p_register], p_value);
}

Interpreter::Step Interpreter::unify_outcome(Outcome p_outcome, uint32_t p_register, Value p_value) {
	switch (p_outcome) {
		case Outcome::Ok:
			return unify_register(p_register, p_value);
		case Outcome::Fail:
			return Step::Fail;
		case Outcome::Park:
			return park();
		default:
			break;
	}
	return invariant("operands outside the op's contract");
}

void Interpreter::capture_frames(const NativeProcedureCell *p_native) {
	raised.frames.clear();
	if (p_native != nullptr) {
		ErrorFrame native;
		native.function = p_native->decorated_name != nullptr ? p_native->decorated_name->text : std::string();
		native.path = "[native]";
		raised.frames.push_back(native);
	}
	uint32_t op = pc;
	for (const FrameCell *current = frame; current != nullptr; current = current->caller) {
		ErrorFrame entry;
		entry.function = current->procedure->name != nullptr ? current->procedure->name->text : std::string();
		entry.path = current->procedure->file;
		entry.line = line_of(current->procedure, op);
		raised.frames.push_back(entry);
		op = current->return_pc > 0 ? current->return_pc - 1 : 0;
	}
}

// spec/failure.md §9.5.
Interpreter::Step Interpreter::park() {
	++park_count;
	raised.error.diagnostic = kInternal;
	raised.error.description = kInternalDescription;
	raised.error.message = "Stage-1 interpreter cannot wait: " + location_of(frame, pc) + " needs a value that is not yet known";
	capture_frames(nullptr);
	stop_outcome = Outcome::Error;
	return Step::Stop;
}

// spec/failure.md §9.4.
Interpreter::Step Interpreter::invariant(const std::string &p_what) {
	raised.error.diagnostic = kInternal;
	raised.error.description = kInternalDescription;
	raised.error.message = "VM invariant violated: " + p_what + " at " + location_of(frame, pc);
	capture_frames(nullptr);
	stop_outcome = Outcome::Error;
	return Step::Stop;
}

Interpreter::Step Interpreter::raise(const RuntimeError &p_error, const NativeProcedureCell *p_native) {
	raised.error = p_error;
	capture_frames(p_native);
	stop_outcome = Outcome::Error;
	return Step::Stop;
}

// Something the bytecode may do that a later task implements: reported like a runtime error, and
// answered as Yield so the host can say "not supported" rather than "your script failed".
Interpreter::Step Interpreter::not_yet(const std::string &p_what) {
	raised.error.diagnostic = kInternal;
	raised.error.description = kInternalDescription;
	raised.error.message = "This runtime cannot run " + p_what + " yet: " + location_of(frame, pc);
	capture_frames(nullptr);
	stop_outcome = Outcome::Yield;
	return Step::Stop;
}

bool Interpreter::unwind_failure() {
	if (contexts.size() <= run_base) {
		return false;
	}
	const FailureContext context = contexts.back();
	contexts.pop_back();
	// T3.5: replay this context's undo records here, before anything else runs.
	effects.resize(context.effects_mark);
	set_frame(context.frame);
	pc = context.on_failure;
	return true;
}

Outcome Interpreter::run(FrameCell *p_entry, Value &r_result) {
	FrameCell *const saved_frame = frame;
	const uint32_t saved_pc = pc;
	const size_t saved_base = run_base;
	const size_t effects_mark = effects.size();
	run_base = contexts.size();
	set_frame(p_entry);
	pc = 0;

	Outcome outcome = Outcome::Ok;
	for (;;) {
		const ProcedureCell *procedure = frame->procedure;
		if (pc >= procedure->ops.size()) {
			invariant("execution ran past the last op");
			outcome = stop_outcome;
			break;
		}
		const DecodedOp &op = procedure->ops[pc];
		const Step step = execute(op, procedure->operand_words.data() + op.operands);
		if (step == Step::Next) {
			++pc;
			continue;
		}
		if (step == Step::Jumped) {
			continue;
		}
		if (step == Step::Fail) {
			if (unwind_failure()) {
				continue;
			}
			effects.resize(effects_mark);
			outcome = Outcome::Fail;
			break;
		}
		if (step == Step::Finished) {
			r_result = run_result;
			run_result = Value();
			break;
		}
		outcome = stop_outcome;
		break;
	}
	contexts.resize(run_base);
	run_base = saved_base;
	set_frame(saved_frame);
	pc = saved_pc;
	return outcome;
}

Interpreter::Step Interpreter::adapt(std::vector<Value> &r_arguments, uint32_t p_count) {
	const size_t given = r_arguments.size();
	if (given == p_count) {
		return Step::Next;
	}
	// spec/calls.md §3, first matching row.
	if (given == 1) {
		const Value tuple = follow(r_arguments[0]);
		if (is_unbound(tuple)) {
			return park();
		}
		if (is_cell_kind(tuple, CellKind::False) && p_count == 0) {
			r_arguments.clear();
			return Step::Next;
		}
		if (!is_array_value(tuple) || cell_as<ArrayCell>(tuple)->length() != p_count) {
			return invariant("a tuple argument whose length is not the callee's parameter count");
		}
		const ArrayCell *array = cell_as<ArrayCell>(tuple);
		std::vector<Value> elements;
		elements.reserve(p_count);
		for (size_t index = 0; index < array->length(); ++index) {
			elements.push_back(array->get(index));
		}
		r_arguments = std::move(elements);
		return Step::Next;
	}
	if (p_count == 1) {
		const Value tuple = make_array(heap, r_arguments, false);
		r_arguments.assign(1, tuple);
		return Step::Next;
	}
	return invariant("an argument count the callee's parameter count cannot be reconciled with");
}

Interpreter::Step Interpreter::call_native(const NativeProcedureCell *p_native, Value p_self, std::vector<Value> &r_arguments, uint32_t p_dest) {
	const Step adapted = adapt(r_arguments, p_native->positional_count);
	if (adapted != Step::Next) {
		return adapted;
	}
	NativeCall call(heap);
	call.interpreter = this;
	call.procedure = p_native;
	call.self = p_self;
	call.arguments = r_arguments.data();
	call.argument_count = uint32_t(r_arguments.size());
	const Outcome outcome = p_native->implementation(call);
	switch (outcome) {
		case Outcome::Ok:
			native_result = call.result;
			return unify_register(p_dest, call.result);
		case Outcome::Fail:
			return Step::Fail;
		case Outcome::Error:
			return raise(call.error, p_native);
		case Outcome::Park:
			return park();
		case Outcome::Yield:
			return not_yet("a native that suspends its task (T4.1)");
		case Outcome::Invalid:
			break;
	}
	return invariant("arguments outside the contract of the native " + (p_native->decorated_name != nullptr ? p_native->decorated_name->text : std::string()));
}

Interpreter::Step Interpreter::enter(const FunctionCell *p_function, Value p_self, std::vector<Value> &r_arguments,
		const std::vector<NamedArgument> &p_named, FrameCell *p_caller, uint32_t p_return_pc, uint32_t p_return_register) {
	const ProcedureCell *procedure = static_cast<const ProcedureCell *>(p_function->callee);
	const Step adapted = adapt(r_arguments, procedure->positional_count);
	if (adapted != Step::Next) {
		return adapted;
	}
	if (procedure->register_count < 2 + procedure->positional_count) {
		return invariant("a procedure with fewer registers than its parameters need");
	}
	FrameCell *callee = heap.make<FrameCell>();
	callee->procedure = procedure;
	callee->registers.assign(procedure->register_count, Value::empty());
	callee->registers[0] = p_self;
	callee->registers[1] = p_function->parent != nullptr ? Value::from_cell(p_function->parent) : heap.false_value();
	for (uint32_t index = 0; index < procedure->positional_count; ++index) {
		callee->registers[2 + index] = r_arguments[index];
	}
	// spec/calls.md §5.2: by interned-name identity, the first match wins, an unmatched parameter
	// receives uninitialized.
	for (const NamedParameter &parameter : procedure->named_parameters) {
		Value value = Value::uninitialized();
		for (const NamedArgument &argument : p_named) {
			if (argument.name == parameter.name) {
				value = argument.value;
				break;
			}
		}
		callee->registers[parameter.register_index] = value;
	}
	callee->caller = p_caller;
	callee->return_pc = p_return_pc;
	callee->return_register = p_return_register;
	set_frame(callee);
	pc = 0;
	return Step::Jumped;
}

Interpreter::Step Interpreter::call(Value p_callee, Value p_self, bool p_with_self, std::vector<Value> &r_arguments,
		const std::vector<NamedArgument> &p_named, uint32_t p_dest) {
	if (is_cell_kind(p_callee, CellKind::Function)) {
		const FunctionCell *function = cell_as<FunctionCell>(p_callee);
		Value self = function->self;
		if (p_with_self) {
			if (!self.is_uninitialized()) {
				return invariant("CallWithSelf on a function that already has a receiver");
			}
			self = p_self;
		}
		if (function->callee != nullptr && function->callee->kind == CellKind::NativeProcedure) {
			self = follow(self);
			if (is_unbound(self)) {
				return park();
			}
			return call_native(static_cast<const NativeProcedureCell *>(function->callee), self, r_arguments, p_dest);
		}
		if (function->callee == nullptr || function->callee->kind != CellKind::Procedure) {
			return invariant("a function with no procedure");
		}
		return enter(function, self, r_arguments, p_named, frame, pc + 1, p_dest);
	}
	if (p_with_self) {
		if (is_cell_kind(p_callee, CellKind::NativeProcedure)) {
			return call_native(cell_as<NativeProcedureCell>(p_callee), p_self, r_arguments, p_dest);
		}
		return invariant("a CallWithSelf callee that is neither a function nor a native procedure");
	}

	if (r_arguments.size() != 1) {
		return invariant("a non-function callee given other than one argument");
	}
	const Value argument = follow(r_arguments[0]);
	if (is_unbound(argument)) {
		return park();
	}
	if (is_array_value(p_callee)) {
		Value element;
		const Outcome outcome = array_index(p_callee, argument, element);
		return unify_outcome(outcome, p_dest, read_slot(element));
	}
	if (is_map_value(p_callee)) {
		Value element;
		const Outcome outcome = map_lookup(p_callee, argument, element);
		return unify_outcome(outcome, p_dest, read_slot(element));
	}
	if (is_type_cell(p_callee)) {
		bool admits = false;
		const Step tested = type_test(p_callee, argument, admits);
		if (tested != Step::Next) {
			return tested;
		}
		return admits ? unify_register(p_dest, argument) : Step::Fail;
	}
	return invariant(std::string("a callee of kind ") + (p_callee.is_cell() ? cell_kind_name(p_callee.as_cell()->kind) : "immediate"));
}

// spec/ops.md TypeCastFastFail's table; spec/objects.md §13.
Interpreter::Step Interpreter::type_test(Value p_type, Value p_value, bool &r_admits) {
	r_admits = false;
	switch (p_type.as_cell()->kind) {
		case CellKind::Class: {
			const ClassCell *type = cell_as<ClassCell>(p_type);
			if (type->class_kind == ClassKind::Struct) {
				return invariant("a cast to a struct");
			}
			r_admits = is_cell_kind(p_value, CellKind::Object) && class_inherits(cell_as<ObjectCell>(p_value)->object_class, type);
			return Step::Next;
		}
		case CellKind::IntType: {
			const BoundedTypeCell *type = cell_as<BoundedTypeCell>(p_type);
			if (!is_whole(p_value)) {
				return Step::Next;
			}
			bool holds = true;
			if (!type->lower.is_uninitialized() && (value_order(OrderOp::Lte, type->lower, p_value, holds) != Outcome::Ok || !holds)) {
				return Step::Next;
			}
			if (!type->upper.is_uninitialized() && (value_order(OrderOp::Lte, p_value, type->upper, holds) != Outcome::Ok || !holds)) {
				return Step::Next;
			}
			r_admits = true;
			return Step::Next;
		}
		case CellKind::FloatType: {
			const BoundedTypeCell *type = cell_as<BoundedTypeCell>(p_type);
			if (!p_value.is_float() || !type->lower.is_float() || !type->upper.is_float()) {
				return Step::Next;
			}
			const double value = p_value.as_float();
			const double lower = type->lower.as_float();
			const double upper = type->upper.as_float();
			if (std::isnan(value)) {
				r_admits = lower == -HUGE_VAL && std::isnan(upper);
			} else {
				r_admits = lower <= value && (std::isnan(upper) || value <= upper);
			}
			return Step::Next;
		}
		case CellKind::SimpleType:
			if (cell_as<SimpleTypeCell>(p_type)->code == 0) {
				r_admits = true;
				return Step::Next;
			}
			break;
		default:
			break;
	}
	return invariant(std::string("a cast to a ") + cell_kind_name(p_type.as_cell()->kind));
}

ObjectCell *Interpreter::object_operand(Value p_value) {
	if (!is_cell_kind(p_value, CellKind::Object)) {
		return nullptr;
	}
	ObjectCell *object = cell_as<ObjectCell>(p_value);
	if (object->layout == nullptr) {
		layouts.lay_out_value_object(object);
	}
	return object->layout != nullptr ? object : nullptr;
}

Value Interpreter::bind(Value p_function, Value p_receiver) {
	const FunctionCell *method = cell_as<FunctionCell>(p_function);
	FunctionCell *bound = heap.make<FunctionCell>();
	bound->callee = method->callee;
	bound->parent = method->parent;
	bound->self = p_receiver;
	return Value::from_cell(bound);
}

// spec/objects.md §6.
Interpreter::Step Interpreter::load_field(Value p_object, const NameCell *p_name, Value &r_result) {
	ObjectCell *object = object_operand(p_object);
	if (object == nullptr) {
		if (is_cell_kind(p_object, CellKind::Object)) {
			return invariant("a field read from an object with no class");
		}
		return not_yet(std::string("LoadField from a ") + (p_object.is_cell() ? cell_kind_name(p_object.as_cell()->kind) : "value") + " (T3.6)");
	}
	const LayoutField *field = object->layout->find(p_name);
	if (field == nullptr) {
		return invariant("a field " + p_name->text + " the object's layout does not have");
	}
	switch (field->kind) {
		case FieldKind::Slot: {
			Value &slot = object->field_values[field->slot];
			if (slot.is_empty()) {
				slot = Value::from_cell(heap.make<PlaceholderCell>());
			}
			r_result = follow(read_slot(slot));
			return Step::Next;
		}
		case FieldKind::Constant:
			if (is_cell_kind(field->value, CellKind::Function) && cell_as<FunctionCell>(field->value)->self.is_uninitialized()) {
				r_result = bind(field->value, Value::from_cell(object));
			} else {
				r_result = field->value;
			}
			return Step::Next;
		case FieldKind::Accessor:
			break;
	}
	return not_yet("an accessor member (T3.6)");
}

bool Interpreter::resolve_method(Value p_object, const NameCell *p_name, Value &r_function) {
	ObjectCell *object = object_operand(follow(p_object));
	if (object == nullptr || p_name == nullptr) {
		return false;
	}
	const LayoutField *field = object->layout->find(p_name);
	if (field == nullptr || field->kind != FieldKind::Constant || !is_cell_kind(field->value, CellKind::Function) ||
			!cell_as<FunctionCell>(field->value)->self.is_uninitialized()) {
		return false;
	}
	r_function = bind(field->value, Value::from_cell(object));
	return true;
}

Outcome Interpreter::invoke(Value p_function, Value p_self, const std::vector<Value> &p_arguments,
		const std::vector<NamedArgument> &p_named, Value &r_result) {
	const Value callee = follow(p_function);
	if (!is_cell_kind(callee, CellKind::Function)) {
		invariant("an entry into something that is not a function");
		return stop_outcome;
	}
	const FunctionCell *function = cell_as<FunctionCell>(callee);
	const Value self = function->self.is_uninitialized() ? p_self : function->self;
	std::vector<Value> arguments = p_arguments;

	FrameCell *const saved_frame = frame;
	const uint32_t saved_pc = pc;
	frame = nullptr;
	pc = 0;

	if (function->callee != nullptr && function->callee->kind == CellKind::NativeProcedure) {
		const Step step = is_unbound(follow(self))
				? park()
				: call_native(static_cast<const NativeProcedureCell *>(function->callee), follow(self), arguments, kNoRegister);
		set_frame(saved_frame);
		pc = saved_pc;
		if (step == Step::Next) {
			r_result = native_result;
			return Outcome::Ok;
		}
		return step == Step::Fail ? Outcome::Fail : stop_outcome;
	}

	const Step step = enter(function, self, arguments, p_named, nullptr, 0, kNoRegister);
	FrameCell *const entry = frame;
	set_frame(saved_frame);
	pc = saved_pc;
	if (step != Step::Jumped) {
		return step == Step::Fail ? Outcome::Fail : stop_outcome;
	}
	return run(entry, r_result);
}

Outcome Interpreter::construct(const ClassCell *p_class, int64_t p_handle, Value &r_object) {
	ObjectCell *object = layouts.new_object(heap, layouts.get(p_class));
	r_object = Value::from_cell(object);
	RootScope root(heap, &r_object);
	adopting_object = object;
	adopting_handle = p_handle;

	Value token = Value::from_int32(kConstructionMarker);
	Outcome outcome = Outcome::Ok;
	if (p_class->constructor != nullptr) {
		const std::vector<Value> arguments = { Value::from_int32(kConstructionMarker), Value::uninitialized(), Value::uninitialized() };
		outcome = invoke(Value::from_cell(p_class->constructor), r_object, arguments, {}, token);
	}
	token = follow(token);
	if (outcome == Outcome::Ok && !is_int(token)) {
		frame = nullptr;
		not_yet("a deferred accessor setter (T3.6)");
		outcome = stop_outcome;
	}
	if (outcome == Outcome::Ok && p_class->class_kind == ClassKind::Class && p_class->blocks != nullptr) {
		Value ignored;
		outcome = invoke(Value::from_cell(p_class->blocks), r_object, {}, {}, ignored);
	}
	adopting_object = nullptr;
	adopting_handle = 0;
	return outcome;
}

Interpreter::Step Interpreter::execute(const DecodedOp &p_op, const uint32_t *p_words) {
	const uint32_t *const w = p_words;
	switch (VbcOp(p_op.opcode)) {
		case VbcOp::Add:
		case VbcOp::Sub:
		case VbcOp::Mul:
		case VbcOp::Div: {
			const Value left = read(w[1]);
			const Value right = read(w[2]);
			if (is_unbound(left) || is_unbound(right)) {
				return park();
			}
			Value result;
			Outcome outcome;
			switch (VbcOp(p_op.opcode)) {
				case VbcOp::Add:
					outcome = value_add(heap, left, right, result);
					break;
				case VbcOp::Sub:
					outcome = value_sub(heap, left, right, result);
					break;
				case VbcOp::Mul:
					outcome = value_mul(heap, left, right, result);
					break;
				default:
					outcome = value_div(heap, left, right, result);
					break;
			}
			return unify_outcome(outcome, w[0], result);
		}
		case VbcOp::Neg: {
			const Value operand = read(w[1]);
			if (is_unbound(operand)) {
				return park();
			}
			Value result;
			const Outcome outcome = value_neg(heap, operand, result);
			return unify_outcome(outcome, w[0], result);
		}
		case VbcOp::Query: {
			const Value source = read(w[1]);
			if (is_unbound(source)) {
				return park();
			}
			Value result;
			const Outcome outcome = option_query(heap, source, result);
			return unify_outcome(outcome, w[0], result);
		}
		case VbcOp::Err: {
			RuntimeError error;
			error.diagnostic = kInternal;
			error.description = kInternalDescription;
			error.message = kInternalDescription;
			return raise(error, nullptr);
		}
		case VbcOp::Tracepoint:
			return Step::Next;

		case VbcOp::Move:
		case VbcOp::MoveTrailed:
			// T3.5: MoveTrailed records a store into a fresh Dest.
			return unify_register(w[0], read(w[1]));
		case VbcOp::MoveNonComparable: {
			const Value source = read(w[1]);
			Value &slot = frame->registers[w[0]];
			if (slot.is_empty() || is_unbound(follow(slot))) {
				return unify_slot(slot, source);
			}
			Binder binder;
			return values_equal(follow(slot), source, &binder) == Equality::Eq ? Step::Next : Step::Fail;
		}
		case VbcOp::Reset:
		case VbcOp::ResetNonTrailed:
			// T3.5: Reset records the register's old content.
			frame->registers[w[0]] = Value::empty();
			return Step::Next;

		case VbcOp::Jump:
			pc = w[0];
			return Step::Jumped;
		case VbcOp::JumpIfInitialized: {
			const uint32_t source = w[0];
			bool initialized = true;
			// A fresh register would read as a placeholder, which is not uninitialized.
			if (source == kAbsentOperand || (source & 1) != 0 || !frame->registers[source >> 1].is_empty()) {
				initialized = !read(source).is_uninitialized();
			}
			if (initialized) {
				pc = w[1];
				return Step::Jumped;
			}
			return Step::Next;
		}
		case VbcOp::Switch: {
			const Value which = read(w[0]);
			const uint32_t count = variadic_count(w[1]);
			if (!which.is_int32() || which.as_int32() < 0 || uint32_t(which.as_int32()) >= count) {
				return invariant("a Switch index that is not an integer below the label count");
			}
			pc = variadic_items(w[1])[which.as_int32()];
			return Step::Jumped;
		}

		case VbcOp::LtFastFail:
		case VbcOp::LteFastFail:
		case VbcOp::GtFastFail:
		case VbcOp::GteFastFail:
		case VbcOp::Lt:
		case VbcOp::Lte:
		case VbcOp::Gt:
		case VbcOp::Gte: {
			const bool fast = p_op.opcode <= uint16_t(VbcOp::GteFastFail);
			const Value left = read(w[fast ? 2 : 1]);
			const Value right = read(w[fast ? 3 : 2]);
			if (is_unbound(left) || is_unbound(right)) {
				return park();
			}
			OrderOp order = OrderOp::Lt;
			switch (VbcOp(p_op.opcode)) {
				case VbcOp::LteFastFail:
				case VbcOp::Lte:
					order = OrderOp::Lte;
					break;
				case VbcOp::GtFastFail:
				case VbcOp::Gt:
					order = OrderOp::Gt;
					break;
				case VbcOp::GteFastFail:
				case VbcOp::Gte:
					order = OrderOp::Gte;
					break;
				default:
					break;
			}
			bool holds = false;
			if (value_order(order, left, right, holds) != Outcome::Ok) {
				return invariant("an ordering of values that have none");
			}
			if (!holds) {
				if (fast) {
					pc = w[4];
					return Step::Jumped;
				}
				return Step::Fail;
			}
			return unify_register(w[0], left);
		}
		case VbcOp::EqFastFail:
		case VbcOp::NeqFastFail: {
			const Value left = read(w[2]);
			const Value right = read(w[3]);
			if (is_unbound(left) || is_unbound(right)) {
				return park();
			}
			const Equality answer = values_equal(left, right);
			const bool holds = VbcOp(p_op.opcode) == VbcOp::EqFastFail ? answer == Equality::Eq : answer == Equality::Neq;
			if (!holds) {
				pc = w[4];
				return Step::Jumped;
			}
			return unify_register(w[0], left);
		}
		case VbcOp::Neq: {
			const Value left = read(w[1]);
			const Value right = read(w[2]);
			Recorder recorder;
			const Equality answer = values_equal(left, right, &recorder);
			if (answer == Equality::Neq) {
				return unify_register(w[0], left);
			}
			if (answer == Equality::Eq && recorder.met) {
				return park();
			}
			return Step::Fail;
		}
		case VbcOp::ArrayIndexFastFail: {
			const Value array = read(w[2]);
			const Value index = read(w[3]);
			if (is_unbound(array) || is_unbound(index)) {
				return park();
			}
			Value element;
			const Outcome outcome = array_index(array, index, element);
			if (outcome == Outcome::Fail) {
				pc = w[4];
				return Step::Jumped;
			}
			return unify_outcome(outcome, w[0], read_slot(element));
		}
		case VbcOp::TypeCastFastFail: {
			const Value type = read(w[2]);
			const Value value = read(w[3]);
			if (is_unbound(type) || is_unbound(value)) {
				return park();
			}
			if (!is_type_cell(type)) {
				return invariant("a cast to a value that is not a type");
			}
			bool admits = false;
			const Step tested = type_test(type, value, admits);
			if (tested != Step::Next) {
				return tested;
			}
			if (!admits) {
				pc = w[4];
				return Step::Jumped;
			}
			return unify_register(w[0], value);
		}
		case VbcOp::QueryFastFail: {
			const Value source = read(w[2]);
			if (is_unbound(source)) {
				return park();
			}
			Value result;
			const Outcome outcome = option_query(heap, source, result);
			if (outcome == Outcome::Fail) {
				pc = w[3];
				return Step::Jumped;
			}
			return unify_outcome(outcome, w[0], result);
		}
		case VbcOp::EndFastFailureContext:
			// Nothing parks in stage 1, so the indicator is always fresh; it must not be read here.
			return Step::Next;
		case VbcOp::CanFastAppendToArrayFastFail: {
			const Value ref = read(w[1]);
			const Value array = read(w[2]);
			if (is_unbound(ref) || is_unbound(array)) {
				return park();
			}
			const bool has_domain = is_cell_kind(ref, CellKind::Ref) && !cell_as<RefCell>(ref)->domain.is_uninitialized();
			if (!is_cell_kind(array, CellKind::MutableArray) || has_domain) {
				pc = w[3];
				return Step::Jumped;
			}
			return Step::Next;
		}
		case VbcOp::FastAppendToArray: {
			const Value left = read(w[0]);
			const Value right = read(w[1]);
			if (is_unbound(left) || is_unbound(right)) {
				return park();
			}
			Value parked;
			// T3.5: each append is recorded.
			return unify_outcome(array_fast_append(heap, left, right, parked), kNoRegister, Value());
		}

		case VbcOp::BeginFailureContext: {
			// T3.5: a context owns its undo log from here; without one a failure undoes nothing but
			// the deferred effects queued inside it.
			FailureContext context;
			context.frame = frame;
			context.on_failure = w[0];
			context.effects_mark = effects.size();
			contexts.push_back(context);
			return Step::Next;
		}
		case VbcOp::EndFailureContext:
			if (contexts.size() <= run_base) {
				return invariant("EndFailureContext with no failure context open");
			}
			contexts.pop_back();
			return Step::Next;

		case VbcOp::SelfTask:
		case VbcOp::BeginTask:
		case VbcOp::CallTask:
		case VbcOp::EndTask:
		case VbcOp::Yield:
		case VbcOp::NewSemaphore:
		case VbcOp::WaitSemaphore:
		case VbcOp::ResumeUnwind:
			return not_yet("tasks (T4.1)");
		case VbcOp::BeginAwait:
		case VbcOp::AwaitSuccess:
		case VbcOp::EndAwait:
		case VbcOp::BeginBatch:
		case VbcOp::EndBatch:
		case VbcOp::RefSetLive:
		case VbcOp::CallSetLive:
		case VbcOp::SetFieldLive:
			return not_yet("await, batch and live variables (T4.3)");

		case VbcOp::Call:
		case VbcOp::CallWithSelf: {
			const bool with_self = VbcOp(p_op.opcode) == VbcOp::CallWithSelf;
			const uint32_t base = with_self ? 1 : 0;
			const Value callee = read(w[1]);
			if (is_unbound(callee)) {
				return park();
			}
			Value self;
			if (with_self) {
				self = read(w[2]);
				if (is_unbound(self)) {
					return park();
				}
			}
			std::vector<Value> arguments;
			const uint32_t count = variadic_count(w[2 + base]);
			arguments.reserve(count);
			for (uint32_t index = 0; index < count; ++index) {
				arguments.push_back(read(variadic_items(w[2 + base])[index]));
			}
			std::vector<NamedArgument> named;
			const uint32_t named_count = variadic_count(w[3 + base]);
			if (named_count != variadic_count(w[4 + base])) {
				return invariant("named arguments and their values of different lengths");
			}
			for (uint32_t index = 0; index < named_count; ++index) {
				NamedArgument argument;
				const Value name = constant(variadic_items(w[3 + base])[index]);
				argument.name = is_cell_kind(name, CellKind::Name) ? cell_as<NameCell>(name) : nullptr;
				argument.value = read(variadic_items(w[4 + base])[index]);
				named.push_back(argument);
			}
			return call(callee, self, with_self, arguments, named, w[0]);
		}
		case VbcOp::Return:
		case VbcOp::ReturnTrailed: {
			// T3.5: ReturnTrailed records both of its stores.
			const Value value = read(w[0]);
			FrameCell *const done = frame;
			if (done->caller == nullptr) {
				run_result = value;
				return Step::Finished;
			}
			set_frame(done->caller);
			pc = done->return_pc;
			const Step step = unify_register(done->return_register, value);
			return step == Step::Next ? Step::Jumped : step;
		}

		case VbcOp::NewRef:
		case VbcOp::NewPersistentOrSessionWeakMapRef: {
			if (VbcOp(p_op.opcode) == VbcOp::NewRef && module_top_level > 0) {
				RuntimeError error;
				error.diagnostic = kGlobalVariable;
				error.description = kGlobalVariableDescription;
				error.message = "Can't create a var at module scope.";
				return raise(error, nullptr);
			}
			RefCell *ref = heap.make<RefCell>(Value::empty());
			if (VbcOp(p_op.opcode) == VbcOp::NewRef) {
				const Value domain = read(w[1]);
				if (!domain.is_uninitialized()) {
					ref->domain = domain;
				}
			}
			return unify_register(w[0], Value::from_cell(ref));
		}
		case VbcOp::RefGet: {
			const Value ref = read(w[1]);
			if (is_unbound(ref)) {
				return park();
			}
			if (!is_cell_kind(ref, CellKind::Ref)) {
				return not_yet("a reference that is not a Verse variable (T3.6)");
			}
			const Value content = cell_as<RefCell>(ref)->content;
			if (content.is_empty()) {
				return invariant("a read of a variable nothing has written");
			}
			return unify_register(w[0], content);
		}
		case VbcOp::RefSet: {
			const Value ref = read(w[0]);
			if (is_unbound(ref)) {
				return park();
			}
			if (!is_cell_kind(ref, CellKind::Ref)) {
				return not_yet("a write through a reference that is not a Verse variable (T3.6)");
			}
			// T3.5 records the old content; T4.3 cancels the live task and resumes awaiters.
			cell_as<RefCell>(ref)->content = read(w[1]);
			return Step::Next;
		}
		case VbcOp::RefCallDomain: {
			const Value ref = read(w[1]);
			if (is_unbound(ref)) {
				return park();
			}
			const Value argument = read(w[2]);
			if (is_cell_kind(ref, CellKind::Ref) && !cell_as<RefCell>(ref)->domain.is_uninitialized()) {
				const Value domain = follow(cell_as<RefCell>(ref)->domain);
				std::vector<Value> arguments = { argument };
				return call(domain, Value(), false, arguments, {}, w[0]);
			}
			return unify_register(w[0], argument);
		}
		case VbcOp::Freeze:
		case VbcOp::FreezeIfAccessor: {
			const Value value = read(w[1]);
			if (is_unbound(value)) {
				return park();
			}
			if (VbcOp(p_op.opcode) == VbcOp::FreezeIfAccessor) {
				return unify_register(w[0], value);
			}
			Value result;
			const Outcome outcome = freeze(heap, value, result);
			return unify_outcome(outcome, w[0], result);
		}
		case VbcOp::Melt: {
			Value result;
			const Outcome outcome = melt(heap, read(w[1]), result);
			return unify_outcome(outcome, w[0], result);
		}
		case VbcOp::Length:
		case VbcOp::LengthWithEffects: {
			Value container = read(w[1]);
			if (VbcOp(p_op.opcode) == VbcOp::LengthWithEffects && is_cell_kind(container, CellKind::Ref)) {
				container = follow(cell_as<RefCell>(container)->content);
			}
			if (is_unbound(container)) {
				return park();
			}
			int64_t length = 0;
			const Outcome outcome = value_length(container, length);
			return unify_outcome(outcome, w[0], make_int(heap, length));
		}
		case VbcOp::CallSet: {
			const Value container = read(w[0]);
			const Value index = read(w[1]);
			if (is_unbound(container) || is_unbound(index)) {
				return park();
			}
			const Value value = read(w[2]);
			// T3.5 records what each write replaced; T4.3 writes through a hidden variable.
			if (is_cell_kind(container, CellKind::MutableArray)) {
				Value old;
				return unify_outcome(array_set(container, index, value, old), kNoRegister, Value());
			}
			if (is_cell_kind(container, CellKind::MutableMap)) {
				bool inserted = false;
				Value old;
				return unify_outcome(map_set(container, index, value, inserted, old), kNoRegister, Value());
			}
			return not_yet(std::string("CallSet on a ") + (container.is_cell() ? cell_kind_name(container.as_cell()->kind) : "value") + " (T3.6)");
		}
		case VbcOp::NewArray:
		case VbcOp::NewMutableArray: {
			std::vector<Value> elements;
			const uint32_t count = variadic_count(w[1]);
			elements.reserve(count);
			for (uint32_t index = 0; index < count; ++index) {
				elements.push_back(read(variadic_items(w[1])[index]));
			}
			return unify_register(w[0], make_array(heap, elements, VbcOp(p_op.opcode) == VbcOp::NewMutableArray));
		}
		case VbcOp::ArrayAdd: {
			const Value container = read(w[1]);
			if (is_unbound(container)) {
				return park();
			}
			// T3.5: recorded when bTransactional (w[3]) is set.
			const Outcome outcome = array_append(container, read(w[2]));
			return unify_outcome(outcome, w[0], container);
		}
		case VbcOp::InPlaceMakeImmutable: {
			const Value container = read(w[1]);
			if (is_unbound(container)) {
				return park();
			}
			// T3.5: recorded.
			return unify_outcome(array_make_immutable(container), w[0], container);
		}
		case VbcOp::NewOption:
			return unify_register(w[0], make_option(heap, read(w[1])));
		case VbcOp::NewMap: {
			const uint32_t count = variadic_count(w[1]);
			if (count != variadic_count(w[2])) {
				return invariant("map keys and values of different lengths");
			}
			std::vector<Value> keys;
			std::vector<Value> values;
			for (uint32_t index = 0; index < count; ++index) {
				const Value key = read(variadic_items(w[1])[index]);
				if (is_unbound(key)) {
					return park();
				}
				keys.push_back(key);
				values.push_back(read(variadic_items(w[2])[index]));
			}
			Value result;
			const Outcome outcome = make_map(heap, keys, values, result);
			return unify_outcome(outcome, w[0], result);
		}
		case VbcOp::MapKey:
		case VbcOp::MapValue: {
			const Value map = read(w[1]);
			const Value index = read(w[2]);
			if (is_unbound(map) || is_unbound(index)) {
				return park();
			}
			Value result;
			const Outcome outcome = VbcOp(p_op.opcode) == VbcOp::MapKey ? map_key_at(map, index, result) : map_value_at(map, index, result);
			return unify_outcome(outcome, w[0], read_slot(result));
		}

		case VbcOp::NewClass:
			return not_yet("NewClass, which appears only in package procedures");
		case VbcOp::BindNativeClass: {
			if (is_unbound(read(w[0]))) {
				return park();
			}
			return Step::Next;
		}
		case VbcOp::ConstructNativeDefaultObject:
			// spec/objects.md §8.3: host-built objects run their constructor instead.
			return Step::Next;
		case VbcOp::LoadImport:
			return invariant("LoadImport, which only @import_as produces");
		case VbcOp::JumpIfDefaultSubObject:
			if (is_unbound(read(w[0]))) {
				return invariant("JumpIfDefaultSubObject on an unbound object");
			}
			return Step::Next;
		case VbcOp::BeginModule: {
			const Value package = read(w[1]);
			if (is_unbound(package)) {
				return park();
			}
			ModuleCell *module = heap.make<ModuleCell>();
			const Value name = constant(w[2]);
			module->name = is_cell_kind(name, CellKind::Name) ? cell_as<NameCell>(name) : nullptr;
			++module_top_level;
			return unify_register(w[0], Value::from_cell(module));
		}
		case VbcOp::EndModule:
			if (module_top_level > 0) {
				--module_top_level;
			}
			return Step::Next;
		case VbcOp::EndModuleData:
			if (is_unbound(read(w[1]))) {
				return park();
			}
			return Step::Next;

		case VbcOp::NewObject: {
			const Value archetype = read(w[1]);
			const Value type = read(w[2]);
			if (is_unbound(archetype) || is_unbound(type)) {
				return park();
			}
			if (!is_cell_kind(type, CellKind::Class) || !is_cell_kind(archetype, CellKind::Archetype)) {
				return invariant("NewObject of something that is not a class and an archetype");
			}
			ObjectCell *object = layouts.new_object(heap, layouts.get(cell_as<ClassCell>(type)));
			return unify_register(w[0], Value::from_cell(object));
		}
		case VbcOp::LoadField: {
			const Value object = read(w[1]);
			if (is_unbound(object)) {
				return park();
			}
			const Value name = constant(w[2]);
			Value result;
			const Step step = load_field(object, cell_as<NameCell>(name), result);
			return step == Step::Next ? unify_register(w[0], result) : step;
		}
		case VbcOp::LoadFieldFromSuper: {
			const Value scope = read(w[1]);
			const Value self = read(w[2]);
			if (is_unbound(scope) || is_unbound(self)) {
				return park();
			}
			if (!is_cell_kind(scope, CellKind::Scope) || !is_cell_kind(self, CellKind::Object) || cell_as<ObjectCell>(self)->is_struct()) {
				return invariant("(super:) from something that is not a scope and an object");
			}
			const ScopeCell *root = cell_as<ScopeCell>(scope);
			while (root->parent != nullptr) {
				root = root->parent;
			}
			if (root->captures.size() != 1 || !is_cell_kind(root->captures[0], CellKind::Archetype)) {
				return invariant("(super:) from a scope that is not a class scope");
			}
			const ClassCell *defining = cell_as<ArchetypeCell>(root->captures[0])->owner;
			const NameCell *name = cell_as<NameCell>(constant(w[3]));
			std::vector<const ClassCell *> order;
			class_body_order(defining, order);
			// spec/calls.md §7.4: skipping the defining class itself.
			for (size_t index = 1; index < order.size(); ++index) {
				for (const ArchetypeEntry &entry : order[index]->archetype->entries) {
					if (entry.name == name && is_cell_kind(entry.value, CellKind::Function) && cell_as<FunctionCell>(entry.value)->self.is_uninitialized()) {
						return unify_register(w[0], bind(entry.value, self));
					}
				}
			}
			return invariant("(super:) finding no ancestor method " + name->text);
		}
		case VbcOp::CreateField: {
			const Value token = read(w[1]);
			const Value value = read(w[2]);
			if (is_unbound(token) || is_unbound(value)) {
				return park();
			}
			ObjectCell *object = object_operand(value);
			if (object == nullptr) {
				return invariant("CreateField on something that is not an object");
			}
			const NameCell *name = cell_as<NameCell>(constant(w[3]));
			const LayoutField *field = object->layout->find(name);
			if (field == nullptr) {
				return invariant("CreateField of a name " + name->text + " the object's layout does not have");
			}
			if (field->kind != FieldKind::Constant && !object->created[field->slot]) {
				object->created[field->slot] = true;
				return Step::Next;
			}
			pc = w[4];
			return Step::Jumped;
		}
		case VbcOp::UnifyField:
		case VbcOp::SetField: {
			const Value value = read(w[0]);
			if (is_unbound(value)) {
				return park();
			}
			ObjectCell *object = object_operand(value);
			if (object == nullptr) {
				return not_yet("a field write on something that is not an object (T3.6)");
			}
			const NameCell *name = cell_as<NameCell>(constant(w[1]));
			const LayoutField *field = object->layout->find(name);
			if (field == nullptr || field->kind != FieldKind::Slot) {
				return invariant("a field write to " + name->text + ", which is not a slot of the object");
			}
			Value &slot = object->field_values[field->slot];
			if (VbcOp(p_op.opcode) == VbcOp::SetField) {
				// T3.5 records what is replaced.
				if (is_cell_kind(slot, CellKind::Ref)) {
					cell_as<RefCell>(slot)->content = read(w[2]);
				} else {
					slot = read(w[2]);
				}
				return Step::Next;
			}
			return unify_slot(slot, read(w[2]));
		}
		case VbcOp::InitializeVar: {
			const Value token = read(w[1]);
			const Value value = read(w[2]);
			if (is_unbound(value)) {
				return park();
			}
			const NameCell *name = cell_as<NameCell>(constant(w[3]));
			if (w[6] != 0 && module_top_level > 0) {
				RuntimeError error;
				error.diagnostic = kGlobalVariable;
				error.description = kGlobalVariableDescription;
				error.message = "Can't allocate mutable var field " + name->text + " while initializing module.";
				return raise(error, nullptr);
			}
			ObjectCell *object = object_operand(value);
			if (object == nullptr) {
				return invariant("InitializeVar on something that is not an object");
			}
			const LayoutField *field = object->layout->find(name);
			if (field == nullptr || field->kind == FieldKind::Constant) {
				return invariant("InitializeVar of " + name->text + ", which is not a slot of the object");
			}
			if (field->kind == FieldKind::Accessor) {
				return not_yet("a deferred accessor setter (T3.6)");
			}
			RefCell *ref = heap.make<RefCell>(read(w[4]));
			const Value domain = read(w[5]);
			if (!domain.is_uninitialized()) {
				ref->domain = domain;
			}
			const Step stored = unify_slot(object->field_values[field->slot], Value::from_cell(ref));
			if (stored != Step::Next) {
				return stored;
			}
			return unify_register(w[0], token);
		}
		case VbcOp::UnifyNativeObject: {
			const Value token = read(w[0]);
			const Value value = read(w[1]);
			if (is_unbound(token) || is_unbound(value)) {
				return park();
			}
			if (!is_int(token)) {
				return not_yet("a deferred accessor setter (T3.6)");
			}
			ObjectCell *object = object_operand(value);
			if (object == nullptr) {
				return invariant("UnifyNativeObject on something that is not an object");
			}
			const ClassCell *actual = object->object_class;
			if (actual->class_kind != ClassKind::Class || actual->blocks == nullptr) {
				return Step::Next;
			}
			std::vector<Value> none;
			return enter(actual->blocks, value, none, {}, frame, pc + 1, kNoRegister);
		}
		case VbcOp::UnwrapNativeConstructorWrapper: {
			const Value value = read(w[1]);
			if (is_unbound(value)) {
				return park();
			}
			return unify_register(w[0], value);
		}
		case VbcOp::LoadConstructor: {
			const Value type = read(w[1]);
			if (is_unbound(type)) {
				return park();
			}
			if (!is_cell_kind(type, CellKind::Class) || cell_as<ClassCell>(type)->constructor == nullptr) {
				return invariant("LoadConstructor of something with no constructor");
			}
			return unify_register(w[0], Value::from_cell(cell_as<ClassCell>(type)->constructor));
		}

		case VbcOp::NewScope: {
			const Value parent = read(w[1]);
			ScopeCell *scope = heap.make<ScopeCell>();
			scope->parent = is_cell_kind(parent, CellKind::Scope) ? cell_as<ScopeCell>(parent) : nullptr;
			const uint32_t count = variadic_count(w[2]);
			for (uint32_t index = 0; index < count; ++index) {
				scope->captures.push_back(read(variadic_items(w[2])[index]));
			}
			return unify_register(w[0], Value::from_cell(scope));
		}
		case VbcOp::NewFunction: {
			const Value procedure = read(w[1]);
			const Value parent = read(w[3]);
			if (is_unbound(procedure) || is_unbound(parent)) {
				return park();
			}
			if (!is_cell_kind(procedure, CellKind::Procedure) && !is_cell_kind(procedure, CellKind::NativeProcedure)) {
				return invariant("NewFunction of something that is not a procedure");
			}
			FunctionCell *function = heap.make<FunctionCell>();
			function->callee = procedure.as_cell();
			function->self = w[2] == kAbsentOperand ? Value::uninitialized() : read(w[2]);
			function->parent = is_cell_kind(parent, CellKind::Scope) ? cell_as<ScopeCell>(parent) : nullptr;
			return unify_register(w[0], Value::from_cell(function));
		}
		case VbcOp::LoadParentScope: {
			const Value scope = read(w[1]);
			if (is_unbound(scope)) {
				return park();
			}
			if (!is_cell_kind(scope, CellKind::Scope) || cell_as<ScopeCell>(scope)->parent == nullptr) {
				return invariant("LoadParentScope of a scope with no parent");
			}
			return unify_register(w[0], Value::from_cell(cell_as<ScopeCell>(scope)->parent));
		}
		case VbcOp::LoadCapture: {
			const Value scope = read(w[1]);
			if (is_unbound(scope)) {
				return park();
			}
			if (!is_cell_kind(scope, CellKind::Scope) || w[2] >= cell_as<ScopeCell>(scope)->captures.size()) {
				return invariant("LoadCapture of a capture the scope does not have");
			}
			return unify_register(w[0], cell_as<ScopeCell>(scope)->captures[w[2]]);
		}
		case VbcOp::BeginProfileBlock:
			return unify_register(w[0], Value::from_int32(0));
		case VbcOp::EndProfileBlock:
			for (const uint32_t operand : { w[0], w[1], w[3], w[4], w[5], w[6] }) {
				if (is_unbound(read(operand))) {
					return invariant("EndProfileBlock with an unbound operand");
				}
			}
			return Step::Next;

		default:
			break;
	}
	return invariant(std::string("the op ") + vbc::kVbcOps[p_op.opcode].name + ", which a loaded program never holds");
}

} // namespace vm
