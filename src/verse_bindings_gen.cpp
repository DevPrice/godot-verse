#include "verse_bindings_gen.h"

#include "verse_api_classes.h"

#include <godot_cpp/classes/class_db_singleton.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/script.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <algorithm>
#include <cmath>
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

/// Every class this generation will declare, Godot's name for it -> the Verse name it is given.
///
/// A class typed as another binding is what this exists for, and it has to be a whole-roster answer
/// rather than a running one: a method of the first class emitted can name the last, and nothing
/// orders a Godot class list. Verse does not mind -- module-scope definitions resolve in any order,
/// which the mirror relies on 12,000 lines before it declares `node2d` -- so the roster is collected
/// first and every member is typed against all of it.
using VerseBindingRoster = std::unordered_map<std::string, std::string>;

/// Every enum this generation declares, Godot's `Owner.Enum` spelling -> the Verse name it is
/// given. Keyed the way Godot's own metadata names one: an enum-typed argument reports the enum's
/// type in `class_name`, as `Thing.State`, with PROPERTY_USAGE_CLASS_IS_ENUM set (measured against
/// Godot 4.7, for a GDScript method and a ClassDB one alike).
using VerseEnumRoster = std::unordered_map<std::string, std::string>;

/// The Verse type a Godot `Variant::Type` plus class name crosses as, or empty when neither the
/// mirror nor this generation's own roster has a spelling for it.
///
/// Deliberately narrow. A member the generator cannot type is **left out** rather than guessed at:
/// a wrong type compiles and then fails at the call, where an absent one is the dynamic route the
/// author already had (R-INT-2). The set here is what a reader exists for in `verse_bindings.cpp`.
std::string verse_type_for(int64_t p_variant_type, const String &p_class_name, const VerseBindingRoster &p_roster) {
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
			// The mirror first, because a class in both is the mirror's: `Node2D` is `node2d`
			// wherever it appears, and a binding is only ever generated for what the mirror lacks.
			const std::string mirrored = mirrored_verse_class(p_class_name);
			if (!mirrored.empty()) {
				return mirrored;
			}
			const VerseBindingRoster::const_iterator bound = p_roster.find(utf8_of(p_class_name));
			return bound == p_roster.end() ? std::string() : bound->second;
		}
		default:
			return std::string();
	}
}

/// Whether a Verse type name is one of this generation's own enums.
bool is_bound_enum(const std::string &p_type, const VerseEnumRoster &p_enums) {
	for (const std::pair<const std::string, std::string> &row : p_enums) {
		if (row.second == p_type) {
			return true;
		}
	}
	return false;
}

/// The Verse type for one argument, result or property, enums included.
///
/// Godot spells an enum as an `int` whose `class_name` names it -- `Thing.State` -- and whose usage
/// carries PROPERTY_USAGE_CLASS_IS_ENUM, for a GDScript member and a ClassDB one alike (measured,
/// Godot 4.7). An enum this generation does not declare falls back to the int it really is, which
/// is what an author would have had to write anyway.
std::string member_type_for(const Dictionary &p_entry, const VerseBindingRoster &p_roster,
		const VerseEnumRoster &p_enums) {
	const int64_t type = p_entry.get("type", (int64_t)Variant::NIL);
	const int64_t usage = p_entry.get("usage", (int64_t)0);
	const String class_name = p_entry.get("class_name", String());
	if (type == Variant::INT && (usage & PROPERTY_USAGE_CLASS_IS_ENUM) != 0) {
		const VerseEnumRoster::const_iterator bound = p_enums.find(utf8_of(class_name));
		if (bound != p_enums.end()) {
			return bound->second;
		}
	}
	return verse_type_for(type, class_name, p_roster);
}

/// The type a parameter is *declared* as, which is not always the type its value crosses at.
///
/// An object argument is optional, because null is a value every Godot object slot can hold and
/// nothing a binding is described from says otherwise: GDScript has no nullability annotation, and
/// `ClassDB.class_get_method_list` carries none of the `required` metadata the mirror reads out of
/// the API dump (godotengine/godot#86079). So the mirror's rule -- optional unless Godot says
/// required -- reads here as "always", which is also how Godot treats the call.
///
/// A *result* is left alone: an object result is already `<decides>`, which says the same thing at
/// the other end of the call.
std::string declared_param_type(int64_t p_variant_type, const std::string &p_type) {
	return p_variant_type == Variant::OBJECT ? "?" + p_type : p_type;
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

/// The Verse spelling of every member the binding's *mirrored* ancestry already declares.
///
/// A member may not shadow an inherited one -- glitch 3532 at the declaration, with nothing said
/// about where the collision came from -- and the mirror declares 3312 properties and 503 signal
/// accessors a binding could collide with. `Mob extends RigidBody2D` declaring `var mass` is the
/// ordinary case rather than an exotic one, and the cost of getting it wrong is the whole package:
/// a bindings package that does not compile takes every binding in the project with it (B35).
std::set<std::string> inherited_member_names(ClassDBSingleton *p_db, const String &p_godot_base) {
	std::set<std::string> names;
	for (String cursor = p_godot_base; !cursor.is_empty(); cursor = p_db->get_parent_class(cursor)) {
		const std::string godot_name = utf8_of(cursor);
		for (const verse_api::method_mapping &member : verse_api::methods) {
			if (godot_name == member.godot_class) {
				names.insert(member.verse_method);
			}
		}
	}
	return names;
}

/// One class's enums as the generator wants them, read out of ClassDB.
void collect_classdb_enums(ClassDBSingleton *p_db, const String &p_godot_name, VerseBindingClass &r_class) {
	const PackedStringArray enums = p_db->class_get_enum_list(p_godot_name, true);
	for (int64_t i = 0; i < enums.size(); i++) {
		const String enum_name = enums[i];
		const PackedStringArray constants = p_db->class_get_enum_constants(p_godot_name, enum_name, true);
		if (constants.is_empty()) {
			continue;
		}

		std::vector<std::string> godot_names;
		for (int64_t c = 0; c < constants.size(); c++) {
			godot_names.push_back(utf8_of(constants[c]));
		}
		const std::vector<std::string> verse_names = verse_binding_enumerator_names(godot_names);

		VerseBindingEnum bound;
		bound.godot_name = utf8_of(enum_name);
		bound.verse_name = verse_binding_enum_name(utf8_of(p_godot_name), bound.godot_name);
		for (int64_t c = 0; c < constants.size(); c++) {
			bound.values.push_back({ verse_names[c],
					p_db->class_get_integer_constant(p_godot_name, constants[c]) });
		}
		r_class.enums.push_back(bound);
	}
}

/// The same, for a script class, whose enums arrive as Dictionary values in the constant map.
///
/// `{ &"State": { "IDLE": 0, "BUSY": 1 } }` is what a GDScript `enum State { IDLE, BUSY }` reports
/// (measured, Godot 4.7), so a Dictionary-valued constant is an enum and everything else is a
/// constant. There is no third kind: a `const` holding a real Dictionary is indistinguishable here
/// and would be bound as an enum of its keys, which is why only integer-valued entries are kept.
void collect_script_enums(const Ref<Script> &p_script, VerseBindingClass &r_class) {
	const Dictionary constants = p_script->get_script_constant_map();
	const Array names = constants.keys();
	for (int64_t i = 0; i < names.size(); i++) {
		const Variant value = constants[names[i]];
		if (value.get_type() != Variant::DICTIONARY) {
			continue;
		}
		const Dictionary entries = value;
		const Array keys = entries.keys();
		std::vector<std::string> godot_names;
		std::vector<int64_t> numbers;
		for (int64_t k = 0; k < keys.size(); k++) {
			const Variant number = entries[keys[k]];
			if (keys[k].get_type() != Variant::STRING || number.get_type() != Variant::INT) {
				godot_names.clear();
				break;
			}
			godot_names.push_back(utf8_of(keys[k]));
			numbers.push_back(number);
		}
		if (godot_names.empty()) {
			continue;
		}

		const std::vector<std::string> verse_names = verse_binding_enumerator_names(godot_names);
		VerseBindingEnum bound;
		bound.godot_name = utf8_of(names[i]);
		bound.verse_name = verse_binding_enum_name(r_class.script_class, bound.godot_name);
		for (size_t k = 0; k < godot_names.size(); k++) {
			bound.values.push_back({ verse_names[k], numbers[k] });
		}
		r_class.enums.push_back(bound);
	}
}

/// The Verse literal for a constant's value, or empty when it has none this can write.
std::string constant_literal(const Variant &p_value, std::string &r_type) {
	switch (p_value.get_type()) {
		case Variant::BOOL:
			r_type = "logic";
			return (bool)p_value ? "true" : "false";
		case Variant::INT:
			r_type = "int";
			return std::string(String::num_int64((int64_t)p_value).utf8().get_data());
		case Variant::FLOAT: {
			r_type = "float";
			const double number = p_value;
			// Verse has no literal for either, and a constant that cannot be written is left out
			// rather than approximated: `Vector2.INF` is the mirror's own example.
			if (std::isinf(number) || std::isnan(number)) {
				return std::string();
			}
			// `1` is an int literal and `1.0` is a float: a float constant has to carry its point.
			std::string text = utf8_of(String::num(number, 17));
			return text.find('.') == std::string::npos ? text + ".0" : text;
		}
		case Variant::STRING:
		case Variant::STRING_NAME: {
			r_type = "string";
			const std::string text = utf8_of(p_value);
			// A quote or a backslash would end the literal early, and an interpolation brace would
			// make Verse evaluate the rest. None of the three is worth escaping for a constant.
			return text.find_first_of("\"\\{}\n\r") == std::string::npos ? "\"" + text + "\"" : std::string();
		}
		default:
			return std::string();
	}
}

/// Whether a script's text names one of the project's Verse classes.
///
/// The test for "loading this could come back to where this generation is standing". Godot resolves
/// such a name by loading the `.verse`, that load builds the project, and the build generates these
/// bindings, so the script is already on this thread's load stack and asking for it again is cyclic.
///
/// **Text, because GDScript keeps no dependency list**: `ResourceFormatLoaderGDScript` forwards
/// `GDScriptParser::get_dependencies`, which returns an empty list under a `// TODO: Keep track of
/// deps.` (`gdscript_parser.h:1699-1702`). Over-inclusive on purpose -- a name in a comment holds
/// the script back for one generation, where a script wrongly *not* held back is the error line
/// this exists to remove.
bool names_a_verse_class(const String &p_path, const std::vector<String> &p_verse_classes) {
	if (p_verse_classes.empty()) {
		return false;
	}
	const String text = FileAccess::get_file_as_string(p_path);
	for (const String &verse_class : p_verse_classes) {
		if (text.find(verse_class) >= 0) {
			return true;
		}
	}
	return false;
}

/// The nearest base a script class has that is not itself a script class -- its engine class.
///
/// Follows the global class list, which records the declared base rather than the engine one, so
/// `class_name Enemy extends Actor` where `Actor` is also a script resolves in two steps. Bounded
/// because a project whose class list names a cycle is a project this must still return from.
std::string native_base_of(const std::unordered_map<std::string, std::string> &p_bases, const std::string &p_class) {
	std::string cursor = p_class;
	for (int guard = 0; guard < 64; guard++) {
		const std::unordered_map<std::string, std::string>::const_iterator found = p_bases.find(cursor);
		if (found == p_bases.end()) {
			return cursor;
		}
		if (found->second.empty()) {
			return std::string();
		}
		cursor = found->second;
	}
	return std::string();
}

/// What the last generation said about this script class, or null if it said nothing.
const VerseBindingClass *remembered_class(const std::vector<VerseBindingClass> *p_previous, const std::string &p_script_class) {
	if (p_previous == nullptr || p_script_class.empty()) {
		return nullptr;
	}
	for (const VerseBindingClass &was : *p_previous) {
		if (was.script_class == p_script_class) {
			return &was;
		}
	}
	return nullptr;
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

/// Reads one class's own methods, signals, properties and constants out of ClassDB.
void describe_from_classdb(ClassDBSingleton *p_db, const String &p_godot_name, const VerseBindingRoster &p_roster,
		const VerseEnumRoster &p_enums, const std::set<std::string> &p_inherited, VerseBindingClass &r_class) {
	// no_inheritance, or every binding re-declares its base's members and trips Verse's shadow
	// rule -- a member that shadows an inherited one is glitch 3532 at the declaration.
	const TypedArray<Dictionary> methods = p_db->class_get_method_list(p_godot_name, true);

	// The whole method list before any of it is classified: the `set_` twin that tells a property's
	// read half from a predicate is a *sibling*, so `is_point_disabled` needs to know whether this
	// class also declares `set_point_disabled`.
	std::set<std::string> sibling_names;
	for (int64_t i = 0; i < methods.size(); i++) {
		sibling_names.insert(utf8_of(Dictionary(methods[i]).get("name", String())));
	}

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
		out.is_static = (flags & METHOD_FLAG_STATIC) != 0;
		// A static is dispatched through `ClassDB.class_call_static`, which is not const whatever
		// the method it reaches is, so `<reads>` is not available to one.
		out.is_const = !out.is_static && (flags & METHOD_FLAG_CONST) != 0;

		const Dictionary ret = method.get("return", Dictionary());
		out.result_type = member_type_for(ret, p_roster, p_enums);
		out.is_predicate = out.result_type == "logic" &&
				verse_binding_is_predicate(out.godot_name, sibling_names);

		if (!out.is_static && p_inherited.count(verse_binding_member_name(out.godot_name)) > 0) {
			continue;
		}

		bool usable = true;
		const Array args = method.get("args", Array());
		for (int64_t a = 0; a < args.size(); a++) {
			const Dictionary arg = args[a];
			const std::string type = member_type_for(arg, p_roster, p_enums);
			if (type.empty()) {
				usable = false;
				break;
			}
			out.params.push_back({ safe_param_name(arg.get("name", String()), a),
					declared_param_type(arg.get("type", (int64_t)Variant::NIL), type) });
		}
		if (!usable) {
			continue;
		}
		(out.is_static ? r_class.statics : r_class.methods).push_back(out);
	}

	// **No properties here, and none are missing.** A ClassDB property is *defined* by a getter and
	// a setter method -- `ADD_PROPERTY` names both -- and those are in the method list above, so a
	// binding already answers `GetProcessCallback()` and `SetProcessCallback()`. Only a GDScript
	// `var`, which has no such pair, needs one invented (see describe_from_script).

	// The enum constants are already carried by the enums themselves, and a Verse enumerator and a
	// constant of the same name in one module is a redefinition.
	const PackedStringArray constants = p_db->class_get_integer_constant_list(p_godot_name, true);
	for (int64_t i = 0; i < constants.size(); i++) {
		const String name = constants[i];
		if (!String(p_db->class_get_integer_constant_enum(p_godot_name, name, true)).is_empty()) {
			continue;
		}
		r_class.constants.push_back({ verse_binding_constant_name(utf8_of(name)), "int",
				utf8_of(String::num_int64(p_db->class_get_integer_constant(p_godot_name, name))) });
	}

	const TypedArray<Dictionary> signals = p_db->class_get_signal_list(p_godot_name, true);
	for (int64_t i = 0; i < signals.size(); i++) {
		const Dictionary signal = signals[i];
		const String name = signal.get("name", String());
		if (name.is_empty() || p_inherited.count(verse_binding_member_name(utf8_of(name))) > 0) {
			continue;
		}
		VerseBindingSignal out;
		out.godot_name = utf8_of(name);
		bool usable = true;
		const Array args = signal.get("args", Array());
		for (int64_t a = 0; a < args.size(); a++) {
			const Dictionary arg = args[a];
			const std::string type = member_type_for(arg, p_roster, p_enums);
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
void describe_from_script(const Ref<Script> &p_script, const VerseBindingRoster &p_roster,
		const VerseEnumRoster &p_enums, const std::set<std::string> &p_inherited, VerseBindingClass &r_class) {
	std::set<std::string> inherited;
	Ref<Script> base = p_script->get_base_script();
	while (base.is_valid()) {
		const TypedArray<Dictionary> base_methods = base->get_script_method_list();
		for (int64_t i = 0; i < base_methods.size(); i++) {
			const Dictionary method = base_methods[i];
			inherited.insert(utf8_of(method.get("name", String())));
		}
		const TypedArray<Dictionary> base_properties = base->get_script_property_list();
		for (int64_t i = 0; i < base_properties.size(); i++) {
			const Dictionary property = base_properties[i];
			inherited.insert(utf8_of(property.get("name", String())));
		}
		base = base->get_base_script();
	}

	const TypedArray<Dictionary> methods = p_script->get_script_method_list();
	for (int64_t i = 0; i < methods.size(); i++) {
		const Dictionary method = methods[i];
		const String name = method.get("name", String());
		if (name.is_empty() || name.begins_with("_") || inherited.count(utf8_of(name)) > 0 ||
				p_inherited.count(verse_binding_member_name(utf8_of(name))) > 0) {
			continue;
		}

		VerseBindingMethod out;
		out.godot_name = utf8_of(name);
		// GDScript has no const methods at all, so every script binding is `<transacts>` and the
		// `<reads>` half of R-INT-9 never fires here.
		out.is_const = false;
		out.is_static = (int64_t(method.get("flags", 0)) & METHOD_FLAG_STATIC) != 0;
		// **And no script method is a predicate.** The mirror's rule reads a test out of Godot's
		// own naming, which Godot chose for its own C++ API; a GDScript author writing
		// `func is_alive() -> bool` has made no such claim, and turning two letters of their method
		// name into `<decides>` would change how every caller spells it. Their `bool` is a `logic`.
		out.is_predicate = false;

		const Dictionary ret = method.get("return", Dictionary());
		out.result_type = member_type_for(ret, p_roster, p_enums);

		bool usable = true;
		const Array args = method.get("args", Array());
		for (int64_t a = 0; a < args.size(); a++) {
			const Dictionary arg = args[a];
			const std::string type = member_type_for(arg, p_roster, p_enums);
			if (type.empty()) {
				usable = false;
				break;
			}
			out.params.push_back({ safe_param_name(arg.get("name", String()), a),
					declared_param_type(arg.get("type", (int64_t)Variant::NIL), type) });
		}
		if (usable) {
			(out.is_static ? r_class.statics : r_class.methods).push_back(out);
		}
	}

	// PROPERTY_USAGE_SCRIPT_VARIABLE is the filter: the list opens with a category row naming the
	// file, which is there only under TOOLS_ENABLED and is not a property at all.
	const TypedArray<Dictionary> properties = p_script->get_script_property_list();
	for (int64_t i = 0; i < properties.size(); i++) {
		const Dictionary property = properties[i];
		const String name = property.get("name", String());
		const int64_t usage = property.get("usage", (int64_t)0);
		if (name.is_empty() || (usage & PROPERTY_USAGE_SCRIPT_VARIABLE) == 0 ||
				inherited.count(utf8_of(name)) > 0) {
			continue;
		}
		const std::string type = member_type_for(property, p_roster, p_enums);
		if (type.empty() || (!is_bound_enum(type, p_enums) && !verse_binding_can_be_property(type))) {
			continue;
		}
		// The pair's own names are what may collide, not the property's: `var speed` becomes
		// `GetSpeed` and `SetSpeed`, and either may already be an inherited mirrored member or a
		// method this script declares itself. A collision is glitch 3532 at the generated
		// declaration, which costs the whole package, so the property is dropped instead.
		const std::string accessor = verse_binding_member_name(utf8_of(name));
		bool clear = true;
		for (const char *const half : { "Get", "Set" }) {
			const std::string spelled = half + accessor;
			clear = clear && p_inherited.count(spelled) == 0;
			for (const VerseBindingMethod &method : r_class.methods) {
				clear = clear && verse_binding_member_name(method.godot_name) != spelled;
			}
		}
		if (!clear) {
			continue;
		}
		// Every GDScript `var` is written as well as read -- there is no read-only `var`, and a
		// custom setter is still a setter -- so there is nothing else to ask about one.
		r_class.properties.push_back({ utf8_of(name), type });
	}

	const Dictionary constants = p_script->get_script_constant_map();
	const Array constant_names = constants.keys();
	for (int64_t i = 0; i < constant_names.size(); i++) {
		const Variant value = constants[constant_names[i]];
		// A Dictionary-valued constant is an enum, which `collect_script_enums` has already taken.
		if (value.get_type() == Variant::DICTIONARY) {
			continue;
		}
		std::string type;
		const std::string literal = constant_literal(value, type);
		if (!literal.empty()) {
			r_class.constants.push_back({ verse_binding_constant_name(utf8_of(constant_names[i])), type, literal });
		}
	}

	const TypedArray<Dictionary> signals = p_script->get_script_signal_list();
	for (int64_t i = 0; i < signals.size(); i++) {
		const Dictionary signal = signals[i];
		const String name = signal.get("name", String());
		if (name.is_empty() || p_inherited.count(verse_binding_member_name(utf8_of(name))) > 0) {
			continue;
		}
		VerseBindingSignal out;
		out.godot_name = utf8_of(name);
		bool usable = true;
		const Array args = signal.get("args", Array());
		for (int64_t a = 0; a < args.size(); a++) {
			const Dictionary arg = args[a];
			const std::string type = member_type_for(arg, p_roster, p_enums);
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

VerseBindings verse_generate_bindings(bool p_inside_resource_load, const std::vector<VerseBindingClass> *p_previous) {
	VerseBindings bindings;

	ClassDBSingleton *db = ClassDBSingleton::get_singleton();
	if (db == nullptr) {
		return bindings;
	}

	// Two names must not collide, and Godot's own uniqueness is not enough on its own: the Verse
	// name is snake_cased, so `FooBar` and `Foo_Bar` would both be `foo_bar`. First one wins and
	// the second is dropped rather than emitted as a redefinition.
	std::set<std::string> taken;

	// Declared first, described second. A method may name any class in the generation and a Godot
	// class list is in no order this could rely on, so nothing is typed until every name is known.
	// The two vectors are appended together and read by index.
	VerseBindingRoster roster;
	struct FPending {
		String godot_class;
		Ref<Script> script;
	};
	std::vector<FPending> pending;

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
		// Enums with the roster rather than with the members, for the roster's own reason: a method
		// of the first class emitted may take the last class's enum.
		collect_classdb_enums(db, godot_name, binding);
		roster[binding.godot_class] = binding.verse_class;
		pending.push_back({ godot_name, Ref<Script>() });
		bindings.classes.push_back(binding);
	}

	// --- script classes with a `class_name` -----------------------------------------------------
	//
	// `get_global_class_list` is language-agnostic, so C# rides along with no new code -- and
	// OQ-17 still says no test in this repository has ever run C#.
	const TypedArray<Dictionary> globals = ProjectSettings::get_singleton()->get_global_class_list();

	// What each script class extends, so a base can be resolved without loading anything. The list
	// records the *declared* base, which for `class_name Foo extends Bar` is another script class,
	// so this is walked rather than read once. The Verse class names beside it are what a script has
	// to mention for its load to reach back here.
	std::unordered_map<std::string, std::string> declared_bases;
	std::vector<String> verse_classes;
	for (int64_t i = 0; i < globals.size(); i++) {
		const Dictionary entry = globals[i];
		const String script_name = entry.get("class", String());
		if (script_name.is_empty()) {
			continue;
		}
		declared_bases[utf8_of(script_name)] = utf8_of(entry.get("base", String()));
		if (String(entry.get("path", String())).get_extension().to_lower() == "verse") {
			verse_classes.push_back(script_name);
		}
	}

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

		// **The load can fail, and skipping the class when it does is a deadlock.** Dropping the
		// class made `test := class(main_script)` an unknown identifier, which failed the build,
		// which left the Verse class unregistered, which is why the load failed. Round it went.
		//
		// So the *type* is never lost, only its members: the class list already says what the script
		// extends, which is all a declaration needs. Anything naming the binding compiles, the build
		// succeeds, the script becomes loadable, and the next generation fills the members in --
		// which is the same bargain §6 asks for when a script simply does not compile.
		Ref<Script> script;
		if (!p_inside_resource_load || !names_a_verse_class(path, verse_classes)) {
			script = ResourceLoader::get_singleton()->load(path);
		}

		// The script's own answer first, because a script may extend one this list does not carry.
		const std::string base = script.is_valid()
				? mirrored_verse_class(script->get_instance_base_type())
				: mirrored_verse_class(String(native_base_of(declared_bases, utf8_of(script_name)).c_str()));
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
		if (!script.is_valid()) {
			// The members the last generation described, because a generation taken during a load
			// otherwise *unsays* what a complete one had already said -- and a script naming a
			// Verse class is held back on every load, not once (B36). Stale at worst; the class is
			// still reported incomplete, so the corrective build replaces this with a read one.
			if (const VerseBindingClass *remembered = remembered_class(p_previous, binding.script_class)) {
				binding.methods = remembered->methods;
				binding.signals = remembered->signals;
				binding.properties = remembered->properties;
				binding.constants = remembered->constants;
				binding.statics = remembered->statics;
				binding.enums = remembered->enums;
			}
			// Say so, once per generation, rather than leaving an author to wonder why completion
			// offers a class with one meaningless method on it.
			bindings.incomplete.push_back(binding.verse_class);
		} else {
			collect_script_enums(script, binding);
		}
		roster[binding.script_class] = binding.verse_class;
		pending.push_back({ String(), script });
		bindings.classes.push_back(binding);
	}

	// Every enum every bound class declares, keyed as Godot's metadata names one.
	VerseEnumRoster enum_roster;
	for (const VerseBindingClass &binding : bindings.classes) {
		const std::string owner = binding.godot_class.empty() ? binding.script_class : binding.godot_class;
		for (const VerseBindingEnum &bound : binding.enums) {
			enum_roster[owner + "." + bound.godot_name] = bound.verse_name;
		}
	}

	for (size_t i = 0; i < pending.size(); i++) {
		VerseBindingClass &binding = bindings.classes[i];
		if (pending[i].script.is_valid()) {
			describe_from_script(pending[i].script, roster, enum_roster,
					inherited_member_names(db, String(pending[i].script->get_instance_base_type())), binding);
		} else if (!pending[i].godot_class.is_empty()) {
			describe_from_classdb(db, pending[i].godot_class, roster, enum_roster,
					inherited_member_names(db, db->get_parent_class(pending[i].godot_class)), binding);
		}

		// The module is named after Godot's own spelling of the class, which is what the mirror
		// does -- `NodeStatics` -- and the suffix is load-bearing rather than decoration: a module
		// called `Mob` would make any local of that name ambiguous.
		if (!binding.constants.empty() || !binding.statics.empty()) {
			binding.statics_module =
					(binding.godot_class.empty() ? binding.script_class : binding.godot_class) + "Statics";
		}
	}

	bindings.source = verse_emit_bindings(bindings.classes);
	return bindings;
}
