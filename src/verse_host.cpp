#include "verse_host.h"

#ifdef _WIN32
#include <windows.h>
#endif

using namespace godot;

#ifdef _WIN32
namespace {

template <typename T>
bool resolve_required(HMODULE p_module, const char *p_name, T &r_fn, String &r_error) {
	r_fn = reinterpret_cast<T>(GetProcAddress(p_module, p_name));
	if (r_fn == nullptr) {
		r_error = String("the Verse host is missing the symbol '") + String(p_name) + String("'");
		return false;
	}
	return true;
}

// An entry point the runtime host does not have, because it has no compiler (ABI 8.2). Absent is
// not an error: VerseRuntime guards each of these with a null test and answers ERR_UNAVAILABLE,
// which is the degradation verse_runtime.h already promises. Absent is also what a pre-8.2 host
// looks like for vh_host_kind, which is why that one is optional too.
template <typename T>
void resolve_optional(HMODULE p_module, const char *p_name, T &r_fn) {
	r_fn = reinterpret_cast<T>(GetProcAddress(p_module, p_name));
}

} // namespace
#endif

VerseHostLibrary::~VerseHostLibrary() {
	unload();
}

void VerseHostLibrary::clear_function_pointers() {
	AbiVersion = nullptr;
	HostKind = nullptr;
	Init = nullptr;
	Shutdown = nullptr;
	Tick = nullptr;
	CompileProject = nullptr;
	ResolveUnknownName = nullptr;
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
	//
	// This moves the process working directory, and so does vh_init; VerseRuntime::load_host_internal
	// puts it back around both.
	const String native_path = p_dll_path.replace("/", "\\");
	HMODULE handle = LoadLibraryExW((LPCWSTR)native_path.utf16().get_data(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
	if (handle == nullptr) {
		r_error = String("failed to load ") + p_dll_path + String(" (GetLastError=") + String::num_int64(GetLastError()) + String(")");
		return false;
	}

	bool ok = true;
	ok = ok && resolve_required(handle, "vh_abi_version", AbiVersion, r_error);
	ok = ok && resolve_required(handle, "vh_init", Init, r_error);
	ok = ok && resolve_required(handle, "vh_shutdown", Shutdown, r_error);
	ok = ok && resolve_required(handle, "vh_tick", Tick, r_error);
	ok = ok && resolve_required(handle, "vh_has_class", HasClass, r_error);
	ok = ok && resolve_required(handle, "vh_instantiate", Instantiate, r_error);
	ok = ok && resolve_required(handle, "vh_release_instance", ReleaseInstance, r_error);
	ok = ok && resolve_required(handle, "vh_instance_has_function", InstanceHasFunction, r_error);
	ok = ok && resolve_required(handle, "vh_instance_call", InstanceCall, r_error);
	ok = ok && resolve_required(handle, "vh_callback_invoke", CallbackInvoke, r_error);
	ok = ok && resolve_required(handle, "vh_callback_release", CallbackRelease, r_error);
	ok = ok && resolve_required(handle, "vh_class_method_list", ClassMethodList, r_error);
	ok = ok && resolve_required(handle, "vh_class_signal_list", ClassSignalList, r_error);
	ok = ok && resolve_required(handle, "vh_class_static_list", ClassStaticList, r_error);
	ok = ok && resolve_required(handle, "vh_class_is_abstract", ClassIsAbstract, r_error);
	ok = ok && resolve_required(handle, "vh_class_export_list", ClassExportList, r_error);
	ok = ok && resolve_required(handle, "vh_instance_get_field", InstanceGetField, r_error);
	ok = ok && resolve_required(handle, "vh_class_default_field", ClassDefaultField, r_error);
	ok = ok && resolve_required(handle, "vh_instance_set_field", InstanceSetField, r_error);
	ok = ok && resolve_required(handle, "vh_instance_set_field_instance", InstanceSetFieldInstance, r_error);
	ok = ok && resolve_required(handle, "vh_debug_set_enabled", DebugSetEnabled, r_error);
	ok = ok && resolve_required(handle, "vh_debug_stack_count", DebugStackCount, r_error);
	ok = ok && resolve_required(handle, "vh_debug_stack_frame", DebugStackFrame, r_error);
	ok = ok && resolve_required(handle, "vh_debug_stack_values", DebugStackValues, r_error);
	ok = ok && resolve_required(handle, "vh_profiling_set_enabled", ProfilingSetEnabled, r_error);
	ok = ok && resolve_required(handle, "vh_profiling_read", ProfilingRead, r_error);

	// The eleven that need a compiler, plus vh_host_kind, which a host older than 8.2 has not got.
	resolve_optional(handle, "vh_host_kind", HostKind);
	resolve_optional(handle, "vh_compile_project", CompileProject);
	resolve_optional(handle, "vh_resolve_unknown_name", ResolveUnknownName);
	resolve_optional(handle, "vh_check_project", CheckProject);
	resolve_optional(handle, "vh_check_project_begin", CheckProjectBegin);
	resolve_optional(handle, "vh_check_project_poll", CheckProjectPoll);
	resolve_optional(handle, "vh_check_project_busy", CheckProjectBusy);
	resolve_optional(handle, "vh_lookup_symbol", LookupSymbol);
	resolve_optional(handle, "vh_complete_symbol", CompleteSymbol);
	resolve_optional(handle, "vh_class_members", ClassMembers);
	resolve_optional(handle, "vh_class_override_candidates", ClassOverrideCandidates);
	resolve_optional(handle, "vh_signature_at", SignatureAt);

	if (!ok) {
		clear_function_pointers();
		FreeLibrary(handle);
		return false;
	}

	// Majors must match; a host with a *higher* minor is fine, because a minor addition is one
	// this consumer never asks about. The reverse is not: a host with a lower minor is missing
	// something this build was compiled expecting. That is the policy at the top of the header,
	// and vh_init applies the same test from the other side.
	const int32_t abi_version = AbiVersion();
	if (abi_version / 1000 != VH_ABI_VERSION_MAJOR || abi_version < VH_ABI_VERSION) {
		r_error = String("the Verse host's ABI version ") + String::num_int64(abi_version) + String(" does not match godot-verse's ") + String::num_int64(VH_ABI_VERSION) + String("; rebuild both");
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

int32_t VerseHostLibrary::host_kind() const {
	// Every host before ABI 8.2 was the editor host, and none of them exports vh_host_kind.
	return HostKind ? HostKind() : (int32_t)VH_HOST_EDITOR;
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
