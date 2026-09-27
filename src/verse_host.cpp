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
#define VERSE_HOST_CLEAR(m_member, m_symbol, m_fn, m_presence, m_role) m_member = nullptr;
	VH_ENTRY_POINTS(VERSE_HOST_CLEAR)
#undef VERSE_HOST_CLEAR
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
#define VERSE_HOST_RESOLVE(m_member, m_symbol, m_fn, m_presence, m_role) \
	if constexpr (m_presence == VH_ENTRY_REQUIRED) { \
		ok = ok && resolve_required(handle, #m_symbol, m_member, r_error); \
	} else { \
		resolve_optional(handle, #m_symbol, m_member); \
	}
	VH_ENTRY_POINTS(VERSE_HOST_RESOLVE)
#undef VERSE_HOST_RESOLVE

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

#ifdef VERSE_VM_STATIC
bool VerseHostLibrary::load_static() {
	if (loaded) {
		unload();
	}

	// Every entry point, required and optional alike: vm/ defines them all (vm/vm_abi.cpp), the
	// compiler-side ones answering VH_ERR_UNSUPPORTED exactly as a WITH_VERSE_COMPILER=0 UE host does.
#define VERSE_HOST_BIND(m_member, m_symbol, m_fn, m_presence, m_role) m_member = &m_symbol;
	VH_ENTRY_POINTS(VERSE_HOST_BIND)
#undef VERSE_HOST_BIND

	loaded = true;
	return true;
}
#endif

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
