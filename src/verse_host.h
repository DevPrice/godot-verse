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
	// and the library is left unloaded. An optional entry point (VH_ENTRY_OPTIONAL) that is
	// absent stays null, and its callers answer ERR_UNAVAILABLE.
	bool load(const godot::String &dll_path, godot::String &out_error);

#ifdef VERSE_VM_STATIC
	// Fills every function pointer directly from vm/'s functions, compiled statically into this
	// library by `scons verse_vm=yes` (docs/phase-7.5-design.md §8). No module is loaded -- the
	// pointers name code already linked into this DLL -- so this cannot fail the way load() can,
	// and unload() clearing them back to null is all a matching teardown needs.
	bool load_static();
#endif

	void unload();
	bool is_loaded() const;

	// VH_HOST_EDITOR / _RUNTIME / _COOKER. Safe before vh_init, and safe against a host older
	// than ABI 8.2, which exports nothing to ask and was always the editor host.
	int32_t host_kind() const;

#define VERSE_HOST_MEMBER(m_member, m_symbol, m_fn, m_presence, m_role) m_fn m_member = nullptr;
	VH_ENTRY_POINTS(VERSE_HOST_MEMBER)
#undef VERSE_HOST_MEMBER

private:
	void clear_function_pointers();

#ifdef _WIN32
	HMODULE module = nullptr;
#endif
	bool loaded = false;
};
