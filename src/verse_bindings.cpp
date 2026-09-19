#include "verse_bindings.h"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace {

/// `gen_verse_api.py`'s `split_pascal`, which is not the obvious split: a run of capitals stays one
/// token unless a lowercase follows the last of them, so `RID` is one token and `RIDs` is `RI`+`Ds`.
/// That rule is what makes `VSlider` -> `v_slider` and `AABB` -> `aabb`.
std::vector<std::string> split_pascal(const std::string &p_name) {
	std::vector<std::string> tokens;
	if (p_name.empty()) {
		return tokens;
	}
	std::string cur(1, p_name[0]);
	for (size_t i = 1; i < p_name.size(); i++) {
		const unsigned char prev = (unsigned char)p_name[i - 1];
		const unsigned char c = (unsigned char)p_name[i];
		const bool split = (std::islower(prev) && std::isupper(c)) ||
				(std::isupper(prev) && std::isupper(c) && i + 1 < p_name.size() && std::islower((unsigned char)p_name[i + 1]));
		if (split) {
			tokens.push_back(cur);
			cur = std::string(1, p_name[i]);
		} else {
			cur += p_name[i];
		}
	}
	tokens.push_back(cur);
	return tokens;
}

std::string lower(const std::string &p_text) {
	std::string out;
	out.reserve(p_text.size());
	for (const char c : p_text) {
		out += (char)std::tolower((unsigned char)c);
	}
	return out;
}

/// `thing_state` -> `ThingState`, which is `gen_verse_api.py`'s `enum_converter_stem`: the name the
/// variant-to-enum converter is spelled from.
std::string enum_stem(const std::string &p_verse_enum) {
	std::string out;
	bool at_start = true;
	for (const char c : p_verse_enum) {
		if (c == '_') {
			at_start = true;
			continue;
		}
		out += at_start ? (char)std::toupper((unsigned char)c) : c;
		at_start = false;
	}
	return out;
}

/// The total builder that packs a value of a declared type into a `variant`.
///
/// **`MakeVariant` is not usable here and the refusal is the reason these exist.** It takes `any`
/// and is `<decides>` -- a tuple, a map or an empty array says nothing about its lane -- so every
/// generated body that used it needed a failure context it has no reason to have. The named
/// builders are total, and the generator knows the declared type, so there is nothing to discover.
const char *builder_for(const std::string &p_type) {
	if (p_type == "int") {
		return "VariantInt";
	}
	if (p_type == "float") {
		return "VariantFloat";
	}
	if (p_type == "logic") {
		return "VariantBool";
	}
	if (p_type == "string") {
		return "VariantString";
	}
	if (p_type == "vector2") {
		return "VariantVector2";
	}
	if (p_type == "vector3") {
		return "VariantVector3";
	}
	if (p_type == "color") {
		return "VariantColor";
	}
	if (p_type == "godot_array") {
		return "VariantArray";
	}
	if (p_type == "dictionary") {
		return "VariantDictionary";
	}
	// Every other parameter type this generator emits is a class, and an object packs through the
	// one builder that takes the whole hierarchy. The optional spelling is every object *argument*
	// of a binding, because nothing Godot reports about one says whether it accepts null and Godot
	// itself accepts it: GDScript has no such annotation, and `class_get_method_list` carries none
	// of the `required` metadata the mirror reads out of the API dump.
	return p_type.rfind("?", 0) == 0 ? "VariantMaybeObject" : "VariantObject";
}

/// The signature Verse spells for a parameter list.
std::string param_list(const VerseBindingMethod &p_method) {
	std::string out;
	for (size_t i = 0; i < p_method.params.size(); i++) {
		if (i > 0) {
			out += ", ";
		}
		out += p_method.params[i].first;
		out += ":";
		out += p_method.params[i].second;
	}
	return out;
}

/// The expression that packs one declared value into the `variant` a call takes.
///
/// An enum has no lane of its own: Godot carries every one as an int, and `ToInt` is the public
/// name the mirror gives that conversion, so a bound enum crosses the same way a mirrored one does.
std::string pack_expression(const std::string &p_type, const std::string &p_value, const VerseBindingEnumNames &p_enums) {
	if (p_enums.count(p_type) > 0) {
		return "VariantInt(ToInt(" + p_value + "))";
	}
	return std::string(builder_for(p_type)) + "(" + p_value + ")";
}

/// The call that reaches Godot, and the only place the two verbs are chosen between.
///
/// `CallConst` where Godot says the method is const, which is R-INT-9's classification and the
/// whole reason that verb exists: `Call` is `<transacts>`, so a binding built on it would start
/// wall 8's cascade in every caller that only reads.
std::string call_expression(const VerseBindingMethod &p_method, const VerseBindingEnumNames &p_enums) {
	const bool reads = p_method.is_const && (!p_method.result_type.empty() || p_method.is_predicate);
	std::string out = reads ? "CallConst(\"" : "Call(\"";
	out += p_method.godot_name;
	out += "\"";
	for (const auto &param : p_method.params) {
		out += ", ";
		out += pack_expression(param.second, param.first, p_enums);
	}
	out += ")";
	return out;
}

/// The same call, without an object to make it on: Godot's own by-name static dispatch.
///
/// Two spellings, because the two kinds of bound class are dispatched differently and neither can
/// serve the other. A ClassDB class has `ClassDB.class_call_static`, which the mirror already
/// carries. A script class has no ClassDB entry at all -- its statics live on the *script
/// resource*, and calling one means loading that resource and calling through it, which is what
/// `Thing.make()` does underneath in GDScript (measured headless, Godot 4.7).
std::string static_call_expression(const VerseBindingMethod &p_method, const VerseBindingClass &p_class,
		const VerseBindingEnumNames &p_enums) {
	std::string args;
	for (const auto &param : p_method.params) {
		args += ", ";
		args += pack_expression(param.second, param.first, p_enums);
	}

	if (!p_class.godot_class.empty()) {
		// Past four arguments the mirror's loose arities run out and the array spelling is the one
		// left, which takes the same values in one `array{}`.
		if (p_method.params.size() > 4) {
			std::string packed;
			for (size_t i = 0; i < p_method.params.size(); i++) {
				packed += i > 0 ? ", " : "";
				packed += pack_expression(p_method.params[i].second, p_method.params[i].first, p_enums);
			}
			return "GetClassDB().ClassCallStatic(\"" + p_class.godot_class + "\", \"" +
					p_method.godot_name + "\", array{" + packed + "})";
		}
		return "GetClassDB().ClassCallStatic(\"" + p_class.godot_class + "\", \"" +
				p_method.godot_name + "\"" + args + ")";
	}
	return "StaticScript.Call(\"" + p_method.godot_name + "\"" + args + ")";
}

/// The reader that turns the answering `variant` back into the declared type, and the fallback for
/// when it declines. A binding never raises: Godot answering something unexpected is not the
/// author's mistake and is not worth an instance's content scope.
///
/// **A class type has no row here and cannot have one**, which is why an object result is emitted
/// `<decides>` instead: there is no value of a class that stands for "Godot answered nothing", and
/// the nearest thing to one -- `some_class{}` -- would mint a live Godot object per declined call.
/// That is the mirror's rule too (R-TYPE-4): every object-returning method in it is failable.
struct FReader {
	const char *accessor;
	const char *fallback;
};

FReader reader_for(const std::string &p_type) {
	if (p_type == "int") {
		return { "AsInt", "0" };
	}
	if (p_type == "float") {
		return { "AsFloat", "0.0" };
	}
	if (p_type == "logic") {
		return { "AsBool", "false" };
	}
	if (p_type == "string") {
		return { "AsString", "\"\"" };
	}
	if (p_type == "vector2") {
		return { "AsVector2", "vector2{}" };
	}
	if (p_type == "vector3") {
		return { "AsVector3", "vector3{}" };
	}
	if (p_type == "color") {
		return { "AsColor", "color{}" };
	}
	if (p_type == "godot_array") {
		return { "AsArray", "MakeArray()" };
	}
	if (p_type == "dictionary") {
		return { "AsDictionary", "MakeDictionary()" };
	}
	return { nullptr, nullptr };
}

/// One method's declaration and body, at `p_indent`, over a call the caller has already spelled.
///
/// Shared by a class's own methods and by its `...Statics` module, which differ in how the call is
/// reached and in nothing else: the five shapes a result takes -- a predicate's status, an enum, a
/// value with a reader, an object, and nothing -- are the same on both sides.
///
/// `p_guard` is a failable clause the body must pass before the call, and it is what a *script*
/// class's static needs: its dispatch is through the script resource, so the resource has to be
/// loaded first and the load can decline. Empty for everything else. In a `<decides>` body it is a
/// line of its own, because the whole body is already a failure context; in a total one it joins
/// the `if` that was there anyway.
std::string emit_method(const VerseBindingMethod &p_method, const std::string &p_call,
		const std::string &p_guard, const VerseBindingEnumNames &p_enums, const std::string &p_indent) {
	const bool is_enum = p_enums.count(p_method.result_type) > 0;
	const FReader reader = is_enum ? FReader{ nullptr, nullptr } : reader_for(p_method.result_type);
	const bool answers_object = !p_method.result_type.empty() && !is_enum && reader.accessor == nullptr;
	const bool decides = answers_object || p_method.is_predicate;
	const bool reads = p_method.is_const && (!p_method.result_type.empty() || p_method.is_predicate);
	const std::string body_indent = p_indent + "\t";

	std::string out = p_indent;
	out += verse_binding_member_name(p_method.godot_name);
	out += "<public>(";
	out += param_list(p_method);
	out += ")";
	if (decides) {
		out += "<decides>";
	}
	// A method answering nothing is `<transacts>` whatever Godot's const flag says: the 38
	// const-and-void methods in Godot's own API are `OS.set_environment` and friends, which
	// plainly do something. The test is const *and answering*, the mirror's exactly -- and a
	// predicate answers, since its whole value is the status.
	out += reads ? "<reads>" : "<transacts>";
	out += ":";
	// A predicate's result *is* its status: `<decides>:void` writes no value in either direction,
	// which is why `call_func` writes the bool itself for a script's own virtuals.
	out += p_method.is_predicate || p_method.result_type.empty() ? "void" : p_method.result_type;
	out += " =\n";

	if (decides) {
		if (!p_guard.empty()) {
			out += body_indent + p_guard + "\n";
		}
		out += body_indent;
		if (p_method.is_predicate) {
			// Declining is "no", which is the honest answer when Godot hands back something that is
			// not a bool: a binding never raises, and there is no third status to report it with.
			out += p_call + ".AsBool[]?\n";
		} else {
			// `AsObject[]` reads the handle out of the answering `variant` and the downcast narrows
			// it to the class Godot annotated. Both decline rather than answering, which is the
			// whole of how "Godot answered null" crosses -- a class has no value that means nothing.
			out += p_method.result_type + "[" + p_call + ".AsObject[]]\n";
		}
		return out;
	}

	if (p_method.result_type.empty()) {
		if (!p_guard.empty()) {
			out += body_indent + "if (" + p_guard + "):\n";
			out += body_indent + "\t" + p_call + "\n";
			return out;
		}
		// A void method still has to swallow the `variant` the call answers, which is what the
		// braces are for: a block whose last expression is a call is void.
		out += body_indent + "{ " + p_call + "; }\n";
		return out;
	}

	if (is_enum) {
		// Total, because a method that answers a *value* must not claim it can fail: a number no
		// enumerator has is Godot disagreeing with its own metadata, and the converter answers the
		// first enumerator for it exactly as the mirror's does.
		const std::string read = "VhTo" + enum_stem(p_method.result_type) + "(" + p_call + ")";
		out += body_indent;
		out += p_guard.empty() ? read
							   : "if (" + p_guard + ") then " + read + " else " + p_method.result_type +
						"." + p_enums.at(p_method.result_type);
		out += "\n";
		return out;
	}

	out += body_indent + "if (";
	if (!p_guard.empty()) {
		out += p_guard + ", ";
	}
	out += "Read := " + p_call + "." + reader.accessor + "[]) then Read else " + reader.fallback + "\n";
	return out;
}

} // namespace

std::string verse_binding_class_name(const std::string &p_godot_name) {
	const std::vector<std::string> tokens = split_pascal(p_godot_name);
	std::string out;
	for (size_t i = 0; i < tokens.size(); i++) {
		if (i > 0) {
			out += "_";
		}
		out += lower(tokens[i]);
	}
	return out;
}

std::string verse_binding_member_name(const std::string &p_godot_name) {
	std::string out;
	bool at_start = true;
	for (const char c : p_godot_name) {
		if (c == '_') {
			at_start = true;
			continue;
		}
		out += at_start ? (char)std::toupper((unsigned char)c) : c;
		at_start = false;
	}
	return out;
}

std::string verse_binding_constant_name(const std::string &p_godot_name) {
	std::string out;
	bool at_start = true;
	for (const char c : p_godot_name) {
		if (c == '_') {
			at_start = true;
			continue;
		}
		// The whole name is shouting -- `NOTIFICATION_ENTER_TREE`, `IDLE` -- so every character but
		// the first of each word comes down, where a method name keeps the tail it was written with.
		out += at_start ? (char)std::toupper((unsigned char)c) : (char)std::tolower((unsigned char)c);
		at_start = false;
	}
	return out;
}

std::string verse_binding_enum_name(const std::string &p_owner, const std::string &p_godot_enum) {
	return verse_binding_class_name(p_owner) + "_" + verse_binding_class_name(p_godot_enum);
}

std::vector<std::string> verse_binding_enumerator_names(const std::vector<std::string> &p_godot_names) {
	std::vector<std::string> full;
	full.reserve(p_godot_names.size());
	for (const std::string &name : p_godot_names) {
		full.push_back(verse_binding_constant_name(name));
	}
	if (p_godot_names.size() < 2) {
		return full;
	}

	// The words every name starts with, never consuming the last word -- `TIMER_PROCESS_PHYSICS`
	// and `TIMER_PROCESS_IDLE` share `TIMER_PROCESS`, and a one-word name keeps itself.
	std::vector<std::vector<std::string>> split;
	size_t shortest = SIZE_MAX;
	for (const std::string &name : p_godot_names) {
		std::vector<std::string> words;
		std::string word;
		for (const char c : name) {
			if (c == '_') {
				words.push_back(word);
				word.clear();
			} else {
				word += c;
			}
		}
		words.push_back(word);
		shortest = std::min(shortest, words.size());
		split.push_back(words);
	}

	size_t shared = 0;
	for (size_t i = 0; i + 1 < shortest; i++) {
		bool all = true;
		for (const std::vector<std::string> &words : split) {
			all = all && words[i] == split[0][i];
		}
		if (!all) {
			break;
		}
		shared++;
	}
	if (shared == 0) {
		return full;
	}

	std::vector<std::string> shortened;
	std::set<std::string> distinct;
	for (const std::vector<std::string> &words : split) {
		std::string name;
		for (size_t i = shared; i < words.size(); i++) {
			name += verse_binding_constant_name(words[i]);
		}
		// Abandoned all or nothing, so one enum reads one way: a `Bool` beside a `TypeInt` would be
		// worse than either. An identifier may not start with a digit, and two enumerators may not
		// come out the same.
		if (name.empty() || std::isdigit((unsigned char)name[0]) || !distinct.insert(name).second) {
			return full;
		}
		shortened.push_back(name);
	}
	return shortened;
}

bool verse_binding_can_be_property(const std::string &p_type) {
	return reader_for(p_type).accessor != nullptr;
}

bool verse_binding_is_predicate(const std::string &p_godot_name, const std::set<std::string> &p_sibling_names) {
	// The read half of a property answers a value rather than a test, and its setter is how you
	// tell: `is_point_disabled` pairs with `set_point_disabled`, and the servers' flattened
	// `font_is_force_autohinter` pairs with `font_set_force_autohinter`.
	static const char *const readers[] = { "is_", "get_", "has_" };
	for (const char *const reader : readers) {
		if (p_godot_name.rfind(reader, 0) == 0 &&
				p_sibling_names.count("set_" + p_godot_name.substr(strlen(reader))) > 0) {
			return false;
		}
		const std::string inner = std::string("_") + reader;
		const size_t at = p_godot_name.find(inner);
		if (at != std::string::npos) {
			const std::string twin = p_godot_name.substr(0, at + 1) + "set_" +
					p_godot_name.substr(at + inner.size());
			if (p_sibling_names.count(twin) > 0) {
				return false;
			}
		}
	}

	// `gen_verse_api.py`'s PREDICATE_NAME_RE, at a word boundary. Its companion table of predicates
	// whose name carries no prefix -- `class_exists`, `test_move`, 24 more -- is not ported: every
	// entry in it names a *mirrored* class, and a mirrored class is never bound. An addon's
	// unprefixed predicate answers a `logic`, which is the same thing the mirror did for all of
	// them before that table was read off the dump by hand.
	static const char *const prefixes[] = { "is_", "has_", "can_", "are_", "should_", "supports_",
		"overlaps_", "intersects_", "matches_", "was_" };
	for (const char *const prefix : prefixes) {
		if (p_godot_name.rfind(prefix, 0) == 0 || p_godot_name.find(std::string("_") + prefix) != std::string::npos) {
			return true;
		}
	}
	return false;
}

std::string verse_emit_binding_class(const VerseBindingClass &p_class, const VerseBindingEnumNames &p_enums) {
	// Built before the header, because a class with no members is written in the other of Verse's
	// two forms and the choice cannot be made until the members are in.
	std::string out;

	for (const VerseBindingSignal &signal : p_class.signals) {
		const std::string name = verse_binding_member_name(signal.godot_name);
		std::string payload;
		if (signal.arg_types.size() == 1) {
			payload = signal.arg_types[0];
		} else if (signal.arg_types.size() > 1) {
			payload = "tuple(";
			for (size_t i = 0; i < signal.arg_types.size(); i++) {
				if (i > 0) {
					payload += ", ";
				}
				payload += signal.arg_types[i];
			}
			payload += ")";
		}
		// `signal()` is the alias for `signal(tuple())`, which is how a payloadless signal is
		// spelled -- the same way /Verse.org/Concurrency spells `listenable()`.
		out += "\t";
		out += name;
		out += "<public>:signal(";
		out += payload;
		out += ") = signal(";
		out += payload;
		out += "){}\n";
	}

	// A GDScript `var` as the accessor pair Godot itself would have given it, had it been a ClassDB
	// property: `GetSpeed()` and `SetSpeed(V)`. The names are invented, which is done here and
	// almost nowhere else -- the alternative was a Verse member, and that spelling costs the class
	// its archetype (see VerseBindingProperty).
	for (const VerseBindingProperty &property : p_class.properties) {
		const std::string name = verse_binding_member_name(property.godot_name);
		const bool is_enum = p_enums.count(property.type) > 0;
		const std::string read = "Get(\"" + property.godot_name + "\")";

		out += "\tGet" + name + "<public>()<transacts>:" + property.type + " =\n\t\t";
		if (is_enum) {
			out += "VhTo" + enum_stem(property.type) + "(" + read + ")\n";
		} else {
			const FReader reader = reader_for(property.type);
			out += "if (Read := " + read + "." + reader.accessor + "[]) then Read else " +
					reader.fallback + "\n";
		}

		out += "\tSet" + name + "<public>(Value:" + property.type + ")<transacts>:void =\n\t\t";
		out += "Set(\"" + property.godot_name + "\", " +
				pack_expression(property.type, "Value", p_enums) + ")\n";
	}

	for (const VerseBindingMethod &method : p_class.methods) {
		out += emit_method(method, call_expression(method, p_enums), std::string(), p_enums, "\t");
	}

	std::string header = p_class.verse_class;
	header += "<public> := class(";
	header += p_class.verse_base;

	if (out.empty()) {
		// A binding with nothing callable is still worth declaring, because it is a *type* -- a
		// parameter, a cast target, a base for the next binding down. It is the *indented* form
		// that needs a member, not Verse: `class(node2d) {}` compiles, takes a downcast and serves
		// as a parameter type, all three measured in `tests/verse_probe/empty_class_probe.verse`.
		// This used to carry a `Bound<public>()<reads>:logic = true` filler instead, which read as
		// a member of the author's own class that they could neither find nor explain.
		//
		// A whole class of them is ordinary rather than exceptional: a GDScript declaring only
		// Godot virtuals and `@export` variables has nothing a binding can carry today, because a
		// leading underscore is skipped and properties are R-INT-9's remainder.
		return header + ") {}\n";
	}

	return header + "):\n" + out;
}

std::string verse_emit_binding_enums(const VerseBindingClass &p_class) {
	std::string out;
	for (const VerseBindingEnum &bound : p_class.enums) {
		const std::string owner = p_class.godot_class.empty() ? p_class.script_class : p_class.godot_class;
		const std::string stem = enum_stem(bound.verse_name);
		out += "# Godot's " + owner + "." + bound.godot_name + ".\n";
		out += bound.verse_name + "<public> := enum:\n";
		for (const auto &value : bound.values) {
			out += "\t" + value.first + "\n";
		}

		// variant -> enum, and total: a method that answers a *value* must not claim it can fail.
		// A number no enumerator has is Godot disagreeing with its own metadata, and the first
		// enumerator is what the mirror answers for one too. Internal, because it is what a
		// generated body needs and not something a script has any use for -- the public direction
		// is `ToInt` below, which is also what makes a bitfield combination spellable.
		out += "\nVhTo" + stem + "(Value:variant)<reads>:" + bound.verse_name + " =\n";
		out += "\tcase (Value.AsInt[] or -1):\n";
		std::set<int64_t> seen;
		for (const auto &value : bound.values) {
			// Godot lets two enumerators share a number -- an alias -- and a `case` may not answer
			// one twice. First wins, which is the one Godot's own list names first.
			if (seen.insert(value.second).second) {
				out += "\t\t" + std::to_string(value.second) + " => " + bound.verse_name + "." + value.first + "\n";
			}
		}
		out += "\t\t_ => " + bound.verse_name + "." + bound.values.front().first + "\n";

		out += "\nToInt<public>(Value:" + bound.verse_name + ")<reads>:int =\n";
		out += "\tcase (Value):\n";
		for (const auto &value : bound.values) {
			out += "\t\t" + bound.verse_name + "." + value.first + " => " + std::to_string(value.second) + "\n";
		}
		out += "\n";
	}
	return out;
}

std::string verse_emit_binding_statics(const VerseBindingClass &p_class, const VerseBindingEnumNames &p_enums) {
	if (p_class.statics_module.empty()) {
		return std::string();
	}

	std::string out = p_class.statics_module + "<public> := module:\n";
	for (const VerseBindingConstant &constant : p_class.constants) {
		out += "\t" + constant.verse_name + "<public>:" + constant.type + " = " + constant.literal + "\n";
	}

	for (const VerseBindingMethod &method : p_class.statics) {
		// A script class has no ClassDB entry, so its statics live on the script *resource* and the
		// resource has to be loaded before one can be called. A ClassDB class needs no such guard:
		// `ClassDB.class_call_static` takes the class by name.
		const std::string guard = p_class.godot_class.empty()
				? "StaticScript := GetResourceLoader().Load[\"" + p_class.script_path +
						"\", \"Script\", resource_loader_cache_mode.Reuse]"
				: std::string();
		out += emit_method(method, static_call_expression(method, p_class, p_enums), guard, p_enums, "\t");
	}
	return out;
}

std::string verse_emit_bindings(const std::vector<VerseBindingClass> &p_classes) {
	if (p_classes.empty()) {
		return std::string();
	}

	std::string out =
			"# Generated by godot-verse. Do not edit: this file is rewritten whenever the class\n"
			"# roster changes, and it is not what an author's own Verse lives in.\n"
			"#\n"
			"# One class per Godot class the mirror does not carry -- a third-party GDExtension's, or\n"
			"# one a script declares with `class_name`. See docs/generated-bindings.md.\n"
			"\n"
			"using { /Godot.org/Godot }\n";

	// Every enum this generation declares, which a member of any class may name: a method of the
	// first class emitted can take the last class's enum, and nothing orders a Godot class list.
	VerseBindingEnumNames enums;
	for (const VerseBindingClass &binding : p_classes) {
		for (const VerseBindingEnum &bound : binding.enums) {
			enums[bound.verse_name] = bound.values.front().first;
		}
	}

	// Enums first, because they are types and the classes below name them. Verse would not mind
	// either order -- module-scope definitions resolve in any -- but a reader would.
	for (const VerseBindingClass &binding : p_classes) {
		const std::string block = verse_emit_binding_enums(binding);
		if (!block.empty()) {
			out += "\n" + block;
		}
	}

	for (const VerseBindingClass &binding : p_classes) {
		out += "\n";
		out += verse_emit_binding_class(binding, enums);
	}

	for (const VerseBindingClass &binding : p_classes) {
		const std::string block = verse_emit_binding_statics(binding, enums);
		if (!block.empty()) {
			out += "\n" + block;
		}
	}
	return out;
}
