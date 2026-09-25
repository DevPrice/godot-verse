#pragma once

#include "verse_host_abi.h"

#include <godot_cpp/variant/string.hpp>

#ifdef _WIN32
struct HINSTANCE__;
typedef struct HINSTANCE__ *HINSTANCE;
typedef HINSTANCE HMODULE;
#endif

// Thin GetProcAddress loader over include/verse_host_abi.h. No Verse or engine logic lives
// here; VerseRuntime owns the vh_init_desc and everything that happens after the symbols
// resolve.
class VerseHostLibrary {
public:
	VerseHostLibrary() = default;
	~VerseHostLibrary();

	VerseHostLibrary(const VerseHostLibrary &) = delete;
	VerseHostLibrary &operator=(const VerseHostLibrary &) = delete;

	// Loads dll_path and resolves every vh_* entry point. On failure, out_error names the
	// first missing *required* symbol (or the reason the library itself could not be loaded)
	// and the library is left unloaded. The compiler-side entries are optional: the runtime
	// host has no compiler and does not export them, so they stay null and their callers
	// answer ERR_UNAVAILABLE.
	bool load(const godot::String &dll_path, godot::String &out_error);

#ifdef VERSE_VM_STATIC
	// Fills every function pointer directly from vm/'s functions, compiled statically into this
	// library by `scons verse_vm=yes` (docs/phase-7.5-design.md §8). No module is loaded -- the
	// pointers name code already linked into this DLL -- so this cannot fail the way load() can,
	// and unload() clearing them back to null is all a matching teardown needs.
	bool load_static();

	// docs/web-vm/tasks.md T5.3's own check: every pointer load_static() should have set, so a
	// forgotten one is a false here rather than a segfault the first time something calls through
	// it -- there being no cooked directory for the vm backend to boot against yet (T5.4), this is
	// what stands in for running it end to end.
	bool all_pointers_assigned() const;
#endif

	void unload();
	bool is_loaded() const;

	// VH_HOST_EDITOR / _RUNTIME / _COOKER. Safe before vh_init, and safe against a host older
	// than ABI 8.2, which exports nothing to ask and was always the editor host.
	int32_t host_kind() const;

	vh_abi_version_fn AbiVersion = nullptr;
	vh_host_kind_fn HostKind = nullptr;
	vh_init_fn Init = nullptr;
	vh_shutdown_fn Shutdown = nullptr;
	vh_tick_fn Tick = nullptr;
	vh_compile_project_fn CompileProject = nullptr;
	vh_set_bindings_fn SetBindings = nullptr;
	vh_resolve_unknown_name_fn ResolveUnknownName = nullptr;
	vh_check_project_fn CheckProject = nullptr;
	vh_check_project_begin_fn CheckProjectBegin = nullptr;
	vh_check_project_poll_fn CheckProjectPoll = nullptr;
	vh_check_project_busy_fn CheckProjectBusy = nullptr;
	vh_has_class_fn HasClass = nullptr;
	vh_instantiate_fn Instantiate = nullptr;
	vh_release_instance_fn ReleaseInstance = nullptr;
	vh_instance_has_function_fn InstanceHasFunction = nullptr;
	vh_instance_call_fn InstanceCall = nullptr;
	vh_callback_invoke_fn CallbackInvoke = nullptr;
	vh_callback_release_fn CallbackRelease = nullptr;
	vh_class_method_list_fn ClassMethodList = nullptr;
	vh_class_signal_list_fn ClassSignalList = nullptr;
	vh_class_rpc_list_fn ClassRpcList = nullptr;
	vh_class_static_list_fn ClassStaticList = nullptr;
	vh_class_is_abstract_fn ClassIsAbstract = nullptr;
	vh_class_base_type_fn ClassBaseType = nullptr;
	vh_class_export_list_fn ClassExportList = nullptr;
	vh_instance_get_field_fn InstanceGetField = nullptr;
	vh_class_default_field_fn ClassDefaultField = nullptr;
	vh_instance_set_field_fn InstanceSetField = nullptr;
	vh_instance_set_field_instance_fn InstanceSetFieldInstance = nullptr;
	vh_instance_to_string_fn InstanceToString = nullptr;
	vh_lookup_symbol_fn LookupSymbol = nullptr;
	vh_complete_symbol_fn CompleteSymbol = nullptr;
	vh_class_members_fn ClassMembers = nullptr;
	vh_class_override_candidates_fn ClassOverrideCandidates = nullptr;
	vh_signature_at_fn SignatureAt = nullptr;
	vh_debug_set_enabled_fn DebugSetEnabled = nullptr;
	vh_debug_stack_count_fn DebugStackCount = nullptr;
	vh_debug_stack_frame_fn DebugStackFrame = nullptr;
	vh_debug_stack_values_fn DebugStackValues = nullptr;
	vh_profiling_set_enabled_fn ProfilingSetEnabled = nullptr;
	vh_profiling_read_fn ProfilingRead = nullptr;

private:
	void clear_function_pointers();

#ifdef _WIN32
	HMODULE module = nullptr;
#endif
	bool loaded = false;
};
