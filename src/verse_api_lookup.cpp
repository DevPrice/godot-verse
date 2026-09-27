#include "verse_api_lookup.h"

#include "verse_class_decl.h"
#include "verse_host_abi.h"

#include <godot_cpp/classes/script_language_extension.hpp>
#include <godot_cpp/core/class_db.hpp>

#include <iterator>

using namespace godot;

const char *verse_godot_class_for(const String &p_verse_class) {
	for (size_t i = 0; i < std::size(verse_api::classes); i++) {
		if (p_verse_class == verse_api::classes[i].verse_name) {
			return verse_api::classes[i].godot_name;
		}
	}
	return nullptr;
}

StringName verse_godot_class_name(const String &p_verse_class, int32_t p_class_kind) {
	if (p_verse_class.is_empty()) {
		return StringName();
	}
	if (p_class_kind == VH_CLASS_SCRIPT) {
		// **The leaf of the qualified name, not the whole of it.** Everything the host is asked about
		// is module-qualified -- `left/palette` for a class under a `.vmodule` -- and ClassDB is one
		// flat namespace a module is deliberately not part of: `@global_class` registers the file
		// stem PascalCased and nothing else. Passing the module through gave `Left/palette`, a name
		// nothing had ever registered, and the inspector answered *"Cannot get class"* the moment a
		// slot of that type was drawn. Found by hand; `by-hand-findings.md` B18.
		const String leaf = p_verse_class.substr(p_verse_class.rfind("/") + 1);
		return StringName(String(verse_pascal_case(std::string(leaf.utf8().get_data())).c_str()));
	}
	const char *godot_class = verse_godot_class_for(p_verse_class);
	return godot_class != nullptr ? StringName(godot_class) : StringName();
}

const char *godot_classdb_class_for(const String &p_verse_class) {
	const char *godot_name = verse_godot_class_for(p_verse_class);
	return godot_name != nullptr && ClassDB::class_exists(godot_name) ? godot_name : nullptr;
}

const char *godot_doc_class_for(const String &p_verse_class) {
	if (p_verse_class == String("vh_object")) {
		return "Object";
	}
	for (size_t i = 0; i < std::size(verse_api::types); i++) {
		if (p_verse_class == verse_api::types[i].verse_name) {
			return verse_api::types[i].godot_class[0] == '\0' ? nullptr : verse_api::types[i].godot_class;
		}
	}
	return verse_godot_class_for(p_verse_class);
}

const verse_api::method_mapping *godot_method_for(const String &p_verse_class, const String &p_verse_method) {
	for (size_t i = 0; i < std::size(verse_api::methods); i++) {
		if (p_verse_method == verse_api::methods[i].verse_method && p_verse_class == verse_api::methods[i].verse_class) {
			return &verse_api::methods[i];
		}
	}
	return nullptr;
}

const verse_api::global_mapping *godot_global_for(const String &p_verse_name) {
	for (size_t i = 0; i < std::size(verse_api::globals); i++) {
		if (p_verse_name == verse_api::globals[i].verse_name) {
			return &verse_api::globals[i];
		}
	}
	return nullptr;
}

String verse_extension_method_name(const String &p_name) {
	const String prefix = "operator'.";
	if (!p_name.begins_with(prefix) || !p_name.ends_with("'") || p_name.length() <= prefix.length() + 1) {
		return String();
	}
	return p_name.substr(prefix.length(), p_name.length() - prefix.length() - 1);
}

String verse_receiver_type(const String &p_function_type) {
	const String prefix = "type{_(:";
	if (!p_function_type.begins_with(prefix)) {
		return String();
	}
	int64_t depth = 0;
	for (int64_t i = prefix.length(); i < p_function_type.length(); i++) {
		const char32_t c = p_function_type[i];
		if (c == U'(' || c == U'[' || c == U'{') {
			depth++;
		} else if (c == U')' || c == U']' || c == U'}') {
			if (depth == 0) {
				return p_function_type.substr(prefix.length(), i - prefix.length());
			}
			depth--;
		} else if (c == U',' && depth == 0) {
			return p_function_type.substr(prefix.length(), i - prefix.length());
		}
	}
	return String();
}

String godot_singleton_class_for(const String &p_verse_name) {
	if (!p_verse_name.begins_with("Get")) {
		return String();
	}
	String godot_class = p_verse_name.substr(3);
	// `GetInputSingleton`, not `GetInput`: gen_verse_api.py moves the accessor out of the way when
	// a mirrored class already carries a member of the plain name (singleton_accessor_name), and
	// a module-level function ambiguous with a class member is a compile error rather than a
	// preference. So the suffix has to come off before the name can be inverted -- and it is tried
	// second, because a Godot class could in principle end in "Singleton".
	for (int attempt = 0; attempt < 2; attempt++) {
		for (size_t i = 0; i < std::size(verse_api::classes); i++) {
			if (godot_class == verse_api::classes[i].godot_name) {
				return godot_class;
			}
		}
		if (!godot_class.ends_with("Singleton")) {
			break;
		}
		godot_class = godot_class.substr(0, godot_class.length() - 9);
	}
	return String();
}

int64_t lookup_result_for(verse_api::member_kind p_kind) {
	switch (p_kind) {
		case verse_api::member_kind::signal:
			return (int64_t)ScriptLanguageExtension::LOOKUP_RESULT_CLASS_SIGNAL;
		case verse_api::member_kind::constant:
			return (int64_t)ScriptLanguageExtension::LOOKUP_RESULT_CLASS_CONSTANT;
		case verse_api::member_kind::property:
			return (int64_t)ScriptLanguageExtension::LOOKUP_RESULT_CLASS_PROPERTY;
		case verse_api::member_kind::method:
			break;
	}
	return (int64_t)ScriptLanguageExtension::LOOKUP_RESULT_CLASS_METHOD;
}

const verse_api::enum_mapping *godot_enum_for(const String &p_verse_enum) {
	for (size_t i = 0; i < std::size(verse_api::enums); i++) {
		if (p_verse_enum == verse_api::enums[i].verse_enum) {
			return &verse_api::enums[i];
		}
	}
	return nullptr;
}

String godot_statics_class_for(const String &p_verse_name) {
	if (p_verse_name == String("GodotStatics")) {
		return String("@GlobalScope");
	}
	if (!p_verse_name.ends_with("Statics")) {
		return String();
	}
	const String godot_class = p_verse_name.substr(0, p_verse_name.length() - 7);
	for (size_t i = 0; i < std::size(verse_api::classes); i++) {
		if (godot_class == verse_api::classes[i].godot_name) {
			return godot_class;
		}
	}
	return String();
}

const char *godot_doc_class_for_primitive(const String &p_verse_type) {
	if (p_verse_type == String("int")) {
		return "int";
	}
	if (p_verse_type == String("float")) {
		return "float";
	}
	if (p_verse_type == String("logic")) {
		return "bool";
	}
	if (p_verse_type == String("string") || p_verse_type == String("char")) {
		return "String";
	}
	return nullptr;
}

bool is_godot_package_global(const String &p_owner, const String &p_path) {
	return p_owner == p_path && is_godot_package_file(p_path);
}

bool is_godot_package_file(const String &p_path) {
	const String file = p_path.get_file();
	// GodotMath.native.verse is the fourth and was missing until the math methods were looked for
	// in it: it is where every extension method on a value type and every scalar `LerpAngle` is
	// written, so leaving it out excluded the largest group of globals the package has from ever
	// being documented as one.
	return file == String("Godot.native.verse")
			|| file == String("GodotApi.native.verse")
			|| file == String("GodotClasses.native.verse")
			|| file == String("GodotMath.native.verse");
}

const char *mirrored_class(const String &p_godot_class) {
	for (size_t i = 0; i < std::size(verse_api::classes); i++) {
		if (p_godot_class == verse_api::classes[i].godot_name) {
			return verse_api::classes[i].verse_name;
		}
	}
	return nullptr;
}

String godot_class_for(const std::string &p_verse_class) {
	const char *godot_name = verse_godot_class_for(String(p_verse_class.c_str()));
	return godot_name != nullptr ? String(godot_name) : String();
}

String verse_base_class_for(const String &p_godot_class) {
	for (String name = p_godot_class; !name.is_empty(); name = ClassDB::get_parent_class(name)) {
		if (const char *mirrored = mirrored_class(name)) {
			return String(mirrored);
		}
	}
	return String("node");
}
