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
	// first missing symbol (or the reason the library itself could not be loaded) and the
	// library is left unloaded.
	bool load(const godot::String &dll_path, godot::String &out_error);
	void unload();
	bool is_loaded() const;

	vh_abi_version_fn AbiVersion = nullptr;
	vh_init_fn Init = nullptr;
	vh_shutdown_fn Shutdown = nullptr;
	vh_tick_fn Tick = nullptr;
	vh_compile_project_fn CompileProject = nullptr;
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
	vh_class_export_list_fn ClassExportList = nullptr;
	vh_instance_get_field_fn InstanceGetField = nullptr;
	vh_class_default_field_fn ClassDefaultField = nullptr;
	vh_instance_set_field_fn InstanceSetField = nullptr;
	vh_instance_set_field_instance_fn InstanceSetFieldInstance = nullptr;
	vh_lookup_symbol_fn LookupSymbol = nullptr;
	vh_complete_symbol_fn CompleteSymbol = nullptr;
	vh_class_members_fn ClassMembers = nullptr;
	vh_signature_at_fn SignatureAt = nullptr;

private:
	void clear_function_pointers();

#ifdef _WIN32
	HMODULE module = nullptr;
#endif
	bool loaded = false;
};
