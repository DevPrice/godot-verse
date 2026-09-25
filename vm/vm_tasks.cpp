#include "vm_interpreter.h"

#include <algorithm>
#include <chrono>
#include <cstddef>

#include "vm_natives.h"
#include "vm_values.h"

// spec/tasks.md: tasks, the task ops, cancellation, termination and the natives of task(t). There is
// no scheduler queue (design §7.5): starting, suspending and finishing move control inside the
// running drive, and whatever makes a suspended task runnable runs it at once in a nested drive,
// so the native stack deepens with resumption nesting and never with Verse call depth.
namespace vm {

namespace {

void remove_task(std::vector<TaskCell *> &r_list, const TaskCell *p_task) {
	r_list.erase(std::remove(r_list.begin(), r_list.end(), p_task), r_list.end());
}

void leave_awaiters(TaskCell *p_task, Cell *p_target) {
	remove_task(static_cast<TaskCell *>(p_target)->awaiters, p_task);
}

void leave_cancelers(TaskCell *p_task, Cell *p_target) {
	remove_task(static_cast<TaskCell *>(p_target)->cancelers, p_task);
}

bool resumable(const TaskCell *p_task) {
	return !p_task->running && !p_task->finished && (p_task->captured_scope == nullptr || !p_task->captured_scope->terminated);
}

Outcome answer(NativeCall &r_call, bool p_holds) {
	if (!p_holds) {
		return Outcome::Fail;
	}
	r_call.result = r_call.heap.false_value();
	return Outcome::Ok;
}

using Phase = TaskCell::Phase;

// spec/natives.md §7: an event's awaiters, oldest first, and its subscriptions in the order they
// were made -- one conforming order among the any that §7.3 allows.
struct EventState : NativeState {
	static constexpr uint32_t kTag = kEventStateTag;

	struct Subscription {
		Value callback;
		ContentScopeCell *scope = nullptr;
		ObjectCell *handle = nullptr;
	};

	std::vector<TaskCell *> awaiters;
	std::vector<Subscription> subscriptions;

	EventState() :
			NativeState(kTag) {}
	void visit_references(CellVisitor &r_visitor) const override {
		for (const TaskCell *awaiter : awaiters) {
			r_visitor.visit(awaiter);
		}
		for (const Subscription &subscription : subscriptions) {
			r_visitor.visit(subscription.callback);
			r_visitor.visit(subscription.scope);
			r_visitor.visit(subscription.handle);
		}
	}
};

// An event_subscription's binding: null once cancelled or cleaned up.
struct SubscriptionState : NativeState {
	static constexpr uint32_t kTag = kSubscriptionStateTag;

	ObjectCell *event = nullptr;

	SubscriptionState() :
			NativeState(kTag) {}
	void visit_references(CellVisitor &r_visitor) const override { r_visitor.visit(event); }
};

template <typename T>
T *state_of(Value p_self) {
	const Value self = follow(p_self);
	return is_cell_kind(self, CellKind::Object) ? cell_as<ObjectCell>(self)->state<T>() : nullptr;
}

void leave_event(TaskCell *p_task, Cell *p_event) {
	remove_task(static_cast<ObjectCell *>(p_event)->state<EventState>()->awaiters, p_task);
}

size_t subscription_index(const EventState *p_state, const ObjectCell *p_handle) {
	for (size_t index = 0; index < p_state->subscriptions.size(); ++index) {
		if (p_state->subscriptions[index].handle == p_handle) {
			return index;
		}
	}
	return p_state->subscriptions.size();
}

} // namespace

double monotonic_seconds() {
	return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

TaskCell *Interpreter::new_task(TaskCell *p_parent) {
	TaskCell *created = heap.make<TaskCell>();
	created->parent = p_parent;
	if (p_parent != nullptr) {
		p_parent->children.push_back(created);
	}
	return created;
}

void Interpreter::join_group(TaskCell *p_task) {
	if (active_scope == nullptr) {
		return;
	}
	p_task->group = active_scope;
	active_scope->group.push_back(p_task);
}

void Interpreter::leave_group(TaskCell *p_task) {
	if (p_task->group == nullptr) {
		return;
	}
	remove_task(p_task->group->group, p_task);
	p_task->group = nullptr;
}

ContentScopeCell *Interpreter::make_scope() {
	return heap.make<ContentScopeCell>();
}

// §5.1: an absent Parent reads as uninitialized, which is no parent.
Interpreter::Step Interpreter::task_operand(uint32_t p_word, TaskCell *&r_task) {
	r_task = nullptr;
	const Value value = read(p_word);
	if (value.is_uninitialized()) {
		return Step::Next;
	}
	if (!is_cell_kind(value, CellKind::Task)) {
		return invariant("a Parent that is not a task");
	}
	r_task = cell_as<TaskCell>(value);
	return Step::Next;
}

// §5.2: Dest, Parent, bAddToTaskGroup, OnYield.
Interpreter::Step Interpreter::begin_task(const uint32_t *p_words) {
	TaskCell *parent = nullptr;
	const Step read_parent = task_operand(p_words[1], parent);
	if (read_parent != Step::Next) {
		return read_parent;
	}
	TaskCell *created = new_task(parent);
	if (p_words[2] != 0 && parent == nullptr) {
		join_group(created);
	}
	const Step unified = unify_register(p_words[0], Value::from_cell(created));
	if (unified != Step::Next) {
		return unified;
	}
	created->yield_task = task;
	created->yield_frame = frame;
	created->yield_pc = p_words[3];
	task = created;
	++pc;
	return Step::Jumped;
}

// §5.3: Dest, Parent, Callee, Arguments.
Interpreter::Step Interpreter::call_task(const uint32_t *p_words) {
	TaskCell *parent = nullptr;
	const Step read_parent = task_operand(p_words[1], parent);
	if (read_parent != Step::Next) {
		return read_parent;
	}
	const Value callee = read(p_words[2]);
	if (!is_cell_kind(callee, CellKind::Function) || cell_as<FunctionCell>(callee)->callee == nullptr ||
			cell_as<FunctionCell>(callee)->callee->kind != CellKind::Procedure) {
		return invariant("CallTask of something that is not a bytecode function");
	}
	const FunctionCell *function = cell_as<FunctionCell>(callee);
	std::vector<Value> arguments;
	const uint32_t count = variadic_count(p_words[3]);
	arguments.reserve(count);
	for (uint32_t index = 0; index < count; ++index) {
		arguments.push_back(read(variadic_items(p_words[3])[index]));
	}
	FrameCell *body = nullptr;
	const Step made = make_frame(function, function->self, arguments, {}, body);
	if (made != Step::Next) {
		return made;
	}
	TaskCell *created = new_task(parent);
	if (parent == nullptr) {
		join_group(created);
	}
	const Step unified = unify_register(p_words[0], Value::from_cell(created));
	if (unified != Step::Next) {
		return unified;
	}
	created->yield_task = task;
	created->yield_frame = frame;
	created->yield_pc = pc + 1;
	task = created;
	set_frame(body);
	pc = 0;
	return Step::Jumped;
}

// §5.6: Source, Count.
Interpreter::Step Interpreter::wait_semaphore(const uint32_t *p_words) {
	const Value source = read(p_words[0]);
	if (!is_cell_kind(source, CellKind::Semaphore)) {
		return invariant("WaitSemaphore on something that is not a semaphore");
	}
	SemaphoreCell *semaphore = cell_as<SemaphoreCell>(source);
	semaphore->count -= int32_t(p_words[1]);
	if (semaphore->count >= 0) {
		return Step::Next;
	}
	// A cancelled waiter stays in the slot (§5.6); it is not a second waiter.
	if (semaphore->waiter != nullptr && !semaphore->waiter->finished) {
		return invariant("WaitSemaphore on a semaphore another task is waiting on");
	}
	semaphore->waiter = task;
	return suspend(pc + 1, kNoRegister);
}

// §5.4: Write?, Switch?, Value, Which, Signal?.
Interpreter::Step Interpreter::end_task(const uint32_t *p_words) {
	TaskCell *const current = task;
	if (current == nullptr || !current->running) {
		return invariant("EndTask executed by a task that is not running");
	}
	if (current->phase == Phase::CancelRequested) {
		current->phase = Phase::CancelStarted;
	}
	std::vector<TaskCell *> resume;
	Value resume_value;
	TaskCell *signaled = nullptr;
	if (current->phase == Phase::Active) {
		switch (cancel_children(current)) {
			case Cancel::Done:
				break;
			case Cancel::Error:
				return Step::Stop;
			case Cancel::Wait: {
				TaskCell *blocking = current->children.back();
				blocking->cancelers.push_back(current);
				current->defer_hooks.push_back(TaskHook{ &leave_cancelers, blocking });
				return suspend(pc, kNoRegister);
			}
		}
		current->result = read(p_words[2]);
		current->has_result = true;
		if (p_words[0] != kAbsentOperand) {
			Value &write = frame->registers[p_words[0]];
			if (!write.is_empty() && follow(write).is_uninitialized()) {
				if (p_words[1] != kAbsentOperand) {
					const Value &which = frame->registers[p_words[1]];
					if (which.is_empty() || !follow(which).is_uninitialized()) {
						return invariant("an EndTask Switch register that does not hold uninitialized");
					}
				}
				record_slot(write);
				write = current->result;
				if (p_words[1] != kAbsentOperand) {
					Value &which = frame->registers[p_words[1]];
					record_slot(which);
					which = read(p_words[3]);
				}
			}
		}
		if (p_words[4] != kAbsentOperand) {
			const Value signal = read(p_words[4]);
			if (!is_cell_kind(signal, CellKind::Semaphore)) {
				return invariant("an EndTask Signal that is not a semaphore");
			}
			SemaphoreCell *semaphore = cell_as<SemaphoreCell>(signal);
			if (++semaphore->count == 0) {
				signaled = semaphore->waiter;
				semaphore->waiter = nullptr;
			}
		}
		resume.swap(current->awaiters);
		resume_value = current->result;
	} else {
		switch (cancel_children(current)) {
			case Cancel::Done:
				break;
			case Cancel::Error:
				return Step::Stop;
			case Cancel::Wait:
				return suspend(pc, kNoRegister);
		}
		current->phase = Phase::Canceled;
		resume.swap(current->cancelers);
		resume_value = heap.false_value();
		// §5.4 step 3.4 names CancelStarted; a parent waiting in its own EndTask for a child it made
		// while unwinding is CancelUnwind, and nothing else would ever resume it.
		TaskCell *parent = current->parent;
		if (parent != nullptr && (parent->phase == Phase::CancelStarted || parent->phase == Phase::CancelUnwind) &&
				!parent->children.empty() && parent->children.back() == current) {
			signaled = parent;
		}
	}
	return finish(current, resume, resume_value, signaled);
}

// A Return from an entry task's root frame: the entry completes (§4.4). It has no EndTask to wait
// in, so its unfinished children are asked to cancel and not waited for.
Interpreter::Step Interpreter::finish_entry_task(Value p_result) {
	TaskCell *const current = task;
	if (current == nullptr || !current->entry) {
		return invariant("Return from the root frame of a task body");
	}
	current->result = p_result;
	current->has_result = true;
	std::vector<TaskCell *> children = current->children;
	for (auto child = children.rbegin(); child != children.rend(); ++child) {
		if (request_cancel(*child) == Cancel::Error) {
			return Step::Stop;
		}
	}
	std::vector<TaskCell *> resume;
	resume.swap(current->awaiters);
	return finish(current, resume, p_result, nullptr);
}

// §5.4 steps 4 to 9. Each resumed task runs to its next stop in a nested drive, in §4.3's order,
// which is what arranging their yield-to points one after another would produce; then control
// goes to the finished task's own yield-to point.
Interpreter::Step Interpreter::finish(TaskCell *p_task, std::vector<TaskCell *> &r_resume, Value p_resume_value, TaskCell *p_signaled) {
	run_hooks(p_task->finish_hooks, p_task, false);
	p_task->running = false;
	leave_group(p_task);
	if (p_task->parent != nullptr) {
		remove_task(p_task->parent->children, p_task);
	}
	p_task->finished = true;
	p_task->resume_frame = nullptr;
	TaskCell *const yield_task = p_task->yield_task;
	FrameCell *const yield_frame = p_task->yield_frame;
	const uint32_t yield_pc = p_task->yield_pc;
	p_task->yield_task = nullptr;
	p_task->yield_frame = nullptr;

	std::vector<TaskCell *> arranged;
	for (TaskCell *resumed : r_resume) {
		if (resumed->phase == Phase::Active && resumable(resumed)) {
			arranged.push_back(resumed);
		}
	}
	for (size_t index = arranged.size(); index-- > 0;) {
		TaskCell *resumed = arranged[index];
		run_hooks(resumed->defer_hooks, resumed, true);
		if (resumed->resume_slot == kNoRegister) {
			continue;
		}
		const Step unified = unify_slot(resumed->resume_frame->registers[resumed->resume_slot], p_resume_value);
		if (unified == Step::Fail) {
			return invariant("a resume value that does not unify with its resume slot");
		}
		if (unified != Step::Next) {
			return unified;
		}
	}
	for (TaskCell *resumed : arranged) {
		if (resumed->phase == Phase::Active && resumable(resumed) && resume_nested(resumed) != Outcome::Ok) {
			return Step::Stop;
		}
	}
	if (p_signaled != nullptr && resumable(p_signaled) && resume_nested(p_signaled) != Outcome::Ok) {
		return Step::Stop;
	}

	if (yield_task == nullptr) {
		return Step::Idle;
	}
	task = yield_task;
	set_frame(yield_frame);
	pc = yield_pc;
	return continue_task();
}

// §4.2.
Interpreter::Step Interpreter::suspend(uint32_t p_resume_pc, uint32_t p_slot, ContentScopeCell *p_captured) {
	TaskCell *const current = task;
	if (current == nullptr) {
		return invariant("a suspension outside any task");
	}
	current->running = false;
	current->resume_frame = frame;
	current->resume_pc = p_resume_pc;
	current->resume_slot = p_slot;
	current->captured_scope = p_captured;
	if (current->phase == Phase::CancelRequested) {
		current->phase = Phase::CancelStarted;
		switch (cancel_children(current)) {
			case Cancel::Done:
				return begin_unwind(current);
			case Cancel::Error:
				return Step::Stop;
			case Cancel::Wait:
				break;
		}
	}
	return transfer(current);
}

Interpreter::Step Interpreter::transfer(TaskCell *p_from) {
	TaskCell *const target = p_from->yield_task;
	FrameCell *const target_frame = p_from->yield_frame;
	const uint32_t target_pc = p_from->yield_pc;
	p_from->yield_task = nullptr;
	p_from->yield_frame = nullptr;
	if (target == nullptr) {
		return Step::Idle;
	}
	task = target;
	set_frame(target_frame);
	pc = target_pc;
	return continue_task();
}

// §4.2 step 5: the task now given control, if it is cancelling its children, tries again.
Interpreter::Step Interpreter::continue_task() {
	TaskCell *const current = task;
	if (current->phase != Phase::CancelStarted) {
		return Step::Jumped;
	}
	current->resume_frame = frame;
	current->resume_pc = pc;
	current->resume_slot = kNoRegister;
	switch (cancel_children(current)) {
		case Cancel::Done:
			return begin_unwind(current);
		case Cancel::Error:
			return Step::Stop;
		case Cancel::Wait:
			break;
	}
	current->running = false;
	return transfer(current);
}

// §7.3, with p_task the current task and its resume point the position it unwinds from.
Interpreter::Step Interpreter::begin_unwind(TaskCell *p_task) {
	p_task->running = true;
	p_task->phase = Phase::CancelUnwind;
	run_hooks(p_task->defer_hooks, p_task, true);
	return land(p_task->resume_frame, p_task->resume_pc);
}

// §7.3 step 3, with format.md §5's coverage: an edge covers position p when it covers op p - 1.
// A frame the walk passes is abandoned, and so is any deferred-setter chain it was running.
Interpreter::Step Interpreter::land(FrameCell *p_frame, uint32_t p_position) {
	for (FrameCell *current = p_frame; current != nullptr; current = current->caller) {
		current->setters_pc = kNoRegister;
		current->setters_token = Value();
		if (p_position > 0) {
			for (const UnwindEdge &edge : current->procedure->unwind_edges) {
				if (edge.first_op <= p_position - 1 && p_position - 1 <= edge.last_op) {
					set_frame(current);
					pc = edge.landing_op;
					return Step::Jumped;
				}
			}
		}
		p_position = current->return_pc;
	}
	return invariant("unwinding that found no unwind edge covering the task's position");
}

// §7.2: newest first, each child completely before the next; the parent counts as running while it
// asks, so nothing a child does can resume it.
Interpreter::Cancel Interpreter::cancel_children(TaskCell *p_task) {
	const bool was_running = p_task->running;
	p_task->running = true;
	Cancel result = Cancel::Done;
	while (!p_task->children.empty()) {
		TaskCell *child = p_task->children.back();
		result = request_cancel(child);
		if (result != Cancel::Done) {
			break;
		}
		if (!p_task->children.empty() && p_task->children.back() == child) {
			p_task->children.pop_back();
		}
	}
	p_task->running = was_running;
	return result;
}

// §7.1 steps 1 to 4, without joining a cancelers list: the caller decides whether it waits.
Interpreter::Cancel Interpreter::request_cancel(TaskCell *p_task) {
	if (p_task->finished || p_task->phase == Phase::Canceled || p_task->has_result) {
		return Cancel::Done;
	}
	if (p_task->phase == Phase::Active) {
		p_task->phase = Phase::CancelRequested;
	}
	if (p_task->running || p_task->phase == Phase::CancelStarted || p_task->phase == Phase::CancelUnwind) {
		return Cancel::Wait;
	}
	p_task->phase = Phase::CancelStarted;
	const Cancel children = cancel_children(p_task);
	if (children != Cancel::Done) {
		return children;
	}
	return unwind_nested(p_task) == Outcome::Ok ? Cancel::Done : Cancel::Error;
}

Outcome Interpreter::unwind_nested(TaskCell *p_task) {
	const Registers saved = save_registers();
	run_base = contexts.size();
	task = p_task;
	p_task->yield_task = nullptr;
	p_task->yield_frame = nullptr;
	Outcome outcome = Outcome::Ok;
	switch (begin_unwind(p_task)) {
		case Step::Jumped:
			outcome = drive();
			break;
		case Step::Idle:
			break;
		default:
			outcome = stop_outcome;
			break;
	}
	if (outcome == Outcome::Fail) {
		invariant("a failure escaping a task's unwinding");
		outcome = stop_outcome;
	}
	restore_registers(saved);
	return outcome;
}

// Runs p_task, already given its resume value, from its resume point to its next stop, under the
// scope its suspension captured (§8.1).
Outcome Interpreter::resume_nested(TaskCell *p_task) {
	const Registers saved = save_registers();
	ContentScopeCell *const saved_scope = active_scope;
	if (p_task->captured_scope != nullptr) {
		active_scope = p_task->captured_scope;
	}
	p_task->captured_scope = nullptr;
	run_base = contexts.size();
	p_task->running = true;
	p_task->yield_task = nullptr;
	p_task->yield_frame = nullptr;
	task = p_task;
	set_frame(p_task->resume_frame);
	pc = p_task->resume_pc;
	Outcome outcome = Outcome::Ok;
	switch (continue_task()) {
		case Step::Jumped:
			outcome = drive();
			break;
		case Step::Idle:
			break;
		default:
			outcome = stop_outcome;
			break;
	}
	if (outcome == Outcome::Fail) {
		invariant("a failure escaping a resumed task");
		outcome = stop_outcome;
	}
	restore_registers(saved);
	active_scope = saved_scope;
	return outcome;
}

Outcome Interpreter::complete(TaskCell *p_task, Value p_value) {
	if (p_task == nullptr || !resumable(p_task) || p_task->phase != Phase::Active) {
		return Outcome::Ok;
	}
	run_hooks(p_task->defer_hooks, p_task, true);
	if (p_task->resume_slot != kNoRegister) {
		const Step unified = unify_slot(p_task->resume_frame->registers[p_task->resume_slot], p_value);
		if (unified == Step::Fail) {
			invariant("a completion value that does not unify with the call's destination");
			return stop_outcome;
		}
		if (unified != Step::Next) {
			return stop_outcome;
		}
	}
	return resume_nested(p_task);
}

void Interpreter::run_hooks(std::vector<TaskHook> &r_hooks, TaskCell *p_task, bool p_newest_first) {
	std::vector<TaskHook> hooks;
	hooks.swap(r_hooks);
	if (p_newest_first) {
		std::reverse(hooks.begin(), hooks.end());
	}
	for (const TaskHook &hook : hooks) {
		hook.run(p_task, hook.target);
	}
}

void Interpreter::terminate_scope(ContentScopeCell *p_scope) {
	if (p_scope == nullptr) {
		return;
	}
	p_scope->terminated = true;
	std::vector<TaskCell *> roots;
	roots.swap(p_scope->group);
	for (TaskCell *root : roots) {
		root->group = nullptr;
		terminate(root);
	}
}

// §8.2: no defer body runs and nothing is resumed; children go the same way, newest first, each
// after its parent's hooks.
void Interpreter::terminate(TaskCell *p_task) {
	if (p_task->finished) {
		return;
	}
	leave_group(p_task);
	p_task->phase = Phase::Canceled;
	run_hooks(p_task->defer_hooks, p_task, true);
	run_hooks(p_task->finish_hooks, p_task, false);
	p_task->finished = true;
	p_task->running = false;
	p_task->resume_frame = nullptr;
	p_task->yield_task = nullptr;
	p_task->yield_frame = nullptr;
	std::vector<TaskCell *> children;
	children.swap(p_task->children);
	for (auto child = children.rbegin(); child != children.rend(); ++child) {
		terminate(*child);
	}
}

Outcome Interpreter::task_query(NativeCall &r_call, TaskCell *&r_task) {
	if (!is_cell_kind(r_call.self, CellKind::Task)) {
		return Outcome::Invalid;
	}
	r_task = cell_as<TaskCell>(r_call.self);
	return Outcome::Ok;
}

// spec/tasks.md §3's table.
Outcome Interpreter::task_active(NativeCall &r_call) {
	TaskCell *target = nullptr;
	return task_query(r_call, target) != Outcome::Ok ? Outcome::Invalid : answer(r_call, target->phase < Phase::CancelStarted && !target->has_result);
}

Outcome Interpreter::task_completed(NativeCall &r_call) {
	TaskCell *target = nullptr;
	return task_query(r_call, target) != Outcome::Ok ? Outcome::Invalid : answer(r_call, target->has_result);
}

Outcome Interpreter::task_canceling(NativeCall &r_call) {
	TaskCell *target = nullptr;
	return task_query(r_call, target) != Outcome::Ok
			? Outcome::Invalid
			: answer(r_call, target->phase == Phase::CancelStarted || target->phase == Phase::CancelUnwind);
}

Outcome Interpreter::task_canceled(NativeCall &r_call) {
	TaskCell *target = nullptr;
	return task_query(r_call, target) != Outcome::Ok ? Outcome::Invalid : answer(r_call, target->phase == Phase::Canceled);
}

Outcome Interpreter::task_unsettled(NativeCall &r_call) {
	TaskCell *target = nullptr;
	return task_query(r_call, target) != Outcome::Ok ? Outcome::Invalid : answer(r_call, target->phase < Phase::Canceled && !target->has_result);
}

Outcome Interpreter::task_settled(NativeCall &r_call) {
	TaskCell *target = nullptr;
	return task_query(r_call, target) != Outcome::Ok ? Outcome::Invalid : answer(r_call, target->phase == Phase::Canceled || target->has_result);
}

Outcome Interpreter::task_uninterrupted(NativeCall &r_call) {
	TaskCell *target = nullptr;
	return task_query(r_call, target) != Outcome::Ok ? Outcome::Invalid : answer(r_call, target->phase == Phase::Active);
}

Outcome Interpreter::task_interrupted(NativeCall &r_call) {
	TaskCell *target = nullptr;
	return task_query(r_call, target) != Outcome::Ok ? Outcome::Invalid : answer(r_call, target->phase != Phase::Active);
}

// spec/tasks.md §9.
Outcome Interpreter::task_await(NativeCall &r_call) {
	TaskCell *target = nullptr;
	if (task_query(r_call, target) != Outcome::Ok) {
		return Outcome::Invalid;
	}
	if (target->has_result) {
		r_call.result = target->result;
		return Outcome::Ok;
	}
	TaskCell *waiter = r_call.interpreter != nullptr ? r_call.interpreter->task : nullptr;
	if (waiter == nullptr) {
		return Outcome::Invalid;
	}
	target->awaiters.push_back(waiter);
	waiter->defer_hooks.push_back(TaskHook{ &leave_awaiters, target });
	return Outcome::Yield;
}

// spec/tasks.md §7.1. A raise while unwinding the target is already the interpreter's; the native
// returns and the call op stops on it.
Outcome Interpreter::task_cancel(NativeCall &r_call) {
	TaskCell *target = nullptr;
	if (task_query(r_call, target) != Outcome::Ok || r_call.interpreter == nullptr) {
		return Outcome::Invalid;
	}
	Interpreter &interpreter = *r_call.interpreter;
	r_call.result = r_call.heap.false_value();
	if (interpreter.request_cancel(target) != Cancel::Wait || interpreter.task == nullptr) {
		return Outcome::Ok;
	}
	TaskCell *canceler = interpreter.task;
	target->cancelers.push_back(canceler);
	canceler->defer_hooks.push_back(TaskHook{ &leave_cancelers, target });
	return Outcome::Yield;
}

// §5.7: the op after BeginAwait is the await point, where the failed condition's Yield returns.
Interpreter::Step Interpreter::begin_await() {
	if (task == nullptr || task->await_initializing || task->await_pc != kNoRegister) {
		return invariant("BeginAwait with an await already open");
	}
	task->await_initializing = true;
	task->await_pc = pc + 1;
	task->await_frame = frame;
	return Step::Next;
}

Interpreter::Step Interpreter::await_success() {
	if (task == nullptr) {
		return invariant("AwaitSuccess outside any task");
	}
	if (task->await_initializing) {
		return Step::Fail;
	}
	task->await_pc = kNoRegister;
	task->await_frame = nullptr;
	return Step::Next;
}

Interpreter::Step Interpreter::end_await() {
	if (task == nullptr || task->await_pc == kNoRegister) {
		return invariant("EndAwait with no await open");
	}
	task->await_initializing = false;
	task->await_pc = kNoRegister;
	task->await_frame = nullptr;
	return Step::Next;
}

Interpreter::Step Interpreter::end_batch() {
	if (batch_depth == 0) {
		return invariant("EndBatch with no batch open");
	}
	if (--batch_depth > 0) {
		return Step::Next;
	}
	std::vector<RefCell *> written;
	written.swap(batched);
	std::vector<AwaitRegistration> registrations;
	for (RefCell *variable : written) {
		registrations.insert(registrations.end(), variable->awaiting.begin(), variable->awaiting.end());
		variable->awaiting.clear();
	}
	return resume_awaiters(registrations);
}

bool Interpreter::awaiting() const {
	return task != nullptr && task->await_pc != kNoRegister;
}

void Interpreter::register_await(RefCell *r_variable) {
	if (!awaiting()) {
		return;
	}
	for (const AwaitRegistration &registration : r_variable->awaiting) {
		if (registration.task == task && registration.frame == task->await_frame && registration.pc == task->await_pc) {
			return;
		}
	}
	r_variable->awaiting.push_back(AwaitRegistration{ task, task->await_frame, task->await_pc });
}

// spec/ops.md §3.1: the replacement is not a transaction write, so it is never undone.
void Interpreter::register_slot(Value &r_slot) {
	RefCell *hidden = nullptr;
	if (is_cell_kind(r_slot, CellKind::Ref) && cell_as<RefCell>(r_slot)->hidden) {
		hidden = cell_as<RefCell>(r_slot);
	} else {
		hidden = heap.make<RefCell>(r_slot);
		hidden->hidden = true;
		r_slot = Value::from_cell(hidden);
	}
	register_await(hidden);
}

// spec/ops.md §3.2 and RefSet: the live task goes first, as a cancel nothing waits for; then the
// content is replaced; then the registered tasks run, each to its next stop, before the write
// completes.
Interpreter::Step Interpreter::write_variable(RefCell *r_variable, Value p_value, TaskCell *p_live) {
	TaskCell *const bound = is_cell_kind(r_variable->live_task, CellKind::Task) ? cell_as<TaskCell>(r_variable->live_task) : nullptr;
	if (bound != p_live) {
		record_slot(r_variable->live_task);
		r_variable->live_task = p_live != nullptr ? Value::from_cell(p_live) : Value::uninitialized();
		if (bound != nullptr && request_cancel(bound) == Cancel::Error) {
			return Step::Stop;
		}
	}
	record_slot(r_variable->content);
	r_variable->content = p_value;
	if (r_variable->awaiting.empty()) {
		return Step::Next;
	}
	if (batch_depth > 0) {
		if (std::find(batched.begin(), batched.end(), r_variable) == batched.end()) {
			batched.push_back(r_variable);
		}
		return Step::Next;
	}
	std::vector<AwaitRegistration> registrations;
	registrations.swap(r_variable->awaiting);
	return resume_awaiters(registrations);
}

// A registration lapses unless its task is still suspended exactly at the await point it read
// under; one that holds resumes there with the point re-armed, so this evaluation's AwaitSuccess
// can pass. A task registered several times runs once. The order among them is the reference's
// open question (spec/tasks.md §14 Q5); this is registration order.
Interpreter::Step Interpreter::resume_awaiters(std::vector<AwaitRegistration> &r_registrations) {
	std::vector<TaskCell *> resumed;
	for (const AwaitRegistration &registration : r_registrations) {
		TaskCell *const awaiter = registration.task;
		if (awaiter->running || awaiter->finished || awaiter->phase != Phase::Active || awaiter->resume_frame != registration.frame ||
				awaiter->resume_pc != registration.pc || std::find(resumed.begin(), resumed.end(), awaiter) != resumed.end()) {
			continue;
		}
		resumed.push_back(awaiter);
		awaiter->await_initializing = false;
		awaiter->await_pc = registration.pc;
		awaiter->await_frame = registration.frame;
		if (complete(awaiter, Value()) != Outcome::Ok) {
			return Step::Stop;
		}
	}
	return Step::Next;
}

Interpreter::Step Interpreter::element_write(Value p_container, Value p_index, Value p_value, TaskCell *p_live) {
	if (Value *slot = element_slot(p_container, p_index, false)) {
		if (is_cell_kind(*slot, CellKind::Ref) && cell_as<RefCell>(*slot)->hidden) {
			return write_variable(cell_as<RefCell>(*slot), p_value, p_live);
		}
	}
	// spec/ops.md §3.2: an element in no hidden variable records no live task.
	if (is_cell_kind(p_container, CellKind::MutableArray)) {
		Value old;
		const Outcome outcome = array_set(p_container, p_index, p_value, old);
		if (outcome == Outcome::Ok) {
			UndoRecord entry;
			entry.kind = UndoRecord::Kind::ArrayElement;
			entry.cell = p_container.as_cell();
			entry.key = p_index;
			entry.old = old;
			record(entry);
		}
		return unify_outcome(outcome, kNoRegister, Value());
	}
	if (is_cell_kind(p_container, CellKind::MutableMap)) {
		bool inserted = false;
		Value old;
		const Outcome outcome = map_set(p_container, p_index, p_value, inserted, old);
		if (outcome == Outcome::Ok) {
			UndoRecord entry;
			entry.kind = inserted ? UndoRecord::Kind::MapInsert : UndoRecord::Kind::MapValue;
			entry.cell = p_container.as_cell();
			entry.key = p_index;
			entry.old = old;
			record(entry);
		}
		return unify_outcome(outcome, kNoRegister, Value());
	}
	return invariant(std::string("CallSet on a ") + (p_container.is_cell() ? cell_kind_name(p_container.as_cell()->kind) : "value that is not a cell"));
}

Interpreter::Step Interpreter::live_task_operand(uint32_t p_word, TaskCell *&r_task) {
	const Value value = read(p_word);
	if (!is_cell_kind(value, CellKind::Task)) {
		return invariant("a live write whose Task is not a task");
	}
	r_task = cell_as<TaskCell>(value);
	return Step::Next;
}

std::vector<TaskCell *> Interpreter::take_due_sleepers(double p_now) {
	std::vector<Sleeper> due;
	std::vector<Sleeper> waiting;
	for (const Sleeper &sleeper : sleepers) {
		(sleeper.deadline <= p_now ? due : waiting).push_back(sleeper);
	}
	sleepers.swap(waiting);
	std::stable_sort(due.begin(), due.end(), [](const Sleeper &p_left, const Sleeper &p_right) { return p_left.deadline < p_right.deadline; });
	std::vector<TaskCell *> tasks;
	for (const Sleeper &sleeper : due) {
		tasks.push_back(sleeper.task);
	}
	return tasks;
}

// spec/natives.md §7.1.
Outcome Interpreter::event_await(NativeCall &r_call) {
	EventState *state = state_of<EventState>(r_call.self);
	TaskCell *waiter = r_call.interpreter != nullptr ? r_call.interpreter->task : nullptr;
	if (state == nullptr || waiter == nullptr) {
		return Outcome::Invalid;
	}
	state->awaiters.push_back(waiter);
	waiter->defer_hooks.push_back(TaskHook{ &leave_event, follow(r_call.self).as_cell() });
	return Outcome::Yield;
}

// spec/natives.md §7.2: the awaiters as one batch taken up front, each run to its next stop; a
// member cancelled before its turn is no longer Active, so completing it does nothing. Then the
// subscriptions as they stand now, each under its own scope and in its own transaction. A raise
// in either stops the Signal, which the call op then reports.
Outcome Interpreter::event_signal(NativeCall &r_call) {
	if (unbound_argument(r_call)) {
		return Outcome::Park;
	}
	EventState *state = state_of<EventState>(r_call.self);
	if (state == nullptr || r_call.interpreter == nullptr) {
		return Outcome::Invalid;
	}
	Interpreter &interpreter = *r_call.interpreter;
	const Value payload = argument(r_call, 0);
	r_call.result = r_call.heap.false_value();
	std::vector<TaskCell *> batch;
	batch.swap(state->awaiters);
	for (TaskCell *awaiter : batch) {
		if (interpreter.complete(awaiter, payload) != Outcome::Ok) {
			return Outcome::Ok;
		}
	}
	const std::vector<EventState::Subscription> subscriptions = state->subscriptions;
	for (const EventState::Subscription &subscription : subscriptions) {
		const size_t index = subscription_index(state, subscription.handle);
		if (index == state->subscriptions.size()) {
			continue;
		}
		if (subscription.scope != nullptr && subscription.scope->terminated) {
			state->subscriptions.erase(state->subscriptions.begin() + ptrdiff_t(index));
			subscription.handle->state<SubscriptionState>()->event = nullptr;
			continue;
		}
		ContentScopeCell *const saved_scope = interpreter.active_scope;
		interpreter.active_scope = subscription.scope;
		interpreter.begin_entry();
		Value ignored;
		const Outcome outcome = interpreter.invoke(subscription.callback, Value(), { payload }, {}, ignored);
		interpreter.end_entry(outcome == Outcome::Ok);
		interpreter.active_scope = saved_scope;
		if (interpreter.unwinding != Outcome::Ok) {
			return Outcome::Ok;
		}
	}
	return Outcome::Ok;
}

// spec/natives.md §7.3: owned by the script instance's scope, and undone with a failing transaction.
Outcome Interpreter::event_subscribe(NativeCall &r_call) {
	if (unbound_argument(r_call)) {
		return Outcome::Park;
	}
	EventState *state = state_of<EventState>(r_call.self);
	if (state == nullptr || r_call.interpreter == nullptr) {
		return Outcome::Invalid;
	}
	Interpreter &interpreter = *r_call.interpreter;
	const ClassCell *subscription_class = find_library_class(interpreter.program, "(/Verse.org/Verse:)event_subscription");
	if (subscription_class == nullptr) {
		return Outcome::Invalid;
	}
	ObjectCell *handle = interpreter.layouts.new_object(r_call.heap, interpreter.layouts.get(subscription_class));
	ObjectCell *event = cell_as<ObjectCell>(follow(r_call.self));
	handle->state<SubscriptionState>()->event = event;
	state->subscriptions.push_back(EventState::Subscription{ argument(r_call, 0), interpreter.active_scope, handle });
	interpreter.compensate([state, handle] {
		const size_t index = subscription_index(state, handle);
		if (index < state->subscriptions.size()) {
			state->subscriptions.erase(state->subscriptions.begin() + ptrdiff_t(index));
		}
		handle->state<SubscriptionState>()->event = nullptr;
	});
	r_call.result = Value::from_cell(handle);
	return Outcome::Ok;
}

Outcome Interpreter::subscription_cancel(NativeCall &r_call) {
	SubscriptionState *binding = state_of<SubscriptionState>(r_call.self);
	if (binding == nullptr || r_call.interpreter == nullptr) {
		return Outcome::Invalid;
	}
	r_call.result = r_call.heap.false_value();
	ObjectCell *const event = binding->event;
	if (event == nullptr) {
		return Outcome::Ok;
	}
	EventState *state = event->state<EventState>();
	ObjectCell *handle = cell_as<ObjectCell>(follow(r_call.self));
	const size_t index = subscription_index(state, handle);
	binding->event = nullptr;
	if (index == state->subscriptions.size()) {
		return Outcome::Ok;
	}
	const EventState::Subscription removed = state->subscriptions[index];
	state->subscriptions.erase(state->subscriptions.begin() + ptrdiff_t(index));
	r_call.interpreter->compensate([state, binding, event, removed, index] {
		state->subscriptions.insert(state->subscriptions.begin() + ptrdiff_t(std::min(index, state->subscriptions.size())), removed);
		binding->event = event;
	});
	return Outcome::Ok;
}

// godot-natives.md §10: below zero is no suspension at all; zero and above wait for the tick.
Outcome Interpreter::sleep(NativeCall &r_call) {
	if (unbound_argument(r_call)) {
		return Outcome::Park;
	}
	const Value seconds = argument(r_call, 0);
	if (!seconds.is_float() || r_call.interpreter == nullptr) {
		return Outcome::Invalid;
	}
	r_call.result = r_call.heap.false_value();
	if (seconds.as_float() < 0.0) {
		return Outcome::Ok;
	}
	Interpreter &interpreter = *r_call.interpreter;
	if (interpreter.task == nullptr) {
		return Outcome::Invalid;
	}
	interpreter.sleepers.push_back(Sleeper{ interpreter.task, interpreter.clock() + seconds.as_float() });
	return Outcome::Yield;
}

NativeFn Interpreter::task_native(std::string_view p_binding_key) {
	struct Binding {
		const char *key;
		NativeFn implementation;
	};
	static constexpr Binding kBindings[] = {
		{ "(/Verse.org/Concurrency/task/(/Verse.org/Concurrency/task:)Active:)Native", &Interpreter::task_active },
		{ "(/Verse.org/Concurrency/task/(/Verse.org/Concurrency/task:)Completed:)Native", &Interpreter::task_completed },
		{ "(/Verse.org/Concurrency/task/(/Verse.org/Concurrency/task:)Canceling:)Native", &Interpreter::task_canceling },
		{ "(/Verse.org/Concurrency/task/(/Verse.org/Concurrency/task:)Canceled:)Native", &Interpreter::task_canceled },
		{ "(/Verse.org/Concurrency/task/(/Verse.org/Concurrency/task:)Unsettled:)Native", &Interpreter::task_unsettled },
		{ "(/Verse.org/Concurrency/task/(/Verse.org/Concurrency/task:)Settled:)Native", &Interpreter::task_settled },
		{ "(/Verse.org/Concurrency/task/(/Verse.org/Concurrency/task:)Uninterrupted:)Native", &Interpreter::task_uninterrupted },
		{ "(/Verse.org/Concurrency/task/(/Verse.org/Concurrency/task:)Interrupted:)Native", &Interpreter::task_interrupted },
		{ "(/Verse.org/Concurrency/task/Await:)Native", &Interpreter::task_await },
		{ "(/Verse.org/Concurrency/task/Cancel:)Native", &Interpreter::task_cancel },
		{ "(/Verse.org/Verse/event/Await:)Native", &Interpreter::event_await },
		{ "(/Verse.org/Verse/event/(/Verse.org/Verse/signalable:)Signal(:payload):)Native", &Interpreter::event_signal },
		{ "(/Verse.org/Verse/subscribable_event_intrnl/(/Verse.org/Verse/signalable:)Signal(:payload):)Native", &Interpreter::event_signal },
		{ "(/Verse.org/Verse/subscribable_event_intrnl/(/Verse.org/Verse/subscribable:)Subscribe(:t->void):)Native", &Interpreter::event_subscribe },
		{ "(/Verse.org/Verse/event_subscription/(/Verse.org/Verse/cancelable:)Cancel:)Native", &Interpreter::subscription_cancel },
		{ "(/Godot.org/Godot/Sleep(:float):)Native", &Interpreter::sleep },
	};
	for (const Binding &binding : kBindings) {
		if (p_binding_key == binding.key) {
			return binding.implementation;
		}
	}
	return nullptr;
}

} // namespace vm
