// The twelve compiler entry points answer VH_ERR_UNSUPPORTED whatever the state, as a
// WITH_VERSE_COMPILER=0 UE host does (VH_REFUSE_WITHOUT_COMPILER): vh_set_bindings, vh_compile_project,
// the four vh_check_project*, vh_lookup_symbol, vh_complete_symbol, vh_resolve_unknown_name,
// vh_class_members, vh_class_override_candidates and vh_signature_at. vh_check_project_busy answers
// 0, having no status to refuse through.
#include "verse_host_abi.h"

#include <cstddef>
#include <cstring>
#include <string>

#include "vm_runtime.h"

namespace {

constexpr int32_t kNotBooted = VH_ERR_STATE;
constexpr int32_t kCompilerOnly = VH_ERR_UNSUPPORTED;
// Entry points a later task implements (T3.4 execution, T3.6 defaults), after vh_init.
constexpr int32_t kNotYet = VH_ERR_UNSUPPORTED;

// The ABI is a set of free functions over one runtime, so this is the one piece of global state.
vm::Runtime *g_runtime = nullptr;

int32_t not_booted_or_not_yet() {
	return g_runtime == nullptr ? kNotBooted : kNotYet;
}

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
	runtime->on_diagnostic = desc.OnDiagnostic;
	runtime->diagnostic_ctx = desc.DiagnosticCtx;
	runtime->on_runtime_error = desc.OnRuntimeError;
	runtime->runtime_error_ctx = desc.RuntimeErrorCtx;

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

void vh_tick(double BudgetSeconds, vh_tick_stats *OutStats) {
	(void)BudgetSeconds;
	if (OutStats == nullptr) {
		return;
	}
	// "Filled up to its own StructSize" is the real ABI's contract; honouring it here means never
	// writing past what an older consumer reserved.
	const int32_t requested = OutStats->StructSize;
	const int32_t clamped = (requested > 0 && requested <= static_cast<int32_t>(sizeof(vh_tick_stats)))
			? requested
			: static_cast<int32_t>(sizeof(vh_tick_stats));
	std::memset(OutStats, 0, static_cast<size_t>(clamped));
	OutStats->StructSize = requested;
}

void vh_collect_garbage(void) {
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
	(void)ClassNameUtf8;
	(void)Handle;
	if (OutInstance != nullptr) {
		*OutInstance = nullptr;
	}
	return not_booted_or_not_yet();
}

void vh_release_instance(vh_instance *Instance) {
	(void)Instance;
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
	(void)Instance;
	(void)DecoratedName;
	(void)Args;
	(void)ArgCount;
	(void)Arena;
	(void)OutResult; // a failed call writes no result, matching the real ABI's own rule
	return kNotBooted;
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
	(void)ClassNameUtf8;
	(void)NameUtf8;
	if (OutValue != nullptr) {
		*OutValue = nullptr;
	}
	return not_booted_or_not_yet();
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
