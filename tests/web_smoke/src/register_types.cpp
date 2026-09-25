#include "register_types.h"

#include <gdextension_interface.h>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/godot.hpp>

#include "web_smoke_node.h"

using namespace godot;

void initialize_web_smoke_types(const ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}
	GDREGISTER_CLASS(WebSmokeNode);
}

void uninitialize_web_smoke_types(const ModuleInitializationLevel p_level) {
}

extern "C" {
GDExtensionBool GDE_EXPORT web_smoke_init(GDExtensionInterfaceGetProcAddress p_get_proc_address, GDExtensionClassLibraryPtr p_library, GDExtensionInitialization *r_initialization) {
	GDExtensionBinding::InitObject init_obj(p_get_proc_address, p_library, r_initialization);

	init_obj.register_initializer(initialize_web_smoke_types);
	init_obj.register_terminator(uninitialize_web_smoke_types);
	init_obj.set_minimum_library_initialization_level(MODULE_INITIALIZATION_LEVEL_SCENE);

	return init_obj.init();
}
}
