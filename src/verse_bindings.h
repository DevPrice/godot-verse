#ifndef VERSE_BINDINGS_H
#define VERSE_BINDINGS_H

#include <string>
#include <vector>

// Generated bindings for classes the mirror does not carry: R-INT-7, and
// docs/generated-bindings.md.
//
// The mirror is generated from `extension_api.json`, which describes core Godot and nothing else,
// so two kinds of class have no declared type in Verse -- one a third-party GDExtension registers
// in ClassDB, and one a script declares with `class_name`. This turns each into an ordinary Verse
// subclass of its mirrored base, in a package of the project's own.
//
// **No godot-cpp in this header, deliberately**, which is the same rule the lexer, the class-decl
// scanner and the module map follow: the naming and the emission are pure text and are unit-tested
// standalone, and only the *enumeration* needs Godot. `verse_bindings_gen.cpp` is that half.

/// One class to bind, described the way the generator wants it rather than the way Godot reports
/// it. The enumeration fills these in; the emitter reads nothing else.
struct VerseBindingMethod {
	std::string godot_name;
	/// Verse type of the result, or empty for a method answering nothing.
	std::string result_type;
	/// The parameters, in order, already named and typed in Verse.
	std::vector<std::pair<std::string, std::string>> params;
	/// Godot says the method does not mutate the object. With a result, that is `<reads>`.
	bool is_const = false;
};

struct VerseBindingSignal {
	std::string godot_name;
	/// Verse types of the payload, in order. One element is `signal(t)`; several are a tuple.
	std::vector<std::string> arg_types;
};

struct VerseBindingClass {
	/// The ClassDB class name, or empty for a script class.
	std::string godot_class;
	/// The script's global `class_name`, or empty for a ClassDB class.
	std::string script_class;
	/// The generated Verse class name.
	std::string verse_class;
	/// The Verse class this one derives from: another binding, or a mirrored class.
	std::string verse_base;

	std::vector<VerseBindingMethod> methods;
	std::vector<VerseBindingSignal> signals;
};

struct VerseBindings {
	/// The whole package as one Verse snippet, or empty when there is nothing to bind.
	std::string source;
	std::vector<VerseBindingClass> classes;
};

/// `RapierBody2D` -> `rapier_body2d`, which is `gen_verse_api.py`'s `verse_class_name` exactly.
/// The differential test in the units layer is what keeps the two from drifting.
std::string verse_binding_class_name(const std::string &p_godot_name);

/// `move_and_slide` -> `MoveAndSlide`, which is `gen_verse_api.py`'s `pascal_member_name`.
std::string verse_binding_member_name(const std::string &p_godot_name);

/// The Verse text for one class, with no trailing blank line.
std::string verse_emit_binding_class(const VerseBindingClass &p_class);

/// The whole package: the `using` and one class per entry, in the order given.
///
/// Empty when there are no classes, which is what a project with no addons and no `class_name`
/// scripts has -- and the host wants an empty string for that rather than a package with nothing
/// in it, because an empty Verse snippet is a parse error.
std::string verse_emit_bindings(const std::vector<VerseBindingClass> &p_classes);

#endif // VERSE_BINDINGS_H
