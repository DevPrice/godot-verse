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
	vh_compile_file_fn CompileFile = nullptr;
	vh_compile_project_fn CompileProject = nullptr;
	vh_open_script_fn OpenScript = nullptr;
	vh_release_script_fn ReleaseScript = nullptr;
	vh_script_has_function_fn ScriptHasFunction = nullptr;
	vh_run_main_fn RunMain = nullptr;
	vh_call_void_fn CallVoid = nullptr;
	vh_call_void_float_fn CallVoidFloat = nullptr;
	vh_has_class_fn HasClass = nullptr;
	vh_instantiate_fn Instantiate = nullptr;
	vh_release_instance_fn ReleaseInstance = nullptr;
	vh_instance_has_function_fn InstanceHasFunction = nullptr;
	vh_instance_call_void_fn InstanceCallVoid = nullptr;
	vh_instance_call_void_float_fn InstanceCallVoidFloat = nullptr;

private:
	void clear_function_pointers();

#ifdef _WIN32
	HMODULE module = nullptr;
#endif
	bool loaded = false;
};
