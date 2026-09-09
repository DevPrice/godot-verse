#include "register_types.h"

#include <gdextension_interface.h>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/defs.hpp>
#include <godot_cpp/core/memory.hpp>
#include <godot_cpp/godot.hpp>

#include "verse_runtime.h"
#include "verse_ticker.h"

using namespace godot;

static VerseRuntime *verse_runtime_singleton = nullptr;

void initialize_gdextension_types(const ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}

	GDREGISTER_CLASS(VerseRuntime);
	GDREGISTER_CLASS(VerseTicker);

	verse_runtime_singleton = memnew(VerseRuntime);
	Engine::get_singleton()->register_singleton("VerseRuntime", verse_runtime_singleton);
}

void uninitialize_gdextension_types(const ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}

	if (verse_runtime_singleton == nullptr) {
		return;
	}

	Engine::get_singleton()->unregister_singleton("VerseRuntime");
	memdelete(verse_runtime_singleton);
	verse_runtime_singleton = nullptr;
}

extern "C" {
GDExtensionBool GDE_EXPORT godot_verse_init(GDExtensionInterfaceGetProcAddress p_get_proc_address, GDExtensionClassLibraryPtr p_library, GDExtensionInitialization *r_initialization) {
	GDExtensionBinding::InitObject init_obj(p_get_proc_address, p_library, r_initialization);

	init_obj.register_initializer(initialize_gdextension_types);
	init_obj.register_terminator(uninitialize_gdextension_types);
	init_obj.set_minimum_library_initialization_level(MODULE_INITIALIZATION_LEVEL_SCENE);

	return init_obj.init();
}
}
