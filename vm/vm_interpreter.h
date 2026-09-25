#pragma once

#include <cstdint>
#include <functional>
#include <random>
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
class GodotBridge;

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

// Seconds on a monotonic clock, what Interpreter::clock answers unless a test replaces it.
double monotonic_seconds();
// Unix time in seconds, with a fraction.
double wall_clock_seconds();

struct NamedArgument {
	const NameCell *name = nullptr;
	Value value;
};

// A Verse frame of the running call stack and the op it is at.
struct StackFrame {
	const FrameCell *frame = nullptr;
	uint32_t op = 0;
};

// Its roots are what it holds between entries (design §7.3): the sleepers, the layouts, and the
// registers of an entry the host is inside. Everything an entry holds only in C++ locals is not a
// root, which is why the collector must never run while in_entry() is true.
class Interpreter : public RootSource {
public:
	Interpreter(Heap &r_heap, const Program &p_program);
	~Interpreter() override;
	Interpreter(const Interpreter &) = delete;
	Interpreter &operator=(const Interpreter &) = delete;

	Heap &heap;
	const Program &program;
	const Sidecar *sidecar = nullptr;
	vh_godot_api godot = {};
	GodotBridge *bridge = nullptr;

	// A VM entry (spec/failure.md §1): a root full failure context. Entries nest when a native calls
	// back into Verse, and a nested one is a transaction inside whatever context the native was
	// called in: end_entry(false) at any depth rolls back what that entry did, and only an outermost
	// end_entry(true) commits -- running the deferred effects of every run in it, in order. An
	// outermost end_entry after a raise terminates the content scope that was active where it was
	// raised (spec/tasks.md §8.4).
	void begin_entry();
	void end_entry(bool p_commit);
	// The collector must not run while this is true: an op's operands, a call's arguments and a
	// native's result live in C++ locals between allocations.
	bool in_entry() const { return !entry_marks.empty(); }

	// Calls p_function in a fresh entry task (spec/calls.md §8, spec/tasks.md §4.4). An unbound
	// function is entered with p_self as its receiver. Ok leaves the result in r_result; Yield is
	// the entry task suspending, with no result; Fail is the root context failing; Error and
	// Unsupported leave error() describing what stopped the run.
	Outcome invoke(Value p_function, Value p_self, const std::vector<Value> &p_arguments,
			const std::vector<NamedArgument> &p_named, Value &r_result);

	// spec/tasks.md §8.1. Root tasks started while a scope is active join its group; a native's
	// suspension captures it. Null is no scope: nothing joins a group.
	ContentScopeCell *active_scope = nullptr;
	ContentScopeCell *make_scope();
	// §8.2: terminates every root task in the group and marks the scope terminated. Not undoable,
	// and never called mid-run.
	void terminate_scope(ContentScopeCell *p_scope);

	// The task whose ops are executing, or null outside a run. A native that answers Yield suspends
	// it, having first recorded it wherever its completion will come from.
	TaskCell *current_task() const { return task; }
	// A native completing a call it suspended (spec/tasks.md §11): p_task resumes synchronously with
	// p_value, and so does everything it resumes, before this returns. Nothing happens when its
	// captured scope was terminated or it is no longer Active. Error propagates the raise.
	Outcome complete(TaskCell *p_task, Value p_value);
	// An event(t)'s Signal made from outside Verse (spec/natives.md §7.2): its awaiters resume, then
	// its subscribers run, before this returns.
	Outcome signal_event(Value p_event, Value p_payload);

	// The natives of `task(t)` and `event(t)` (spec/natives.md §7, §8) and `Sleep`
	// (godot-natives.md §10), by binding key, or null.
	static NativeFn task_native(std::string_view p_binding_key);

	// godot-natives.md §10: a task suspended in Sleep until `deadline` on `clock`. A sleeper that was
	// cancelled stays here until it is due, and its wake then does nothing (spec/tasks.md §11.1).
	struct Sleeper {
		TaskCell *task = nullptr;
		double deadline = 0.0;
	};
	std::vector<Sleeper> sleepers;
	// Monotonic seconds, blind to Godot's time scale and pause; a test substitutes its own.
	double (*clock)() = nullptr;
	// Removes every sleeper due at p_now and answers them earliest deadline first, ties in the order
	// they began to sleep.
	std::vector<TaskCell *> take_due_sleepers(double p_now);

	// A host-built object (spec/objects.md §7.11): NewObject with no archetype entries, the
	// constructor with (marker, uninitialized, uninitialized), then the deferred setters and the
	// blocks. The object adopts p_handle when its vh_object block asks (godot-natives.md §4.1).
	// Without p_run_blocks it stops after the setters, as a class default object does (§8.3).
	Outcome construct(const ClassCell *p_class, int64_t p_handle, Value &r_object, bool p_run_blocks = true);

	// The method p_name resolves to on p_object, bound to it, or false when the layout has no
	// method of that name.
	bool resolve_method(Value p_object, const NameCell *p_name, Value &r_function);

	Layouts layouts;

	// Queues an outside effect for the root commit (spec/failure.md §11, the defer pattern).
	void defer(std::function<void()> p_effect);
	// Registers the undo of an outside effect already performed (spec/failure.md §11, the
	// compensate pattern): run if the current transaction, or an ancestor it merged into, aborts,
	// after the undo log is replayed and most recent first; dropped at the root commit.
	void compensate(std::function<void()> p_action);
	// godot-natives.md §4.4 and §8.12: records a minted peer and registers its discard on abort.
	void record_mint(int64_t p_handle, const Cell *p_object);

	const RaisedError &error() const { return raised; }
	uint64_t park_count = 0;

	// godot-natives.md §4.2: set while a reading device constructs a class default object, during
	// which neither a peer nor a default container may be minted.
	bool mint_suppressed = false;
	// godot-natives.md §4.1's pending adoption: consumed by the one object it names.
	const Cell *adopting_object = nullptr;
	int64_t adopting_handle = 0;
	// §4.4: every peer this VM minted, and the object that minted it. Not a root: the row is what a
	// sweep of that object releases.
	std::unordered_map<int64_t, const Cell *> minted_peers;
	// The Godot name of a mirrored class, from the generated table; null answers no mint.
	const char *(*mirrored_godot_name)(std::string_view p_verse_name) = nullptr;

	// godot-natives.md §3.2, §8.29: p_object, a godot_ref, now owns the claim on p_ref, which the
	// sweep that finds it unreachable hands back through ReleaseRef, once. Ref 0 names nothing.
	void adopt_ref(const Cell *p_object, int64_t p_ref);
	std::unordered_map<const Cell *, int64_t> adopted_refs;

	// spec/natives.md §5.6: one sample, taken when the runtime starts.
	double epoch_seconds = 0.0;
	// spec/natives.md §6: seeded from the OS once; a failing transaction does not undo a draw.
	std::mt19937_64 random;

	// The Verse frames of the running call stack, innermost first, at most p_limit of them: the
	// current task's frames, then those of the task it yields to, as spec/failure.md §9.2 walks them.
	// Past the last is native code -- the embedder's entry, or a native that entered the VM.
	void stack_frames(std::vector<StackFrame> &r_frames, size_t p_limit) const;

	void visit_roots(CellVisitor &r_visitor) const override;
	void sweep_weak() override;
	void after_collect() override;

private:
	// Releases the sweep found due, made once the heap is consistent: the embedder may call back in.
	std::vector<int64_t> released_peers;
	std::vector<int64_t> released_refs;

	// Idle: control passed to an empty yield-to point, so the drive that was running is done.
	enum class Step : uint8_t {
		Next,
		Jumped,
		Fail,
		Stop,
		Idle,
	};

	// spec/tasks.md §7.1: Done when the target is settled or was unwound now; Wait when it cannot be
	// unwound yet; Error when unwinding it raised.
	enum class Cancel : uint8_t {
		Done,
		Wait,
		Error,
	};

	// Where a transaction's share of the three flat logs begins (spec/failure.md §7). A commit into
	// the parent leaves its records where they are; an abort truncates to the marks.
	struct Marks {
		size_t undo = 0;
		size_t effects = 0;
		size_t compensations = 0;
	};

	struct FailureContext {
		FrameCell *frame = nullptr;
		uint32_t on_failure = 0;
		Marks marks;
	};

	// spec/failure.md §6: what one write replaced. The logs are empty outside an entry, which is the
	// only time the collector runs, so the cells named here need no rooting.
	struct UndoRecord {
		enum class Kind : uint8_t {
			Slot,
			ArrayElement,
			ArrayLength,
			ArrayMutable,
			MapValue,
			MapInsert,
			PlaceholderLink,
		};

		Kind kind = Kind::Slot;
		Value *slot = nullptr;
		Cell *cell = nullptr;
		Value key;
		Value old;
		size_t length = 0;
	};

	FrameCell *frame = nullptr;
	uint32_t pc = 0;
	TaskCell *task = nullptr;
	size_t run_base = 0;
	Value native_result;
	Outcome stop_outcome = Outcome::Ok;
	// Error or Unsupported once anything in the outermost entry has stopped that way: a native a
	// nested entry returned through stops too, because the whole entry rolls back
	// (spec/failure.md §9.3).
	Outcome unwinding = Outcome::Ok;
	// The scope active at the first raise of the outermost entry, which its end terminates.
	ContentScopeCell *raise_scope = nullptr;
	// The bCalleeYields of the Call op now calling a native (spec/calls.md §4.4).
	bool callee_may_yield = false;
	std::vector<FailureContext> contexts;
	std::vector<UndoRecord> undo_log;
	std::vector<std::function<void()>> effects;
	std::vector<std::function<void()>> compensations;
	std::vector<Marks> entry_marks;
	RaisedError raised;
	uint32_t module_top_level = 0;
	// spec/tasks.md §5.7: open batch levels, and the variables written under them whose awaiting
	// tasks the outermost EndBatch resumes.
	uint32_t batch_depth = 0;
	std::vector<RefCell *> batched;

	// What a nested drive saves of the run it interrupts and puts back.
	struct Registers {
		FrameCell *frame;
		uint32_t pc;
		TaskCell *task;
		size_t run_base;
	};
	Registers save_registers();
	void restore_registers(const Registers &p_saved);

	void set_frame(FrameCell *p_frame) { frame = p_frame; }

	Marks marks() const;
	void record_slot(Value &r_slot);
	void record(const UndoRecord &p_record);
	void record_link(PlaceholderCell *p_placeholder);
	void abort_to(const Marks &p_marks);
	void abort_run(size_t p_base, const Marks &p_marks);

	Outcome run(FrameCell *p_entry, Value &r_result);
	// Executes the current task from frame/pc until control reaches an empty yield-to point (Ok),
	// or a failure no context catches, or a raise.
	Outcome drive();
	Outcome past_last_op();
	Step execute(const DecodedOp &p_op, const uint32_t *p_words);
	bool unwind_failure();
	Step stop(Outcome p_outcome);
	void append_frames(const NativeProcedureCell *p_native);

	// spec/tasks.md, in vm_tasks.cpp.
	TaskCell *new_task(TaskCell *p_parent);
	void join_group(TaskCell *p_task);
	static void leave_group(TaskCell *p_task);
	Step task_operand(uint32_t p_word, TaskCell *&r_task);
	Step begin_task(const uint32_t *p_words);
	Step call_task(const uint32_t *p_words);
	Step end_task(const uint32_t *p_words);
	Step wait_semaphore(const uint32_t *p_words);
	Step finish_entry_task(Value p_result);
	Step finish(TaskCell *p_task, std::vector<TaskCell *> &r_resume, Value p_resume_value, TaskCell *p_signaled);
	Step suspend(uint32_t p_resume_pc, uint32_t p_slot, ContentScopeCell *p_captured = nullptr);
	Step transfer(TaskCell *p_from);
	Step continue_task();
	Step begin_unwind(TaskCell *p_task);
	Step land(FrameCell *p_frame, uint32_t p_position);
	Cancel cancel_children(TaskCell *p_task);
	Cancel request_cancel(TaskCell *p_task);
	Outcome unwind_nested(TaskCell *p_task);
	Outcome resume_nested(TaskCell *p_task);
	void run_hooks(std::vector<TaskHook> &r_hooks, TaskCell *p_task, bool p_newest_first);
	void terminate(TaskCell *p_task);

	// spec/tasks.md §5.7 and spec/ops.md §3, in vm_tasks.cpp. A slot read while the current task has
	// an await point is put in a hidden variable the task is registered with; a write to a variable
	// ends the live binding it does not belong to and resumes its registered tasks, or, inside a
	// batch, defers them to the outermost EndBatch.
	Step begin_await();
	Step await_success();
	Step end_await();
	Step end_batch();
	bool awaiting() const;
	void register_await(RefCell *r_variable);
	void register_slot(Value &r_slot);
	Step write_variable(RefCell *r_variable, Value p_value, TaskCell *p_live);
	Step resume_awaiters(std::vector<AwaitRegistration> &r_registrations);
	// CallSet and CallSetLive on a mutable array or map; p_live is null for CallSet.
	Step element_write(Value p_container, Value p_index, Value p_value, TaskCell *p_live);
	Step live_task_operand(uint32_t p_word, TaskCell *&r_task);

	static Outcome event_await(NativeCall &r_call);
	static Outcome event_signal(NativeCall &r_call);
	static Outcome event_subscribe(NativeCall &r_call);
	static Outcome subscription_cancel(NativeCall &r_call);
	static Outcome sleep(NativeCall &r_call);

	static Outcome task_query(NativeCall &r_call, TaskCell *&r_task);
	static Outcome task_active(NativeCall &r_call);
	static Outcome task_completed(NativeCall &r_call);
	static Outcome task_canceling(NativeCall &r_call);
	static Outcome task_canceled(NativeCall &r_call);
	static Outcome task_unsettled(NativeCall &r_call);
	static Outcome task_settled(NativeCall &r_call);
	static Outcome task_uninterrupted(NativeCall &r_call);
	static Outcome task_interrupted(NativeCall &r_call);
	static Outcome task_await(NativeCall &r_call);
	static Outcome task_cancel(NativeCall &r_call);

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
	Step make_frame(const FunctionCell *p_function, Value p_self, std::vector<Value> &r_arguments,
			const std::vector<NamedArgument> &p_named, FrameCell *&r_frame);
	Step enter(const FunctionCell *p_function, Value p_self, std::vector<Value> &r_arguments,
			const std::vector<NamedArgument> &p_named, FrameCell *p_caller, uint32_t p_return_pc, uint32_t p_return_register);
	Step adapt(std::vector<Value> &r_arguments, uint32_t p_count);

	Step type_test(Value p_type, Value p_value, bool &r_admits);
	Step load_field(Value p_object, const NameCell *p_name, Value &r_result);
	Value accessor_reference(Value p_object, const AccessorCell *p_accessor, const AccessorRefCell *p_extended, Value p_step);
	Step accessor_callee(Value p_reference, bool p_setter, Value p_value, Value &r_function, std::vector<Value> &r_arguments);
	Step accessor_call(Value p_reference, bool p_setter, Value p_value, uint32_t p_dest);
	Step native_store(Value p_value);
	Step unify_native_object(Value p_token, Value p_object);
	ObjectCell *object_operand(Value p_value);
	Value bind(Value p_function, Value p_receiver);
};

} // namespace vm
