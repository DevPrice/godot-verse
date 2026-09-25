// The twelve compiler entry points answer VH_ERR_UNSUPPORTED whatever the state, as a
// WITH_VERSE_COMPILER=0 UE host does (VH_REFUSE_WITHOUT_COMPILER): vh_set_bindings, vh_compile_project,
// the four vh_check_project*, vh_lookup_symbol, vh_complete_symbol, vh_resolve_unknown_name,
// vh_class_members, vh_class_override_candidates and vh_signature_at. vh_check_project_busy answers
// 0, having no status to refuse through.
#include "verse_host_abi.h"

#include <cstring>

namespace {

constexpr int32_t kBootFailed = VH_ERR_INIT;
constexpr int32_t kNotBooted = VH_ERR_STATE;
constexpr int32_t kCompilerOnly = VH_ERR_UNSUPPORTED;

} // namespace

int32_t vh_abi_version(void) {
	return VH_ABI_VERSION;
}

int32_t vh_host_kind(void) {
	return VH_HOST_RUNTIME;
}

int32_t vh_init(const vh_init_desc *Desc) {
	(void)Desc;
	return kBootFailed;
}

void vh_shutdown(void) {
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
	(void)ClassNameUtf8;
	return 0;
}

int32_t vh_instantiate(const char *ClassNameUtf8, vh_handle Handle, vh_instance **OutInstance) {
	(void)ClassNameUtf8;
	(void)Handle;
	if (OutInstance != nullptr) {
		*OutInstance = nullptr;
	}
	return kNotBooted;
}

void vh_release_instance(vh_instance *Instance) {
	(void)Instance;
}

int32_t vh_class_method_list(const char *ClassNameUtf8, const vh_method_desc **OutMethods, int32_t *OutCount) {
	(void)ClassNameUtf8;
	if (OutMethods != nullptr) {
		*OutMethods = nullptr;
	}
	if (OutCount != nullptr) {
		*OutCount = 0;
	}
	return kNotBooted;
}

int32_t vh_class_signal_list(const char *ClassNameUtf8, const vh_signal_desc **OutSignals, int32_t *OutCount) {
	(void)ClassNameUtf8;
	if (OutSignals != nullptr) {
		*OutSignals = nullptr;
	}
	if (OutCount != nullptr) {
		*OutCount = 0;
	}
	return kNotBooted;
}

int32_t vh_class_rpc_list(const char *ClassNameUtf8, const vh_rpc_desc **OutRpcs, int32_t *OutCount) {
	(void)ClassNameUtf8;
	if (OutRpcs != nullptr) {
		*OutRpcs = nullptr;
	}
	if (OutCount != nullptr) {
		*OutCount = 0;
	}
	return kNotBooted;
}

int32_t vh_class_static_list(const char *ClassNameUtf8, const vh_static_desc **OutStatics, int32_t *OutCount) {
	(void)ClassNameUtf8;
	if (OutStatics != nullptr) {
		*OutStatics = nullptr;
	}
	if (OutCount != nullptr) {
		*OutCount = 0;
	}
	return kNotBooted;
}

vh_bool vh_class_is_abstract(const char *ClassNameUtf8) {
	(void)ClassNameUtf8;
	return 0;
}

int32_t vh_class_base_type(const char *ClassNameUtf8, const char **OutUtf8) {
	(void)ClassNameUtf8;
	if (OutUtf8 != nullptr) {
		*OutUtf8 = nullptr;
	}
	return kNotBooted;
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
	(void)ClassNameUtf8;
	if (OutExports != nullptr) {
		*OutExports = nullptr;
	}
	if (OutCount != nullptr) {
		*OutCount = 0;
	}
	return kNotBooted;
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
	return kNotBooted;
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
