// The twelve compiler entry points answer VH_ERR_UNSUPPORTED whatever the state, as a
// WITH_VERSE_COMPILER=0 UE host does (VH_REFUSE_WITHOUT_COMPILER): vh_set_bindings, vh_compile_project,
// the four vh_check_project*, vh_lookup_symbol, vh_complete_symbol, vh_resolve_unknown_name,
// vh_class_members, vh_class_override_candidates and vh_signature_at. vh_check_project_busy answers
// 0, having no status to refuse through.
#include "verse_host_abi.h"

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "vm_marshal.h"
#include "vm_objects.h"
#include "vm_runtime.h"
#include "vm_values.h"

namespace {

constexpr int32_t kNotBooted = VH_ERR_STATE;
constexpr int32_t kCompilerOnly = VH_ERR_UNSUPPORTED;

// The ABI is a set of free functions over one runtime, so this is the one piece of global state.
vm::Runtime *g_runtime = nullptr;

template <typename Desc>
int32_t refuse_list(const Desc **r_out, int32_t *r_count) {
	if (r_out != nullptr) {
		*r_out = nullptr;
	}
	if (r_count != nullptr) {
		*r_count = 0;
	}
	return kNotBooted;
}

// Ends the VM entry an execution entry point began, committing only a normal completion, and
// answers its status. A suspended entry task is a success with no result (spec/tasks.md §4.4). A
// raise is reported first; so is an operation this runtime cannot perform yet, which answers
// VH_ERR_UNSUPPORTED because the script did nothing wrong.
int32_t entry_status(vm::Outcome p_outcome) {
	vm::Interpreter &interpreter = g_runtime->interpreter;
	switch (p_outcome) {
		case vm::Outcome::Ok:
		case vm::Outcome::Yield:
			interpreter.end_entry(true);
			return VH_OK;
		case vm::Outcome::Fail:
			interpreter.end_entry(false);
			return VH_ERR_FAILED;
		case vm::Outcome::Unsupported:
			interpreter.end_entry(false);
			g_runtime->report_raised(interpreter.error());
			return VH_ERR_UNSUPPORTED;
		default:
			interpreter.end_entry(false);
			// A nested entry's raise also stops the outer one, whose frames include the inner
			// frames; reporting it here as well would report it twice.
			if (!interpreter.in_entry()) {
				g_runtime->report_raised(interpreter.error());
			}
			return VH_ERR_RUNTIME;
	}
}

// An entry made while another is running -- Godot calling back into a script from inside a native
// -- hands the outer entry its own scope back.
struct ScopeRestore {
	vm::Interpreter &interpreter;
	vm::ContentScopeCell *saved;
	~ScopeRestore() { interpreter.active_scope = saved; }
};

const vm::SidecarMethod *find_method(const vm::SidecarClass *p_class, const char *p_decorated) {
	if (p_class == nullptr) {
		return nullptr;
	}
	for (const vm::SidecarMethod &method : p_class->methods) {
		if (method.decorated == p_decorated) {
			return &method;
		}
	}
	return nullptr;
}

const vm::SidecarMemberType *find_member_type(const vm::SidecarClass *p_class, const char *p_name) {
	if (p_class == nullptr || p_name == nullptr) {
		return nullptr;
	}
	for (const std::pair<std::string, vm::SidecarMemberType> &member : p_class->member_types) {
		if (member.first == p_name) {
			return &member.second;
		}
	}
	return nullptr;
}

const vm::ClassCell *superclass_of(const vm::ClassCell *p_class) {
	if (p_class == nullptr || p_class->inherited.empty()) {
		return nullptr;
	}
	const vm::ClassCell *first = p_class->inherited.front();
	return first != nullptr && first->class_kind != vm::ClassKind::Interface ? first : nullptr;
}

const vm::Cell *callee_of(const vm::LayoutField *p_field) {
	if (p_field == nullptr || p_field->kind != vm::FieldKind::Constant || !vm::is_cell_kind(p_field->value, vm::CellKind::Function)) {
		return nullptr;
	}
	return vm::cell_as<vm::FunctionCell>(p_field->value)->callee;
}

// Which of the runtime's result arenas a call answers into when the consumer hands none.
struct CallDepth {
	size_t depth;
	CallDepth() :
			depth(g_runtime->call_depth++) {}
	~CallDepth() { --g_runtime->call_depth; }
};

// What vh_init reads of a descriptor, copied so every field past the consumer's StructSize reads
// as zero rather than as whatever followed the consumer's shorter struct -- the descriptor's own
// and the callback table's alike (include/verse_host_abi.h, CLAUDE.md on minors).
vh_init_desc copy_descriptor(const vh_init_desc &p_desc) {
	vh_init_desc copy = {};
	const size_t desc_size = size_t(p_desc.StructSize) < sizeof(vh_init_desc) ? size_t(p_desc.StructSize) : sizeof(vh_init_desc);
	std::memcpy(&copy, &p_desc, desc_size);
	const size_t godot_offset = offsetof(vh_init_desc, Godot);
	if (desc_size < godot_offset + sizeof(int32_t)) {
		std::memset(&copy.Godot, 0, sizeof(copy.Godot));
		return copy;
	}
	const int32_t godot_size = copy.Godot.StructSize;
	const size_t kept = godot_size <= 0 ? 0 : (size_t(godot_size) < sizeof(vh_godot_api) ? size_t(godot_size) : sizeof(vh_godot_api));
	std::memset(reinterpret_cast<unsigned char *>(&copy.Godot) + kept, 0, sizeof(vh_godot_api) - kept);
	return copy;
}

bool gc_stress_requested() {
	const char *const name = "VERSE_VM_GC_STRESS";
#ifdef _MSC_VER
	char *value = nullptr;
	size_t length = 0;
	const bool set = _dupenv_s(&value, &length, name) == 0 && value != nullptr && value[0] != '\0' && value[0] != '0';
	std::free(value);
	return set;
#else
	const char *value = std::getenv(name);
	return value != nullptr && value[0] != '\0' && value[0] != '0';
#endif
}

// The stress mode's collection after an entry the host made. A nested entry -- Godot calling back
// in from inside a native -- is still inside the outer one, which collect_garbage declines.
void collect_after_entry() {
	if (g_runtime->gc_stress) {
		g_runtime->collect_garbage();
	}
}

} // namespace

int32_t vh_abi_version(void) {
	return VH_ABI_VERSION;
}

int32_t vh_host_kind(void) {
	return VH_HOST_RUNTIME;
}

int32_t vh_init(const vh_init_desc *Desc) {
	if (g_runtime != nullptr) {
		return VH_ERR_STATE;
	}
	if (Desc == nullptr || Desc->StructSize < int32_t(offsetof(vh_init_desc, AbiVersion) + sizeof(int32_t)) ||
			Desc->AbiVersion / 1000 != VH_ABI_VERSION_MAJOR) {
		return VH_ERR_ABI;
	}
	const vh_init_desc desc = copy_descriptor(*Desc);

	vm::Runtime *runtime = new vm::Runtime();
	runtime->godot = desc.Godot;
	runtime->interpreter.godot = desc.Godot;
	runtime->on_diagnostic = desc.OnDiagnostic;
	runtime->diagnostic_ctx = desc.DiagnosticCtx;
	runtime->on_runtime_error = desc.OnRuntimeError;
	runtime->runtime_error_ctx = desc.RuntimeErrorCtx;
	runtime->gc_stress = gc_stress_requested();

	std::string error;
	int32_t status = VH_ERR_INIT;
	if (desc.CookedDirUtf8 == nullptr) {
		error = "This runtime runs cooked Verse only, and vh_init was given no cooked directory.";
	} else {
		status = runtime->boot(desc.CookedDirUtf8, error);
	}
	if (status != VH_OK) {
		runtime->report_error(error);
		delete runtime;
		return status;
	}
	g_runtime = runtime;
	return VH_OK;
}

// Frees the heap without a collection, so no ReleaseObject or ReleaseRef reaches the consumer for
// a peer or ref still live. The consumer drops its whole ref table and minted-peer table before it
// calls this (src/verse_runtime.cpp, unload_host) and answers a release naming either with nothing,
// so a release here would be a callback per live object into a consumer tearing down, for no effect.
void vh_shutdown(void) {
	delete g_runtime;
	g_runtime = nullptr;
}

// The budget governs queued work, and this runtime queues none: a due sleeper is not subject to it
// (godot-natives.md §10), so JobsPending and Overran are always zero.
void vh_tick(double BudgetSeconds, vh_tick_stats *OutStats) {
	(void)BudgetSeconds;
	vh_tick_stats stats = {};
	if (g_runtime != nullptr && g_runtime->on_init_thread()) {
		g_runtime->tick(stats);
	}
	if (OutStats == nullptr) {
		return;
	}
	// Filled up to the consumer's own StructSize; one smaller than the v6.0 struct is left alone.
	const int32_t requested = OutStats->StructSize;
	if (requested < int32_t(offsetof(vh_tick_stats, AnalysisWaits))) {
		return;
	}
	stats.StructSize = requested;
	std::memcpy(OutStats, &stats, size_t(requested) < sizeof(stats) ? size_t(requested) : sizeof(stats));
}

void vh_collect_garbage(void) {
	if (g_runtime != nullptr) {
		g_runtime->collect_garbage();
	}
}

int32_t vh_set_bindings(const char *SourceUtf8, int32_t SourceLen, const vh_binding_class *Classes, int32_t ClassCount) {
	(void)SourceUtf8;
	(void)SourceLen;
	(void)Classes;
	(void)ClassCount;
	return kCompilerOnly;
}

int32_t vh_compile_project(const vh_source_file *Files, int32_t Count, int32_t *OutGeneration) {
	(void)Files;
	(void)Count;
	(void)OutGeneration; // a failed build leaves OutGeneration alone, per the real ABI's own rule
	return kCompilerOnly;
}

int32_t vh_check_project(const char *PathUtf8, const char *SourceUtf8) {
	(void)PathUtf8;
	(void)SourceUtf8;
	return kCompilerOnly;
}

int32_t vh_check_project_begin(const char *PathUtf8, const char *SourceUtf8) {
	(void)PathUtf8;
	(void)SourceUtf8;
	return kCompilerOnly;
}

int32_t vh_check_project_poll(vh_bool *OutFinished) {
	if (OutFinished != nullptr) {
		*OutFinished = 0;
	}
	return kCompilerOnly;
}

vh_bool vh_check_project_busy(void) {
	return 0;
}

int32_t vh_run_main(const char *const *Args, int32_t ArgCount, int64_t *OutExitCode) {
	(void)Args;
	(void)ArgCount;
	(void)OutExitCode;
	return kNotBooted;
}

vh_bool vh_has_class(const char *ClassNameUtf8) {
	return g_runtime != nullptr && g_runtime->has_class(ClassNameUtf8) ? 1 : 0;
}

int32_t vh_instantiate(const char *ClassNameUtf8, vh_handle Handle, vh_instance **OutInstance) {
	if (OutInstance != nullptr) {
		*OutInstance = nullptr;
	}
	if (g_runtime == nullptr) {
		return kNotBooted;
	}
	if (ClassNameUtf8 == nullptr || OutInstance == nullptr) {
		return VH_ERR_ARGUMENT;
	}
	if (!g_runtime->on_init_thread()) {
		g_runtime->refuse_thread(std::string("Instantiating ") + ClassNameUtf8);
		return VH_ERR_THREAD;
	}
	const vm::ClassIndexEntry *entry = g_runtime->program.find_class(vm::ClassOrigin::Script, ClassNameUtf8);
	if (entry == nullptr || !g_runtime->has_class(ClassNameUtf8)) {
		return VH_ERR_NOT_FOUND;
	}
	vm::Interpreter &interpreter = g_runtime->interpreter;
	vh_instance *instance = new vh_instance();
	g_runtime->heap.add_handle_root(&instance->object);
	g_runtime->heap.add_handle_root(&instance->scope);
	const ScopeRestore restore{ interpreter, interpreter.active_scope };
	g_runtime->activate_scope(instance->scope);
	interpreter.begin_entry();
	const vm::Outcome outcome = interpreter.construct(entry->class_cell, Handle, instance->object);
	const int32_t status = entry_status(outcome);
	if (status != VH_OK) {
		g_runtime->heap.remove_handle_root(&instance->object);
		g_runtime->heap.remove_handle_root(&instance->scope);
		delete instance;
		collect_after_entry();
		return status;
	}
	instance->handle = Handle;
	instance->sidecar_class = g_runtime->sidecar.find_class(ClassNameUtf8);
	g_runtime->instance_scopes.push_back(&instance->scope);
	g_runtime->bridge.register_instance(instance);
	g_runtime->bridge.bind_instance_signals(instance);
	*OutInstance = instance;
	collect_after_entry();
	return VH_OK;
}

void vh_release_instance(vh_instance *Instance) {
	if (Instance == nullptr) {
		return;
	}
	if (g_runtime != nullptr) {
		vm::Interpreter &interpreter = g_runtime->interpreter;
		if (vm::is_cell_kind(Instance->scope, vm::CellKind::ContentScope)) {
			vm::ContentScopeCell *scope = vm::cell_as<vm::ContentScopeCell>(Instance->scope);
			// spec/tasks.md §8.2: termination inside an open transaction happens when it commits.
			if (interpreter.in_entry()) {
				interpreter.defer([&interpreter, scope] { interpreter.terminate_scope(scope); });
			} else {
				interpreter.terminate_scope(scope);
			}
		}
		g_runtime->bridge.release_instance_signals(Instance);
		g_runtime->bridge.forget_instance(Instance);
		g_runtime->heap.remove_handle_root(&Instance->object);
		g_runtime->heap.remove_handle_root(&Instance->scope);
		std::vector<const vm::Value *> &scopes = g_runtime->instance_scopes;
		scopes.erase(std::remove(scopes.begin(), scopes.end(), &Instance->scope), scopes.end());
	}
	delete Instance;
}

int32_t vh_class_method_list(const char *ClassNameUtf8, const vh_method_desc **OutMethods, int32_t *OutCount) {
	return g_runtime == nullptr ? refuse_list(OutMethods, OutCount) : g_runtime->method_list(ClassNameUtf8, OutMethods, OutCount);
}

int32_t vh_class_signal_list(const char *ClassNameUtf8, const vh_signal_desc **OutSignals, int32_t *OutCount) {
	return g_runtime == nullptr ? refuse_list(OutSignals, OutCount) : g_runtime->signal_list(ClassNameUtf8, OutSignals, OutCount);
}

int32_t vh_class_rpc_list(const char *ClassNameUtf8, const vh_rpc_desc **OutRpcs, int32_t *OutCount) {
	return g_runtime == nullptr ? refuse_list(OutRpcs, OutCount) : g_runtime->rpc_list(ClassNameUtf8, OutRpcs, OutCount);
}

int32_t vh_class_static_list(const char *ClassNameUtf8, const vh_static_desc **OutStatics, int32_t *OutCount) {
	return g_runtime == nullptr ? refuse_list(OutStatics, OutCount) : g_runtime->static_list(ClassNameUtf8, OutStatics, OutCount);
}

vh_bool vh_class_is_abstract(const char *ClassNameUtf8) {
	return g_runtime != nullptr && g_runtime->is_abstract(ClassNameUtf8) ? 1 : 0;
}

int32_t vh_class_base_type(const char *ClassNameUtf8, const char **OutUtf8) {
	if (g_runtime == nullptr) {
		if (OutUtf8 != nullptr) {
			*OutUtf8 = nullptr;
		}
		return kNotBooted;
	}
	return g_runtime->base_type(ClassNameUtf8, OutUtf8);
}

// godot-natives.md §11.1: implemented unless the nearest class above the script's own that is not
// a script class resolves the name to the same body.
vh_bool vh_instance_has_function(vh_instance *Instance, const char *DecoratedName) {
	if (g_runtime == nullptr || Instance == nullptr || DecoratedName == nullptr) {
		return 0;
	}
	const vm::NameCell *name = g_runtime->heap.find_interned(DecoratedName);
	if (name == nullptr || !vm::is_cell_kind(Instance->object, vm::CellKind::Object)) {
		return 0;
	}
	const vm::ObjectCell *object = vm::cell_as<vm::ObjectCell>(Instance->object);
	const vm::Cell *own = object->layout != nullptr ? callee_of(object->layout->find(name)) : nullptr;
	if (own == nullptr) {
		return 0;
	}
	vm::Interpreter &interpreter = g_runtime->interpreter;
	for (const vm::ClassCell *current = superclass_of(object->object_class); current != nullptr; current = superclass_of(current)) {
		const vm::ClassIndexEntry *entry = g_runtime->program.entry_for(current);
		if (entry != nullptr && entry->origin == vm::ClassOrigin::Script) {
			continue;
		}
		return callee_of(interpreter.layouts.get(current).find(name)) == own ? 0 : 1;
	}
	return 1;
}

int32_t vh_instance_call(vh_instance *Instance, const char *DecoratedName, const vh_value *Args, int32_t ArgCount, vh_arena *Arena, vh_value *OutResult) {
	if (g_runtime == nullptr) {
		return kNotBooted;
	}
	if (!g_runtime->on_init_thread()) {
		g_runtime->refuse_thread(DecoratedName != nullptr ? DecoratedName : "A method");
		return VH_ERR_THREAD;
	}
	if (Instance == nullptr || DecoratedName == nullptr || ArgCount < 0 || (ArgCount > 0 && Args == nullptr)) {
		return VH_ERR_ARGUMENT;
	}
	vm::Heap &heap = g_runtime->heap;
	vm::Interpreter &interpreter = g_runtime->interpreter;
	const vm::NameCell *name = heap.find_interned(DecoratedName);
	vm::Value function;
	if (name == nullptr || !interpreter.resolve_method(Instance->object, name, function)) {
		return VH_ERR_NOT_FOUND;
	}
	vm::RootScope function_root(heap, &function);
	const vm::SidecarMethod *method = find_method(Instance->sidecar_class, DecoratedName);
	if (method != nullptr && (ArgCount < method->required || size_t(ArgCount) > method->params.size())) {
		return VH_ERR_ARGUMENT;
	}
	Instance->sealed = true;
	vm::GodotBridge &bridge = g_runtime->bridge;
	bridge.ensure_event_connections(Instance);
	const CallDepth call_depth;

	const ScopeRestore restore{ interpreter, interpreter.active_scope };
	g_runtime->activate_scope(Instance->scope);
	interpreter.begin_entry();

	// Arguments past the procedure's positional ones are its named parameters, in declaration
	// order (spec/calls.md §8). Converting one may build the object a handle names, which is Verse.
	const vm::SidecarMethodTypes *types = bridge.method_types(DecoratedName);
	const vm::Cell *callee = vm::cell_as<vm::FunctionCell>(function)->callee;
	const uint32_t positional_count = callee->kind == vm::CellKind::Procedure
			? static_cast<const vm::ProcedureCell *>(callee)->positional_count
			: static_cast<const vm::NativeProcedureCell *>(callee)->positional_count;
	std::vector<vm::Value> converted;
	std::string why;
	bool shaped = true;
	if (types != nullptr && types->params.size() >= size_t(ArgCount)) {
		shaped = bridge.wire_to_arguments(types, Args, ArgCount, false, converted, why);
	} else {
		for (int32_t index = 0; index < ArgCount && shaped; ++index) {
			const int32_t declared = method != nullptr ? method->params[size_t(index)].type : Args[index].Type;
			vm::Value value;
			shaped = vm::wire_to_value(heap, Args[index], declared, value);
			converted.push_back(value);
		}
	}
	if (!shaped) {
		interpreter.end_entry(false);
		return VH_ERR_ARGUMENT;
	}
	std::vector<vm::Value> positional;
	std::vector<vm::NamedArgument> named;
	for (size_t index = 0; index < converted.size(); ++index) {
		if (uint32_t(index) < positional_count || method == nullptr) {
			positional.push_back(converted[index]);
		} else {
			named.push_back(vm::NamedArgument{ heap.intern(method->params[index].name), converted[index] });
		}
	}

	vm::Value result;
	vm::RootScope result_root(heap, &result);
	const vm::Outcome outcome = interpreter.invoke(function, Instance->object, positional, named, result);
	if (outcome == vm::Outcome::Ok && OutResult != nullptr) {
		vh_arena *arena = Arena != nullptr ? Arena : &g_runtime->result_arena(call_depth.depth);
		vh_value wire = {};
		const int32_t result_type = method != nullptr ? method->result : VH_TYPE_VOID;
		const int32_t result_tag = method != nullptr ? method->result_tag : 0;
		const bool carried = types != nullptr && result_type != VH_TYPE_VOID
				? bridge.member_to_wire(result, types->result, arena, wire, why)
				: vm::value_to_wire(result, result_type, result_tag, arena, wire, why);
		if (!carried) {
			interpreter.end_entry(false);
			vm::RaisedError raised;
			raised.error.diagnostic = "ErrRuntime_Internal";
			raised.error.description = "An internal runtime error occurred. There is no other information available.";
			raised.error.message = std::string("The result of ") + DecoratedName + " could not be handed to the host: " + why + ".";
			g_runtime->report_raised(raised);
			collect_after_entry();
			return VH_ERR_RUNTIME;
		}
		*OutResult = wire;
	}
	const int32_t status = entry_status(outcome);
	collect_after_entry();
	return status;
}

// godot-natives.md §9.5: a method call, or a wait or an event resumed, each in an entry of its own.
int32_t vh_callback_invoke(int64_t CallbackId, const vh_value *Args, int32_t ArgCount, vh_arena *Arena, vh_value *OutResult) {
	if (g_runtime == nullptr) {
		return kNotBooted;
	}
	if (!g_runtime->on_init_thread()) {
		g_runtime->refuse_thread("A Verse callback");
		return VH_ERR_THREAD;
	}
	if (ArgCount < 0 || (ArgCount > 0 && Args == nullptr)) {
		return VH_ERR_ARGUMENT;
	}
	vm::GodotBridge &bridge = g_runtime->bridge;
	vm::Interpreter &interpreter = g_runtime->interpreter;
	vh_instance *owner = nullptr;
	if (!bridge.callback_owner(CallbackId, owner)) {
		return VH_ERR_NOT_FOUND;
	}
	const CallDepth call_depth;
	const ScopeRestore restore{ interpreter, interpreter.active_scope };
	g_runtime->activate_scope(owner != nullptr ? owner->scope : g_runtime->project_scope);
	interpreter.begin_entry();
	vm::GodotBridge::CallbackTarget target;
	bridge.resolve_callback(CallbackId, Args, ArgCount, target);
	vm::RootScope function_root(g_runtime->heap, &target.function);
	vm::RootScope event_root(g_runtime->heap, &target.event);
	vm::RootScope payload_root(g_runtime->heap, &target.payload);
	vm::Outcome outcome = vm::Outcome::Ok;
	vm::Value result;
	vm::RootScope result_root(g_runtime->heap, &result);
	switch (target.delivery) {
		case vm::GodotBridge::Delivery::NotFound:
			interpreter.end_entry(false);
			return VH_ERR_NOT_FOUND;
		case vm::GodotBridge::Delivery::BadArguments:
			interpreter.end_entry(false);
			return VH_ERR_ARGUMENT;
		case vm::GodotBridge::Delivery::Noop:
			break;
		case vm::GodotBridge::Delivery::Signal:
			outcome = interpreter.signal_event(target.event, target.payload);
			break;
		case vm::GodotBridge::Delivery::Call:
			outcome = interpreter.invoke(target.function, vm::Value(), target.arguments, {}, result);
			if (outcome == vm::Outcome::Ok && OutResult != nullptr && target.types != nullptr) {
				vh_arena *arena = Arena != nullptr ? Arena : &g_runtime->result_arena(call_depth.depth);
				vh_value wire = {};
				std::string why;
				if (target.types->result.described.type != VH_TYPE_VOID && bridge.member_to_wire(result, target.types->result, arena, wire, why)) {
					*OutResult = wire;
				}
			}
			break;
	}
	const int32_t status = entry_status(outcome);
	collect_after_entry();
	return status;
}

void vh_callback_release(int64_t CallbackId) {
	if (g_runtime != nullptr) {
		g_runtime->bridge.release_callback(CallbackId);
	}
}

int32_t vh_class_export_list(const char *ClassNameUtf8, const vh_export_desc **OutExports, int32_t *OutCount) {
	return g_runtime == nullptr ? refuse_list(OutExports, OutCount) : g_runtime->export_list(ClassNameUtf8, OutExports, OutCount);
}

int32_t vh_instance_get_field(vh_instance *Instance, const char *NameUtf8, const vh_value **OutValue) {
	if (OutValue != nullptr) {
		*OutValue = nullptr;
	}
	if (g_runtime == nullptr) {
		return kNotBooted;
	}
	if (Instance == nullptr || NameUtf8 == nullptr || OutValue == nullptr) {
		return VH_ERR_ARGUMENT;
	}
	const vm::SidecarMemberType *type = find_member_type(Instance->sidecar_class, NameUtf8);
	vm::GodotBridge &bridge = g_runtime->bridge;
	const vm::Value value = bridge.field_value(Instance->object, NameUtf8);
	if (type == nullptr || value.is_empty() || vm::is_unbound_placeholder(value)) {
		return VH_ERR_NOT_FOUND;
	}
	g_runtime->field_arena.reset();
	std::string why;
	if (!bridge.member_to_wire(value, *type, &g_runtime->field_arena, g_runtime->field_answer, why)) {
		return VH_ERR_NOT_FOUND;
	}
	*OutValue = &g_runtime->field_answer;
	return VH_OK;
}

int32_t vh_class_default_field(const char *ClassNameUtf8, const char *NameUtf8, const vh_value **OutValue) {
	if (g_runtime == nullptr) {
		if (OutValue != nullptr) {
			*OutValue = nullptr;
		}
		return kNotBooted;
	}
	return g_runtime->default_field(ClassNameUtf8, NameUtf8, OutValue);
}

namespace {

// The slot a member write lands in, or null for a member that is absent, a shape constant, or a
// non-var one the first call has sealed (include/verse_host_abi.h, vh_instance_set_field).
vm::Value *writable_slot(vh_instance *p_instance, const char *p_name, const vm::SidecarMemberType *&r_type) {
	r_type = find_member_type(p_instance->sidecar_class, p_name);
	if (r_type == nullptr || (p_instance->sealed && !r_type->is_var) || !vm::is_cell_kind(p_instance->object, vm::CellKind::Object)) {
		return nullptr;
	}
	return g_runtime->bridge.field_slot(vm::cell_as<vm::ObjectCell>(p_instance->object), p_name);
}

// A `var` holds its content melted, which every read of it freezes again (spec/values.md §7).
void store(vm::Value &r_slot, vm::Value p_value) {
	vm::Value held = vm::read_slot(r_slot);
	if (!vm::is_cell_kind(held, vm::CellKind::Ref)) {
		held = r_slot;
	}
	if (vm::is_cell_kind(held, vm::CellKind::Ref)) {
		vm::Value melted = p_value;
		vm::melt(g_runtime->heap, p_value, melted);
		vm::cell_as<vm::RefCell>(held)->content = melted;
		return;
	}
	r_slot = p_value;
}

} // namespace

int32_t vh_instance_set_field(vh_instance *Instance, const char *NameUtf8, const vh_value *Value) {
	if (g_runtime == nullptr) {
		return kNotBooted;
	}
	if (Instance == nullptr || NameUtf8 == nullptr || Value == nullptr) {
		return VH_ERR_ARGUMENT;
	}
	if (!g_runtime->on_init_thread()) {
		return VH_ERR_THREAD;
	}
	const vm::SidecarMemberType *type = nullptr;
	vm::Value *slot = writable_slot(Instance, NameUtf8, type);
	if (slot == nullptr) {
		return VH_ERR_NOT_FOUND;
	}
	vm::Interpreter &interpreter = g_runtime->interpreter;
	const ScopeRestore restore{ interpreter, interpreter.active_scope };
	g_runtime->activate_scope(Instance->scope);
	interpreter.begin_entry();
	vm::Value value;
	vm::RootScope value_root(g_runtime->heap, &value);
	std::string why;
	if (!g_runtime->bridge.wire_to_member(*Value, *type, value, why)) {
		interpreter.end_entry(false);
		return VH_ERR_ARGUMENT;
	}
	store(*slot, value);
	const int32_t status = entry_status(vm::Outcome::Ok);
	collect_after_entry();
	return status;
}

int32_t vh_instance_set_field_instance(vh_instance *Instance, const char *NameUtf8, vh_instance *Value) {
	if (g_runtime == nullptr) {
		return kNotBooted;
	}
	if (Instance == nullptr || NameUtf8 == nullptr) {
		return VH_ERR_ARGUMENT;
	}
	const vm::SidecarMemberType *type = nullptr;
	vm::Value *slot = writable_slot(Instance, NameUtf8, type);
	if (slot == nullptr || !type->has_ref) {
		return VH_ERR_NOT_FOUND;
	}
	vm::Heap &heap = g_runtime->heap;
	if (Value == nullptr) {
		if (!type->ref_option) {
			return VH_ERR_NOT_FOUND;
		}
		store(*slot, heap.false_value());
		return VH_OK;
	}
	const vm::ClassIndexEntry *declared = g_runtime->program.find_class(
			type->ref_origin == 2 ? vm::ClassOrigin::Script : type->ref_origin == 1 ? vm::ClassOrigin::Mirrored : vm::ClassOrigin::Binding, type->ref);
	if (!vm::is_cell_kind(Value->object, vm::CellKind::Object) ||
			(declared != nullptr && !vm::class_inherits(vm::cell_as<vm::ObjectCell>(Value->object)->object_class, declared->class_cell))) {
		return VH_ERR_NOT_FOUND;
	}
	store(*slot, type->ref_option ? vm::make_option(heap, Value->object) : Value->object);
	return VH_OK;
}

// R-NODE-10: the class's ToString extension method, a module-level function taking the receiver
// and the call's own (empty) argument tuple.
int32_t vh_instance_to_string(vh_instance *Instance, const vh_value **OutValue) {
	if (OutValue != nullptr) {
		*OutValue = nullptr;
	}
	if (g_runtime == nullptr) {
		return kNotBooted;
	}
	if (Instance == nullptr || OutValue == nullptr) {
		return VH_ERR_ARGUMENT;
	}
	if (!g_runtime->on_init_thread()) {
		return VH_ERR_THREAD;
	}
	if (Instance->sidecar_class == nullptr || Instance->sidecar_class->to_string.empty()) {
		return VH_ERR_NOT_FOUND;
	}
	vm::Value function = g_runtime->bridge.definition(Instance->sidecar_class->to_string);
	if (!vm::is_cell_kind(function, vm::CellKind::Function)) {
		return VH_ERR_NOT_FOUND;
	}
	vm::Interpreter &interpreter = g_runtime->interpreter;
	vm::Heap &heap = g_runtime->heap;
	const ScopeRestore restore{ interpreter, interpreter.active_scope };
	g_runtime->activate_scope(Instance->scope);
	interpreter.begin_entry();
	vm::Value result;
	vm::RootScope result_root(heap, &result);
	const std::vector<vm::Value> arguments = { Instance->object, vm::make_array(heap, {}, false) };
	const vm::Outcome outcome = interpreter.invoke(function, vm::Value(), arguments, {}, result);
	std::string text;
	const bool is_text = outcome == vm::Outcome::Ok && vm::string_bytes(vm::follow(result), text);
	const int32_t status = entry_status(outcome);
	collect_after_entry();
	if (status != VH_OK || !is_text) {
		return status != VH_OK ? status : VH_ERR_NOT_FOUND;
	}
	g_runtime->string_arena.reset();
	g_runtime->string_answer = vh_value{};
	std::string why;
	if (!vm::value_to_wire(vm::make_string(heap, text), VH_TYPE_STRING, VH_VARIANT_STRING, &g_runtime->string_arena, g_runtime->string_answer, why)) {
		return VH_ERR_NOT_FOUND;
	}
	*OutValue = &g_runtime->string_answer;
	return VH_OK;
}

int32_t vh_lookup_symbol(const char *PathUtf8, int32_t Line, int32_t Column, const vh_lookup_desc **OutResult) {
	(void)PathUtf8;
	(void)Line;
	(void)Column;
	if (OutResult != nullptr) {
		*OutResult = nullptr;
	}
	return kCompilerOnly;
}

int32_t vh_complete_symbol(const char *PathUtf8, const char *SourceUtf8, int32_t Line, int32_t Column, int32_t Mode, const vh_complete_item **OutItems, int32_t *OutCount) {
	(void)PathUtf8;
	(void)SourceUtf8;
	(void)Line;
	(void)Column;
	(void)Mode;
	if (OutItems != nullptr) {
		*OutItems = nullptr;
	}
	if (OutCount != nullptr) {
		*OutCount = 0;
	}
	return kCompilerOnly;
}

int32_t vh_resolve_unknown_name(const char *NameUtf8, const vh_module_ref **OutModules, int32_t *OutCount) {
	(void)NameUtf8;
	if (OutModules != nullptr) {
		*OutModules = nullptr;
	}
	if (OutCount != nullptr) {
		*OutCount = 0;
	}
	return kCompilerOnly;
}

int32_t vh_class_members(const char *ClassNameUtf8, const vh_complete_item **OutItems, int32_t *OutCount) {
	(void)ClassNameUtf8;
	if (OutItems != nullptr) {
		*OutItems = nullptr;
	}
	if (OutCount != nullptr) {
		*OutCount = 0;
	}
	return kCompilerOnly;
}

int32_t vh_class_override_candidates(const char *ClassNameUtf8, const vh_complete_item **OutItems, int32_t *OutCount) {
	(void)ClassNameUtf8;
	if (OutItems != nullptr) {
		*OutItems = nullptr;
	}
	if (OutCount != nullptr) {
		*OutCount = 0;
	}
	return kCompilerOnly;
}

int32_t vh_signature_at(const char *PathUtf8, const char *SourceUtf8, int32_t Line, int32_t Column, const vh_signature_desc **OutResult) {
	(void)PathUtf8;
	(void)SourceUtf8;
	(void)Line;
	(void)Column;
	if (OutResult != nullptr) {
		*OutResult = nullptr;
	}
	return kCompilerOnly;
}

int32_t vh_debug_set_enabled(vh_bool Enabled) {
	(void)Enabled;
	return kNotBooted;
}

int32_t vh_debug_stack_count(int32_t *OutCount) {
	if (OutCount != nullptr) {
		*OutCount = 0;
	}
	return kNotBooted;
}

int32_t vh_debug_stack_frame(int32_t Level, const vh_debug_frame **OutFrame) {
	(void)Level;
	if (OutFrame != nullptr) {
		*OutFrame = nullptr;
	}
	return kNotBooted;
}

int32_t vh_debug_stack_values(int32_t Level, int32_t Kind, const vh_debug_value **OutValues, int32_t *OutCount) {
	(void)Level;
	(void)Kind;
	if (OutValues != nullptr) {
		*OutValues = nullptr;
	}
	if (OutCount != nullptr) {
		*OutCount = 0;
	}
	return kNotBooted;
}

int32_t vh_profiling_set_enabled(vh_bool Enabled) {
	(void)Enabled;
	return kNotBooted;
}

int32_t vh_profiling_read(vh_bool FrameOnly, const vh_profile_row **OutRows, int32_t *OutCount) {
	(void)FrameOnly;
	if (OutRows != nullptr) {
		*OutRows = nullptr;
	}
	if (OutCount != nullptr) {
		*OutCount = 0;
	}
	return kNotBooted;
}
