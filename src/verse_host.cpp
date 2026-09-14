#include "verse_host.h"

#ifdef _WIN32
#include <windows.h>
#endif

using namespace godot;

#ifdef _WIN32
namespace {

template <typename T>
bool resolve(HMODULE p_module, const char *p_name, T &r_fn, String &r_error) {
	r_fn = reinterpret_cast<T>(GetProcAddress(p_module, p_name));
	if (r_fn == nullptr) {
		r_error = String("verse_host.dll is missing the symbol '") + String(p_name) + String("'");
		return false;
	}
	return true;
}

} // namespace
#endif

VerseHostLibrary::~VerseHostLibrary() {
	unload();
}

void VerseHostLibrary::clear_function_pointers() {
	AbiVersion = nullptr;
	Init = nullptr;
	Shutdown = nullptr;
	Tick = nullptr;
	CompileProject = nullptr;
	CheckProject = nullptr;
	CheckProjectBegin = nullptr;
	CheckProjectPoll = nullptr;
	CheckProjectBusy = nullptr;
	HasClass = nullptr;
	Instantiate = nullptr;
	ReleaseInstance = nullptr;
	InstanceHasFunction = nullptr;
	InstanceCall = nullptr;
	CallbackInvoke = nullptr;
	CallbackRelease = nullptr;
	ClassMethodList = nullptr;
	ClassSignalList = nullptr;
	ClassStaticList = nullptr;
	ClassIsAbstract = nullptr;
	ClassExportList = nullptr;
	InstanceGetField = nullptr;
	ClassDefaultField = nullptr;
	InstanceSetField = nullptr;
	InstanceSetFieldInstance = nullptr;
	LookupSymbol = nullptr;
	CompleteSymbol = nullptr;
	ClassMembers = nullptr;
	ClassOverrideCandidates = nullptr;
	SignatureAt = nullptr;
	DebugSetEnabled = nullptr;
	DebugStackCount = nullptr;
	DebugStackFrame = nullptr;
	DebugStackValues = nullptr;
	ProfilingSetEnabled = nullptr;
	ProfilingRead = nullptr;
}

bool VerseHostLibrary::load(const String &p_dll_path, String &r_error) {
#ifdef _WIN32
	if (loaded) {
		unload();
	}

	// LOAD_WITH_ALTERED_SEARCH_PATH puts the host's own directory (which holds tbbmalloc.dll)
	// on the search path, but only for a fully qualified native path - forward slashes are not.
	const String native_path = p_dll_path.replace("/", "\\");
	HMODULE handle = LoadLibraryExW((LPCWSTR)native_path.utf16().get_data(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
	if (handle == nullptr) {
		r_error = String("failed to load ") + p_dll_path + String(" (GetLastError=") + String::num_int64(GetLastError()) + String(")");
		return false;
	}

	bool ok = true;
	ok = ok && resolve(handle, "vh_abi_version", AbiVersion, r_error);
	ok = ok && resolve(handle, "vh_init", Init, r_error);
	ok = ok && resolve(handle, "vh_shutdown", Shutdown, r_error);
	ok = ok && resolve(handle, "vh_tick", Tick, r_error);
	ok = ok && resolve(handle, "vh_compile_project", CompileProject, r_error);
	ok = ok && resolve(handle, "vh_resolve_unknown_name", ResolveUnknownName, r_error);
	ok = ok && resolve(handle, "vh_check_project", CheckProject, r_error);
	ok = ok && resolve(handle, "vh_check_project_begin", CheckProjectBegin, r_error);
	ok = ok && resolve(handle, "vh_check_project_poll", CheckProjectPoll, r_error);
	ok = ok && resolve(handle, "vh_check_project_busy", CheckProjectBusy, r_error);
	ok = ok && resolve(handle, "vh_has_class", HasClass, r_error);
	ok = ok && resolve(handle, "vh_instantiate", Instantiate, r_error);
	ok = ok && resolve(handle, "vh_release_instance", ReleaseInstance, r_error);
	ok = ok && resolve(handle, "vh_instance_has_function", InstanceHasFunction, r_error);
	ok = ok && resolve(handle, "vh_instance_call", InstanceCall, r_error);
	ok = ok && resolve(handle, "vh_callback_invoke", CallbackInvoke, r_error);
	ok = ok && resolve(handle, "vh_callback_release", CallbackRelease, r_error);
	ok = ok && resolve(handle, "vh_class_method_list", ClassMethodList, r_error);
	ok = ok && resolve(handle, "vh_class_signal_list", ClassSignalList, r_error);
	ok = ok && resolve(handle, "vh_class_static_list", ClassStaticList, r_error);
	ok = ok && resolve(handle, "vh_class_is_abstract", ClassIsAbstract, r_error);
	ok = ok && resolve(handle, "vh_class_export_list", ClassExportList, r_error);
	ok = ok && resolve(handle, "vh_instance_get_field", InstanceGetField, r_error);
	ok = ok && resolve(handle, "vh_class_default_field", ClassDefaultField, r_error);
	ok = ok && resolve(handle, "vh_instance_set_field", InstanceSetField, r_error);
	ok = ok && resolve(handle, "vh_instance_set_field_instance", InstanceSetFieldInstance, r_error);
	ok = ok && resolve(handle, "vh_lookup_symbol", LookupSymbol, r_error);
	ok = ok && resolve(handle, "vh_complete_symbol", CompleteSymbol, r_error);
	ok = ok && resolve(handle, "vh_class_members", ClassMembers, r_error);
	ok = ok && resolve(handle, "vh_class_override_candidates", ClassOverrideCandidates, r_error);
	ok = ok && resolve(handle, "vh_signature_at", SignatureAt, r_error);
	ok = ok && resolve(handle, "vh_debug_set_enabled", DebugSetEnabled, r_error);
	ok = ok && resolve(handle, "vh_debug_stack_count", DebugStackCount, r_error);
	ok = ok && resolve(handle, "vh_debug_stack_frame", DebugStackFrame, r_error);
	ok = ok && resolve(handle, "vh_debug_stack_values", DebugStackValues, r_error);
	ok = ok && resolve(handle, "vh_profiling_set_enabled", ProfilingSetEnabled, r_error);
	ok = ok && resolve(handle, "vh_profiling_read", ProfilingRead, r_error);

	if (!ok) {
		clear_function_pointers();
		FreeLibrary(handle);
		return false;
	}

	const int32_t abi_version = AbiVersion();
	if (abi_version != VH_ABI_VERSION) {
		r_error = String("verse_host.dll ABI version ") + String::num_int64(abi_version) + String(" does not match the godot-verse ABI version ") + String::num_int64(VH_ABI_VERSION);
		clear_function_pointers();
		FreeLibrary(handle);
		return false;
	}

	module = handle;
	loaded = true;
	return true;
#else
	r_error = "unsupported platform";
	return false;
#endif
}

void VerseHostLibrary::unload() {
	// The module is deliberately never freed. A monolithic UE runtime cannot survive
	// FreeLibrary: its static destructors and DllMain(DETACH) run after the engine has already
	// torn itself down in vh_shutdown, and the process hangs on the way out.
#ifdef _WIN32
	module = nullptr;
#endif
	clear_function_pointers();
	loaded = false;
}

bool VerseHostLibrary::is_loaded() const {
	return loaded;
}
