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
	CompileFile = nullptr;
	ReleaseScript = nullptr;
	ScriptHasFunction = nullptr;
	RunMain = nullptr;
	CallVoid = nullptr;
	CallVoidFloat = nullptr;
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
	ok = ok && resolve(handle, "vh_compile_file", CompileFile, r_error);
	ok = ok && resolve(handle, "vh_release_script", ReleaseScript, r_error);
	ok = ok && resolve(handle, "vh_script_has_function", ScriptHasFunction, r_error);
	ok = ok && resolve(handle, "vh_run_main", RunMain, r_error);
	ok = ok && resolve(handle, "vh_call_void", CallVoid, r_error);
	ok = ok && resolve(handle, "vh_call_void_float", CallVoidFloat, r_error);

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
