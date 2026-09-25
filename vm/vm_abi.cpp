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
#include "vm_runtime.h"

// One live script object the host holds. Its object is a root for as long as the host holds it
// (design §7.3).
struct vh_instance {
	vm::Value object;
	// The instance's content scope (spec/tasks.md §8.1): made at vh_instantiate, active for every
	// call into it, replaced after a raise terminated it, terminated at vh_release_instance.
	vm::Value scope;
	vh_handle handle = 0;
	const vm::SidecarClass *sidecar_class = nullptr;
};

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

void vh_shutdown(void) {
	delete g_runtime;
	g_runtime = nullptr;
}

// The budget governs queued work, and this runtime queues none: a due sleeper is not subject to it
// (godot-natives.md §10), so JobsPending and Overran are always zero.
void vh_tick(double BudgetSeconds, vh_tick_stats *OutStats) {
	(void)BudgetSeconds;
	vh_tick_stats stats = {};
	if (g_runtime != nullptr) {
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

vh_bool vh_instance_has_function(vh_instance *Instance, const char *DecoratedName) {
	(void)Instance;
	(void)DecoratedName;
	return 0;
}

int32_t vh_instance_call(vh_instance *Instance, const char *DecoratedName, const vh_value *Args, int32_t ArgCount, vh_arena *Arena, vh_value *OutResult) {
	if (g_runtime == nullptr) {
		return kNotBooted;
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

	// Arguments past the procedure's positional ones are its named parameters, in declaration
	// order (spec/calls.md §8).
	const vm::Cell *callee = vm::cell_as<vm::FunctionCell>(function)->callee;
	const uint32_t positional_count = callee->kind == vm::CellKind::Procedure
			? static_cast<const vm::ProcedureCell *>(callee)->positional_count
			: static_cast<const vm::NativeProcedureCell *>(callee)->positional_count;
	std::vector<vm::Value> positional;
	std::vector<vm::NamedArgument> named;
	for (int32_t index = 0; index < ArgCount; ++index) {
		const int32_t declared = method != nullptr ? method->params[size_t(index)].type : Args[index].Type;
		vm::Value value;
		if (!vm::wire_to_value(heap, Args[index], declared, value)) {
			return VH_ERR_ARGUMENT;
		}
		if (uint32_t(index) < positional_count || method == nullptr) {
			positional.push_back(value);
		} else {
			named.push_back(vm::NamedArgument{ heap.intern(method->params[size_t(index)].name), value });
		}
	}

	const ScopeRestore restore{ interpreter, interpreter.active_scope };
	g_runtime->activate_scope(Instance->scope);
	interpreter.begin_entry();
	vm::Value result;
	vm::RootScope result_root(heap, &result);
	const vm::Outcome outcome = interpreter.invoke(function, Instance->object, positional, named, result);
	if (outcome == vm::Outcome::Ok && OutResult != nullptr && Arena != nullptr) {
		std::string why;
		vh_value wire = {};
		const int32_t result_type = method != nullptr ? method->result : VH_TYPE_VOID;
		const int32_t result_tag = method != nullptr ? method->result_tag : 0;
		if (!vm::value_to_wire(result, result_type, result_tag, Arena, wire, why)) {
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

int32_t vh_callback_invoke(int64_t CallbackId, const vh_value *Args, int32_t ArgCount, vh_arena *Arena, vh_value *OutResult) {
	(void)CallbackId;
	(void)Args;
	(void)ArgCount;
	(void)Arena;
	(void)OutResult;
	return kNotBooted;
}

void vh_callback_release(int64_t CallbackId) {
	(void)CallbackId;
}

int32_t vh_class_export_list(const char *ClassNameUtf8, const vh_export_desc **OutExports, int32_t *OutCount) {
	return g_runtime == nullptr ? refuse_list(OutExports, OutCount) : g_runtime->export_list(ClassNameUtf8, OutExports, OutCount);
}

int32_t vh_instance_get_field(vh_instance *Instance, const char *NameUtf8, const vh_value **OutValue) {
	(void)Instance;
	(void)NameUtf8;
	if (OutValue != nullptr) {
		*OutValue = nullptr;
	}
	return kNotBooted;
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

int32_t vh_instance_set_field(vh_instance *Instance, const char *NameUtf8, const vh_value *Value) {
	(void)Instance;
	(void)NameUtf8;
	(void)Value;
	return kNotBooted;
}

int32_t vh_instance_set_field_instance(vh_instance *Instance, const char *NameUtf8, vh_instance *Value) {
	(void)Instance;
	(void)NameUtf8;
	(void)Value;
	return kNotBooted;
}

int32_t vh_instance_to_string(vh_instance *Instance, const vh_value **OutValue) {
	(void)Instance;
	if (OutValue != nullptr) {
		*OutValue = nullptr;
	}
	return kNotBooted;
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
