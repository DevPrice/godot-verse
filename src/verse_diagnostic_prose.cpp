#include "verse_diagnostic_prose.h"

#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/typed_array.hpp>

using namespace godot;

// The two rules read as instructions because they have a fix the author can apply. The third does
// not: it is the bridge's own coverage, and saying so plainly is better than a suggestion that
// would not work.
String export_hint_attribute_name(vh_export_hint p_hint) {
	switch (p_hint) {
		case VH_EXPORT_HINT_FILE:
			return String("`@export_file`");
		case VH_EXPORT_HINT_DIR:
			return String("`@export_dir`");
		case VH_EXPORT_HINT_MULTILINE:
			return String("`@export_multiline`");
		case VH_EXPORT_HINT_FLAGS:
			return String("`@export_flags`");
		case VH_EXPORT_HINT_NODE_PATH:
			return String("`@export_node_path`");
		// Only called for VH_EXPORT_HINT_WRONG_TYPE, which is refused only for the five attribute
		// hints above -- these five are type-driven and never reach this function rejected this way.
		case VH_EXPORT_HINT_NONE:
		case VH_EXPORT_HINT_RANGE:
		case VH_EXPORT_HINT_ENUM:
		case VH_EXPORT_HINT_CLASS:
		case VH_EXPORT_HINT_SCRIPT_CLASS:
			return String("an inspector hint");
	}
	return String("an inspector hint");
}

String export_rejection_message(vh_export_reject p_reject, vh_export_hint p_hint,
		const String &p_name, const String &p_class_name, const String &p_native_class) {
	switch (p_reject) {
		case VH_EXPORT_OBJECT_NOT_OPTIONAL:
			return p_name + String(" is a ") + p_class_name
					+ String(", and the inspector may leave that slot empty. Declare it `?") + p_class_name
					+ String("` so the member can hold the empty case.");
		case VH_EXPORT_OPTION_NOT_OBJECT:
			return p_name + String(" is an option around a value the inspector has no empty slot for. ")
					+ String("Only a node or a resource can be left unassigned.");
		case VH_EXPORT_HINT_WRONG_TYPE: {
			// R-EXP-1's five exist *because* the type says nothing, so the author is the only one
			// who can pair them -- and a mispairing is silent otherwise: the attribute compiles and
			// Godot draws a plain field, which is also what no attribute at all draws.
			const char *wants = p_hint == VH_EXPORT_HINT_FLAGS ? "an `int`" : "a `string`";
			return p_name + String(" carries ") + export_hint_attribute_name(p_hint)
					+ String(", which describes ") + String(wants)
					+ String(" -- and this member is not one. The attribute exists because the ")
					+ String("declared type cannot say what a value is for, so the two have to be ")
					+ String("written to agree.");
		}
		case VH_EXPORT_SCRIPT_CLASS_NOT_GLOBAL:
			// Narrow since B19: a class with no registered Godot name is exported anyway, filtered
			// by its nearest mirrored ancestor. What is left here is the case with no such ancestor
			// either -- a class whose chain reaches `object` without passing a mirrored one -- so
			// there is nothing to filter a slot by at all.
			return p_name + String(" refers to ") + p_class_name
					+ String(", which is neither a node nor a resource, so the inspector has nothing ")
					+ String("to draw for it. Derive ") + p_class_name
					+ String(" from a Godot class the inspector can pick one of.");
		case VH_EXPORT_BINDING_CLASS_UNSUPPORTED:
			// Refused on purpose rather than folded into the generic sentence below: this member's
			// type is a GDScript class, reached through a generated binding rather than through the
			// mirror or the project's own @global_class, and @export cannot carry one of those yet
			// (docs/generated-bindings.md). NativeClassOf has already walked the binding's own
			// superclass chain for the nearest class the inspector *can* draw, so name it when there
			// is one to name.
			return p_name + String(" refers to ") + p_class_name
					+ String(", which is a GDScript class, and `@export` cannot carry a GDScript ")
					+ String("class reached through a generated binding yet.")
					+ (p_native_class.is_empty() ? String() : String(" Export its native base class `") + p_native_class + String("` instead."));
		case VH_EXPORT_OK:
		case VH_EXPORT_UNSUPPORTED_TYPE:
			return p_name + String(" has a type godot-verse cannot carry to the inspector yet, so it is not exported.");
	}
	return p_name + String(" has a type godot-verse cannot carry to the inspector yet, so it is not exported.");
}

String export_rejection_code(int64_t p_reject) {
	switch ((vh_export_reject)p_reject) {
		case VH_EXPORT_OBJECT_NOT_OPTIONAL:
			return String("OBJECT_EXPORT_NOT_OPTIONAL");
		case VH_EXPORT_OPTION_NOT_OBJECT:
			return String("OPTION_EXPORT_NOT_OBJECT");
		case VH_EXPORT_SCRIPT_CLASS_NOT_GLOBAL:
			return String("SCRIPT_CLASS_EXPORT_NOT_GLOBAL");
		case VH_EXPORT_HINT_WRONG_TYPE:
			return String("EXPORT_HINT_WRONG_TYPE");
		case VH_EXPORT_BINDING_CLASS_UNSUPPORTED:
			return String("EXPORT_BINDING_CLASS_UNSUPPORTED");
		case VH_EXPORT_OK:
		case VH_EXPORT_UNSUPPORTED_TYPE:
			return String("EXPORT_TYPE_UNSUPPORTED");
	}
	return String("EXPORT_TYPE_UNSUPPORTED");
}

// Every one of these was a runtime surprise before it was a warning, and three of them were silent:
// the member compiled, the signal was absent from Godot, and the author found out at the first
// emission or never. So each sentence names the rule and the edit that satisfies it.
String signal_rejection_message(const String &p_name, int32_t p_reject, const String &p_reject_detail) {
	switch ((vh_signal_reject)p_reject) {
		case VH_SIGNAL_IS_VAR:
			return p_name + String(" is a `var`, and a signal is an identity rather than a value. Its ")
					+ String("binding is made once against the object the member was built on, so ")
					+ String("reassigning it leaves the name pointing at nothing. Drop the `var`.");
		// Retired with the enumerator: the host does not test a member's access level, so this is
		// unreachable from a host built against this header. Kept for as long as the value is.
		case VH_SIGNAL_NOT_PUBLIC:
			return p_name + String(" is not `<public>`, so nothing outside the class can connect to it ")
					+ String("-- which is the only thing connecting ever is. Declare it `")
					+ p_name + String("<public>`.");
		case VH_SIGNAL_NO_GODOT_OWNER:
			return p_name + String(" is on a class that does not derive from `object`, so Godot never ")
					+ String("gives it an object to register the signal on. Unlike GDScript, where ")
					+ String("every class is an Object with a signal table of its own, a plain Verse ")
					+ String("class has no Godot counterpart at all.");
		case VH_SIGNAL_PAYLOAD_NESTED_STRUCT:
			return p_name + String(" has a payload whose field `") + p_reject_detail
					+ String("` is itself a struct. A struct payload becomes one Godot argument per ")
					+ String("top-level field, and Godot has no argument shape for a struct, so there ")
					+ String("is no second level to flatten into. Flatten the field, or carry it as ")
					+ String("one of the mirrored math types.");
		case VH_SIGNAL_PAYLOAD_UNSUPPORTED:
			return p_name + String(" has a payload argument `") + p_reject_detail
					+ String("` with no Godot type, so an emission would have nothing to carry it in.");
		case VH_SIGNAL_NEEDS_ATTRIBUTE:
			return p_name + String(" carries no `@export_signal`, so Godot is never told about it: it ")
					+ String("cannot be connected in the Node panel, emitted to, or seen from ")
					+ String("GDScript. The attribute is what registers a member, the way `@export` ")
					+ String("is what sends one to the inspector. Write `@export_signal` on the line ")
					+ String("above `") + p_name + String("`.");
		case VH_SIGNAL_OK:
			return p_name + String(" cannot be registered with Godot, so nothing can connect to it.");
	}
	return p_name + String(" cannot be registered with Godot, so nothing can connect to it.");
}

// What a refused `@rpc` has to say for itself, at the line that declared the method.
//
// Every one of these is silent otherwise: the attribute compiles -- its constructor only has to
// typecheck -- and the method is simply not in the config Godot reads, so the author finds out at
// the first call that goes nowhere, or never. GDScript's own messages are the model, and the first
// of them lists the seven words because guessing which one was meant is not this bridge's job.
String rpc_rejection_message(const String &p_name, int32_t p_reject, const String &p_reject_detail) {
	switch ((vh_rpc_reject)p_reject) {
		case VH_RPC_UNKNOWN_ARGUMENT:
			return p_name + String(": `") + p_reject_detail
					+ String("` is not an @rpc word. It must be one of \"call_local\"/\"call_remote\" ")
					+ String("(local calls), \"any_peer\"/\"authority\" (permission), or ")
					+ String("\"reliable\"/\"unreliable\"/\"unreliable_ordered\" (transfer mode).");
		case VH_RPC_DUPLICATE_CATEGORY:
			return p_name + String(": ") + p_reject_detail
					+ String(" is given twice. Each of the three may be said no more than once.");
		case VH_RPC_OK:
		case VH_RPC_BAD_ARGUMENT_TYPE:
			return p_name + String(": @rpc wants ") + p_reject_detail
					+ String(" in this position.");
	}
	return p_name + String(": @rpc wants ") + p_reject_detail + String(" in this position.");
}

String rpc_rejection_code(int32_t p_reject) {
	switch ((vh_rpc_reject)p_reject) {
		case VH_RPC_UNKNOWN_ARGUMENT:
			return String("RPC_UNKNOWN_ARGUMENT");
		case VH_RPC_DUPLICATE_CATEGORY:
			return String("RPC_DUPLICATE_CATEGORY");
		case VH_RPC_OK:
		case VH_RPC_BAD_ARGUMENT_TYPE:
			return String("RPC_BAD_ARGUMENT_TYPE");
	}
	return String("RPC_BAD_ARGUMENT_TYPE");
}

String signal_rejection_code(int32_t p_reject) {
	switch ((vh_signal_reject)p_reject) {
		case VH_SIGNAL_IS_VAR:
			return String("SIGNAL_IS_VAR");
		case VH_SIGNAL_NOT_PUBLIC:
			return String("SIGNAL_NOT_PUBLIC");
		case VH_SIGNAL_NO_GODOT_OWNER:
			return String("SIGNAL_NO_GODOT_OWNER");
		case VH_SIGNAL_PAYLOAD_NESTED_STRUCT:
			return String("SIGNAL_PAYLOAD_NESTED_STRUCT");
		case VH_SIGNAL_NEEDS_ATTRIBUTE:
			return String("SIGNAL_NEEDS_ATTRIBUTE");
		case VH_SIGNAL_OK:
		case VH_SIGNAL_PAYLOAD_UNSUPPORTED:
			return String("SIGNAL_PAYLOAD_UNSUPPORTED");
	}
	return String("SIGNAL_PAYLOAD_UNSUPPORTED");
}

String verse_formatted_diagnostic(const Dictionary &p_error) {
	return String(p_error["path"]) + ":" + String::num_int64((int64_t)p_error["line"]) + ":"
			+ String::num_int64((int64_t)p_error["column"]) + ": " + String(p_error["message"]);
}

PackedStringArray verse_flattened_diagnostics(const Dictionary &p_errors_by_path) {
	PackedStringArray flattened;
	const Array paths = p_errors_by_path.keys();
	for (int64_t i = 0; i < paths.size(); i++) {
		const TypedArray<Dictionary> errors = p_errors_by_path[paths[i]];
		for (int64_t e = 0; e < errors.size(); e++) {
			flattened.push_back(verse_formatted_diagnostic(errors[e]));
		}
	}
	return flattened;
}
