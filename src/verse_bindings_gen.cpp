#include "verse_bindings_gen.h"

#include "verse_api_classes.h"

#include <godot_cpp/classes/class_db_singleton.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/script.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <set>
#include <unordered_map>

using namespace godot;

namespace {

std::string utf8_of(const String &p_text) {
	return std::string(p_text.utf8().get_data());
}

/// The mirrored Verse class for a Godot class, or empty. Binary search, because the table is
/// sorted by Godot name and this is asked once per class per generation.
std::string mirrored_verse_class(const String &p_godot_name) {
	const std::string name = utf8_of(p_godot_name);
	int64_t low = 0;
	int64_t high = (int64_t)std::size(verse_api::classes) - 1;
	while (low <= high) {
		const int64_t mid = low + ((high - low) / 2);
		const int cmp = name.compare(verse_api::classes[mid].godot_name);
		if (cmp == 0) {
			return verse_api::classes[mid].verse_name;
		}
		if (cmp > 0) {
			low = mid + 1;
		} else {
			high = mid - 1;
		}
	}
	return std::string();
}

/// The Verse type a Godot `Variant::Type` plus class name crosses as, or empty when the mirror has
/// no spelling for it.
///
/// Deliberately narrow. A member the generator cannot type is **left out** rather than guessed at:
/// a wrong type compiles and then fails at the call, where an absent one is the dynamic route the
/// author already had (R-INT-2). The set here is what a reader exists for in `verse_bindings.cpp`.
std::string verse_type_for(int64_t p_variant_type, const String &p_class_name) {
	switch (p_variant_type) {
		case Variant::NIL:
			return std::string(); // void for a result; refused for a parameter.
		case Variant::BOOL:
			return "logic";
		case Variant::INT:
			return "int";
		case Variant::FLOAT:
			return "float";
		case Variant::STRING:
		case Variant::STRING_NAME:
			return "string";
		case Variant::VECTOR2:
			return "vector2";
		case Variant::VECTOR3:
			return "vector3";
		case Variant::COLOR:
			return "color";
		case Variant::ARRAY:
			return "godot_array";
		case Variant::DICTIONARY:
			return "dictionary";
		case Variant::OBJECT: {
			// An object crosses as the mirrored class it is, and only as one the mirror carries:
			// a binding naming another binding would need the two emitted in dependency order,
			// which the roster does not give.
			const std::string mirrored = mirrored_verse_class(p_class_name);
			return mirrored.empty() ? std::string() : mirrored;
		}
		default:
			return std::string();
	}
}

/// A parameter name Verse will accept beside the class's inherited members.
///
/// Two collisions to dodge and the second is not obvious. Verse's own reserved words, and -- the
/// one that cost a round of the spikes -- **an inherited mirrored property of the same name**:
/// `Apply(Power:int, Scale:float)` on a `class(node2d)` is glitch 3532 against `node2d.Scale`,
/// reported at the parameter. An addon's parameter names are not ours to choose, so every one of
/// them is suffixed rather than tested against 3312 properties.
std::string safe_param_name(const String &p_godot_name, int64_t p_index) {
	std::string name = verse_binding_member_name(utf8_of(p_godot_name));
	if (name.empty()) {
		name = "Arg" + std::to_string(p_index);
	}
	return "In" + name;
}

/// Whether a ClassDB class is one the mirror already carries.
bool is_mirrored(const String &p_godot_name) {
	return !mirrored_verse_class(p_godot_name).empty();
}

/// The nearest ancestor with a Verse spelling: a mirrored class, since a binding's base is always
/// mirrored in this pass. Walks ClassDB, so a driver class registered under a mirrored parent
/// resolves to that parent.
std::string mirrored_base_of(ClassDBSingleton *p_db, const String &p_godot_name) {
	String cursor = p_db->get_parent_class(p_godot_name);
	while (!cursor.is_empty()) {
		const std::string mirrored = mirrored_verse_class(cursor);
		if (!mirrored.empty()) {
			return mirrored;
		}
		cursor = p_db->get_parent_class(cursor);
	}
	return std::string();
}

/// Reads one class's own methods and signals out of ClassDB.
void describe_from_classdb(ClassDBSingleton *p_db, const String &p_godot_name, VerseBindingClass &r_class) {
	// no_inheritance, or every binding re-declares its base's members and trips Verse's shadow
	// rule -- a member that shadows an inherited one is glitch 3532 at the declaration.
	const TypedArray<Dictionary> methods = p_db->class_get_method_list(p_godot_name, true);
	for (int64_t i = 0; i < methods.size(); i++) {
		const Dictionary method = methods[i];
		const String name = method.get("name", String());
		if (name.is_empty() || name.begins_with("_")) {
			// A leading underscore is Godot's own virtual, which a binding must not re-declare:
			// the mirror already spells the 1413 of them and an override belongs to the script.
			continue;
		}

		const int64_t flags = method.get("flags", 0);
		if ((flags & METHOD_FLAG_VIRTUAL) != 0) {
			continue;
		}

		VerseBindingMethod out;
		out.godot_name = utf8_of(name);
		out.is_const = (flags & METHOD_FLAG_CONST) != 0;

		const Dictionary ret = method.get("return", Dictionary());
		out.result_type = verse_type_for(ret.get("type", (int64_t)Variant::NIL), ret.get("class_name", String()));

		bool usable = true;
		const Array args = method.get("args", Array());
		for (int64_t a = 0; a < args.size(); a++) {
			const Dictionary arg = args[a];
			const std::string type = verse_type_for(arg.get("type", (int64_t)Variant::NIL), arg.get("class_name", String()));
			if (type.empty()) {
				usable = false;
				break;
			}
			out.params.push_back({ safe_param_name(arg.get("name", String()), a), type });
		}
		if (!usable) {
			continue;
		}
		r_class.methods.push_back(out);
	}

	const TypedArray<Dictionary> signals = p_db->class_get_signal_list(p_godot_name, true);
	for (int64_t i = 0; i < signals.size(); i++) {
		const Dictionary signal = signals[i];
		const String name = signal.get("name", String());
		if (name.is_empty()) {
			continue;
		}
		VerseBindingSignal out;
		out.godot_name = utf8_of(name);
		bool usable = true;
		const Array args = signal.get("args", Array());
		for (int64_t a = 0; a < args.size(); a++) {
			const Dictionary arg = args[a];
			const std::string type = verse_type_for(arg.get("type", (int64_t)Variant::NIL), arg.get("class_name", String()));
			if (type.empty()) {
				usable = false;
				break;
			}
			out.arg_types.push_back(type);
		}
		if (usable) {
			r_class.signals.push_back(out);
		}
	}
}

/// Reads a script class's own methods off the script resource.
///
/// `get_script_method_list` reports the base's members too (`gdscript.cpp:316` calls
/// `_get_script_method_list(r_list, true)` with no own-only flag), so the base's are subtracted --
/// re-declaring one is glitch 3532 at the declaration, with nothing said about where the collision
/// came from.
void describe_from_script(const Ref<Script> &p_script, VerseBindingClass &r_class) {
	std::set<std::string> inherited;
	Ref<Script> base = p_script->get_base_script();
	while (base.is_valid()) {
		const TypedArray<Dictionary> base_methods = base->get_script_method_list();
		for (int64_t i = 0; i < base_methods.size(); i++) {
			const Dictionary method = base_methods[i];
			inherited.insert(utf8_of(method.get("name", String())));
		}
		base = base->get_base_script();
	}

	const TypedArray<Dictionary> methods = p_script->get_script_method_list();
	for (int64_t i = 0; i < methods.size(); i++) {
		const Dictionary method = methods[i];
		const String name = method.get("name", String());
		if (name.is_empty() || name.begins_with("_") || inherited.count(utf8_of(name)) > 0) {
			continue;
		}

		VerseBindingMethod out;
		out.godot_name = utf8_of(name);
		// GDScript has no const methods at all, so every script binding is `<transacts>` and the
		// `<reads>` half of R-INT-9 never fires here.
		out.is_const = false;

		const Dictionary ret = method.get("return", Dictionary());
		out.result_type = verse_type_for(ret.get("type", (int64_t)Variant::NIL), ret.get("class_name", String()));

		bool usable = true;
		const Array args = method.get("args", Array());
		for (int64_t a = 0; a < args.size(); a++) {
			const Dictionary arg = args[a];
			const std::string type = verse_type_for(arg.get("type", (int64_t)Variant::NIL), arg.get("class_name", String()));
			if (type.empty()) {
				usable = false;
				break;
			}
			out.params.push_back({ safe_param_name(arg.get("name", String()), a), type });
		}
		if (usable) {
			r_class.methods.push_back(out);
		}
	}

	const TypedArray<Dictionary> signals = p_script->get_script_signal_list();
	for (int64_t i = 0; i < signals.size(); i++) {
		const Dictionary signal = signals[i];
		const String name = signal.get("name", String());
		if (name.is_empty()) {
			continue;
		}
		VerseBindingSignal out;
		out.godot_name = utf8_of(name);
		bool usable = true;
		const Array args = signal.get("args", Array());
		for (int64_t a = 0; a < args.size(); a++) {
			const Dictionary arg = args[a];
			const std::string type = verse_type_for(arg.get("type", (int64_t)Variant::NIL), arg.get("class_name", String()));
			if (type.empty()) {
				usable = false;
				break;
			}
			out.arg_types.push_back(type);
		}
		if (usable) {
			r_class.signals.push_back(out);
		}
	}
}

} // namespace

VerseBindings verse_generate_bindings() {
	VerseBindings bindings;

	ClassDBSingleton *db = ClassDBSingleton::get_singleton();
	if (db == nullptr) {
		return bindings;
	}

	// Two names must not collide, and Godot's own uniqueness is not enough on its own: the Verse
	// name is snake_cased, so `FooBar` and `Foo_Bar` would both be `foo_bar`. First one wins and
	// the second is dropped rather than emitted as a redefinition.
	std::set<std::string> taken;

	// --- ClassDB, minus everything the mirror carries -------------------------------------------
	const PackedStringArray class_list = db->get_class_list();
	for (int64_t i = 0; i < class_list.size(); i++) {
		const String godot_name = class_list[i];
		if (is_mirrored(godot_name)) {
			continue;
		}

		// **Only what a GDExtension registered**, which is what R-INT-7 is about and what the rest
		// of ClassDB is not. The editor's ClassDB carries every editor-only class -- `EditorPlugin`,
		// `AbstractPolygon2DEditor`, three hundred more -- and none of them exists in an exported
		// game, so binding them produced 1677 lines of Verse that the cooker then could not compile
		// against a runtime ClassDB. API_CORE minus the mirror is not a gap either: it is the
		// driver classes `GDCLASS` registers without the dump hearing of them (`IPWindows`,
		// `GodotNavigationServer2D`), which nothing a script writes should name.
		if (db->class_get_api_type(godot_name) != ClassDBSingleton::API_EXTENSION) {
			continue;
		}
		const std::string base = mirrored_base_of(db, godot_name);
		if (base.empty()) {
			// Nothing in its ancestry has a Verse spelling, so there is no class to derive from.
			continue;
		}
		VerseBindingClass binding;
		binding.godot_class = utf8_of(godot_name);
		binding.verse_class = verse_binding_class_name(binding.godot_class);
		binding.verse_base = base;
		if (binding.verse_class.empty() || !taken.insert(binding.verse_class).second) {
			continue;
		}
		describe_from_classdb(db, godot_name, binding);
		bindings.classes.push_back(binding);
	}

	// --- script classes with a `class_name` -----------------------------------------------------
	//
	// `get_global_class_list` is language-agnostic, so C# rides along with no new code -- and
	// OQ-17 still says no test in this repository has ever run C#.
	const TypedArray<Dictionary> globals = ProjectSettings::get_singleton()->get_global_class_list();
	for (int64_t i = 0; i < globals.size(); i++) {
		const Dictionary entry = globals[i];
		const String script_name = entry.get("class", String());
		const String path = entry.get("path", String());
		if (script_name.is_empty() || path.is_empty()) {
			continue;
		}
		// A Verse script registers its own Godot class name through `@global_class`, and it is
		// already a Verse class: binding it would declare it twice.
		if (path.get_extension().to_lower() == "verse") {
			continue;
		}

		// **The load can fail, and skipping the class when it does is a deadlock.** A GDScript that
		// names a Verse global class does not parse until the Verse project has built -- and the
		// build needs this package, because a Verse file may name the binding. Dropping the class
		// here made `test := class(main_script)` an unknown identifier, which failed the build,
		// which left the Verse class unregistered, which is why the load failed. Round it went.
		//
		// So the *type* is never lost, only its members: the global class list already says what the
		// script extends, which is all a declaration needs. Anything naming the binding compiles,
		// the build succeeds, the script becomes loadable, and the next generation fills the members
		// in -- which is the same bargain §6 asks for when a script simply does not compile.
		// Attempted every time, even on the first generation of a session where a GDScript naming a
		// Verse global class cannot parse yet and Godot prints `Error loading resource` for it. Not
		// attempting would be quieter and worse: a Verse file calling a binding's *method* would not
		// compile on the first build, because the first generation would have described nothing.
		// The message is accurate, appears once per generation, and stops after the first build.
		const Ref<Script> script = ResourceLoader::get_singleton()->load(path);

		const std::string base = script.is_valid()
				? mirrored_verse_class(script->get_instance_base_type())
				: mirrored_verse_class(entry.get("base", String()));
		if (base.empty()) {
			continue;
		}

		VerseBindingClass binding;
		binding.script_class = utf8_of(script_name);
		binding.script_path = utf8_of(path);
		binding.verse_class = verse_binding_class_name(binding.script_class);
		binding.verse_base = base;
		if (binding.verse_class.empty() || !taken.insert(binding.verse_class).second) {
			continue;
		}
		if (script.is_valid()) {
			describe_from_script(script, binding);
		} else {
			// Say so, once per generation, rather than leaving an author to wonder why completion
			// offers a class with one meaningless method on it.
			bindings.incomplete.push_back(binding.verse_class);
		}
		bindings.classes.push_back(binding);
	}

	bindings.source = verse_emit_bindings(bindings.classes);
	return bindings;
}
