#pragma once

#include "verse_script.h"

#include <gdextension_interface.h>

#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/variant/string_name.hpp>

namespace godot {
class Object;
}

// One Verse script bound to one Godot object.
//
// Not a godot::Object: Godot's script instances are raw GDExtensionScriptInstanceInfo3 vtables,
// and the pointer VerseScript::_instance_create hands back is what script_instance_create3
// returns with `this` as its data pointer. Godot owns that pointer and frees it through the
// vtable's free_func.
struct VerseScriptInstance {
	// Strong, the way GDScriptInstance holds its own script: during scene instantiation Godot
	// drops its reference to the script between creating the instance and calling into it, and
	// a borrowed pointer is dangling by the time _ready arrives.
	godot::Ref<VerseScript> script;
	godot::Object *owner = nullptr; // borrowed; the owner outlives its own script instance

	// Resolved once at attach time rather than looked up per frame.
	bool has_ready = false;
	bool has_process = false;
	bool has_physics_process = false;

	// Returns nullptr when the script has no compiled handle, which tells Godot to fall back to
	// a placeholder instance.
	static GDExtensionScriptInstancePtr create(VerseScript *p_script, godot::Object *p_owner);

	// Godot lifecycle name -> decorated Verse name, or nullptr for anything this phase does not
	// dispatch. The decoration is the host's, not Godot's: a plain Update(Delta:float) is stored
	// as Update(:float).
	static const char *verse_name_for(const godot::StringName &p_method);
};
