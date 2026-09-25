#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "verse_host_abi.h"
#include "vm_cell.h"
#include "vm_heap.h"
#include "vm_loader.h"
#include "vm_objects.h"
#include "vm_status.h"
#include "vm_value.h"

// The interpreter core (design §7.4): a switch loop over decoded ops, frames on the heap, so a
// Verse-to-Verse call never recurses on the native stack -- the native stack deepens only when a
// native calls back into Verse.
namespace vm {

struct Sidecar;

struct ErrorFrame {
	std::string function;
	std::string path;
	int32_t line = 0;
};

// A runtime error with its call stack, innermost frame first (spec/failure.md §9.2).
struct RaisedError {
	RuntimeError error;
	std::vector<ErrorFrame> frames;

	// `<Name>: <Description> (<Message>)`, or without the parentheses when the message is empty.
	std::string message_line() const;
};

struct NamedArgument {
	const NameCell *name = nullptr;
	Value value;
};

class Interpreter {
public:
	Interpreter(Heap &r_heap, const Program &p_program);

	Heap &heap;
	const Program &program;
	const Sidecar *sidecar = nullptr;
	vh_godot_api godot = {};

	// A VM entry (spec/failure.md §1): a root full failure context and a fresh entry task
	// (spec/tasks.md §4.4). Every run between begin_entry and end_entry shares the root, so the
	// deferred effects of all of them run, in order, at an outermost end_entry(true) and are
	// dropped by an end_entry(false). Entries nest when a native calls back into Verse.
	void begin_entry();
	void end_entry(bool p_commit);
	// The collector must not run while this is true: an op's operands, a call's arguments and a
	// native's result live in C++ locals between allocations. Only the entry tasks, which hold the
	// frames, are rooted.
	bool in_entry() const { return !entry_tasks.empty(); }

	// Calls p_function to completion (spec/calls.md §8). An unbound function is entered with
	// p_self as its receiver. Ok leaves the result in r_result; Fail is the root context failing;
	// Error and Yield leave error() describing what stopped the run -- Yield is a suspension this
	// runtime cannot perform yet.
	Outcome invoke(Value p_function, Value p_self, const std::vector<Value> &p_arguments,
			const std::vector<NamedArgument> &p_named, Value &r_result);

	// A host-built object (spec/objects.md §7.11): NewObject with no archetype entries, the
	// constructor with (marker, uninitialized, uninitialized), then the deferred setters and the
	// blocks. The object adopts p_handle when its vh_object block asks (godot-natives.md §4.1).
	Outcome construct(const ClassCell *p_class, int64_t p_handle, Value &r_object);

	// The method p_name resolves to on p_object, bound to it, or false when the layout has no
	// method of that name.
	bool resolve_method(Value p_object, const NameCell *p_name, Value &r_function);

	Layouts layouts;

	// Queues an outside effect for the root commit (spec/failure.md §11, the defer pattern).
	void defer(std::function<void()> p_effect);

	const RaisedError &error() const { return raised; }
	uint64_t park_count = 0;

	// godot-natives.md §4.1's pending adoption: consumed by the one object it names.
	const Cell *adopting_object = nullptr;
	int64_t adopting_handle = 0;
	// §4.4: every peer this VM minted, and the object that minted it. Not a root: the row is what a
	// sweep of that object releases.
	std::unordered_map<int64_t, const Cell *> minted_peers;
	// The Godot name of a mirrored class, from the generated table; null answers no mint.
	const char *(*mirrored_godot_name)(std::string_view p_verse_name) = nullptr;

private:
	enum class Step : uint8_t {
		Next,
		Jumped,
		Fail,
		Stop,
		Finished,
	};

	struct FailureContext {
		FrameCell *frame = nullptr;
		uint32_t on_failure = 0;
		size_t effects_mark = 0;
	};

	FrameCell *frame = nullptr;
	uint32_t pc = 0;
	size_t run_base = 0;
	Value run_result;
	Value native_result;
	Outcome stop_outcome = Outcome::Ok;
	std::vector<FailureContext> contexts;
	std::vector<std::function<void()>> effects;
	std::deque<Value> entry_tasks;
	RaisedError raised;
	uint32_t module_top_level = 0;

	TaskCell *current_task() const;
	void set_frame(FrameCell *p_frame);

	Outcome run(FrameCell *p_entry, Value &r_result);
	Step execute(const DecodedOp &p_op, const uint32_t *p_words);
	bool unwind_failure();

	Value read(uint32_t p_word);
	uint32_t variadic_count(uint32_t p_word) const;
	const uint32_t *variadic_items(uint32_t p_word) const;
	Value constant(uint32_t p_index) const;
	Step unify_register(uint32_t p_register, Value p_value);
	Step unify_slot(Value &r_slot, Value p_value);
	Step unify_outcome(Outcome p_outcome, uint32_t p_register, Value p_value);

	Step park();
	Step invariant(const std::string &p_what);
	Step raise(const RuntimeError &p_error, const NativeProcedureCell *p_native);
	Step not_yet(const std::string &p_what);
	void capture_frames(const NativeProcedureCell *p_native);

	Step call(Value p_callee, Value p_self, bool p_with_self, std::vector<Value> &r_arguments,
			const std::vector<NamedArgument> &p_named, uint32_t p_dest);
	Step call_native(const NativeProcedureCell *p_native, Value p_self, std::vector<Value> &r_arguments, uint32_t p_dest);
	Step enter(const FunctionCell *p_function, Value p_self, std::vector<Value> &r_arguments,
			const std::vector<NamedArgument> &p_named, FrameCell *p_caller, uint32_t p_return_pc, uint32_t p_return_register);
	Step adapt(std::vector<Value> &r_arguments, uint32_t p_count);

	Step type_test(Value p_type, Value p_value, bool &r_admits);
	Step load_field(Value p_object, const NameCell *p_name, Value &r_result);
	ObjectCell *object_operand(Value p_value);
	Value bind(Value p_function, Value p_receiver);
};

} // namespace vm
