#include "verse_bindings.h"

#include <cctype>

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
	// Every other parameter type this generator emits is a mirrored class, and an object packs
	// through the one builder that takes the whole hierarchy.
	return "VariantObject";
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

/// The call that reaches Godot, and the only place the two verbs are chosen between.
///
/// `CallConst` where Godot says the method is const, which is R-INT-9's classification and the
/// whole reason that verb exists: `Call` is `<transacts>`, so a binding built on it would start
/// wall 8's cascade in every caller that only reads.
std::string call_expression(const VerseBindingMethod &p_method) {
	std::string out = p_method.is_const && !p_method.result_type.empty() ? "CallConst(\"" : "Call(\"";
	out += p_method.godot_name;
	out += "\"";
	for (const auto &param : p_method.params) {
		out += ", ";
		out += builder_for(param.second);
		out += "(";
		out += param.first;
		out += ")";
	}
	out += ")";
	return out;
}

/// The reader that turns the answering `variant` back into the declared type, and the fallback for
/// when it declines. A binding never raises: Godot answering something unexpected is not the
/// author's mistake and is not worth an instance's content scope.
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

std::string verse_emit_binding_class(const VerseBindingClass &p_class) {
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

	for (const VerseBindingMethod &method : p_class.methods) {
		const FReader reader = reader_for(method.result_type);
		out += "\t";
		out += verse_binding_member_name(method.godot_name);
		out += "<public>(";
		out += param_list(method);
		out += ")";
		// A method answering nothing is `<transacts>` whatever Godot's const flag says: the 38
		// const-and-void methods in Godot's own API are `OS.set_environment` and friends, which
		// plainly do something. The test is const *and answering*, the mirror's exactly.
		out += method.is_const && !method.result_type.empty() ? "<reads>" : "<transacts>";
		out += ":";
		out += method.result_type.empty() ? "void" : method.result_type;
		out += " =\n\t\t";
		if (method.result_type.empty()) {
			// A void method still has to swallow the `variant` the call answers, which is what the
			// braces are for: a block whose last expression is a call is void.
			out += "{ ";
			out += call_expression(method);
			out += "; }\n";
		} else if (reader.accessor != nullptr) {
			out += "if (Read := ";
			out += call_expression(method);
			out += ".";
			out += reader.accessor;
			out += "[]) then Read else ";
			out += reader.fallback;
			out += "\n";
		} else {
			// No reader for the declared type, so the method answers the raw `variant`. A script
			// cannot spell that type, so this arm is only reached for a result the enumeration
			// should have refused -- it is here so a bad row is a compile error in the generated
			// file rather than a silently dropped method.
			out += call_expression(method);
			out += "\n";
		}
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
	for (const VerseBindingClass &binding : p_classes) {
		out += "\n";
		out += verse_emit_binding_class(binding);
	}
	return out;
}
