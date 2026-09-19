#ifndef VERSE_BINDINGS_H
#define VERSE_BINDINGS_H

#include <map>
#include <set>
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
	/// Its `bool` is a test rather than a value, so it is spelled `<decides>:void` -- the mirror's
	/// own split, and `verse_binding_is_predicate` is the rule. Never set for a script class: see
	/// that function.
	bool is_predicate = false;
	/// Godot dispatches it without an object, so it is not a member of the class at all. Verse has
	/// no `static`, so it goes where the constants go -- the class's `...Statics` module.
	bool is_static = false;
};

/// One `var` a GDScript class declares, reached the way the mirror's own properties are:
/// `set Thing.Speed = 2.0`, over a member whose `<getter>`/`<setter>` call Godot.
///
/// **`= external{}` is available here although this is a Source package**, which is the one thing
/// about it worth knowing: the rule that bans `external{}` outside a digest is waived for a member
/// *with accessors*, in as many words -- *"optional accessors must be initialized with
/// `= external{}` regardless of package role"* (`SemanticAnalyzer.cpp:20121`). Without it the var
/// would have to be uninitialized, which is the only other spelling the compiler takes, and then
/// every archetype of the class would have to supply it -- `mob{}` would stop compiling and
/// R-INT-12's construction with it.
///
/// A **container**-typed property is the exception and gets `GetTag()`/`SetTag(V)` instead; see
/// `verse_binding_property_is_member`.
///
/// **Only a script class has these.** A ClassDB property is defined in terms of a getter and a
/// setter *method*, both of which are in the class's method list already, so a ClassDB binding gets
/// `GetProcessCallback()` and `SetProcessCallback()` with nothing added here.
struct VerseBindingProperty {
	std::string godot_name;
	/// Verse type of the value, which may be one of this generation's own enums.
	std::string type;
};

/// One constant, in the class's `...Statics` module: `ThingStatics.Limit`.
struct VerseBindingConstant {
	/// Already in Verse's spelling -- `LIMIT` arrives here as `Limit`.
	std::string verse_name;
	std::string type;
	/// The Verse literal, spelled by the enumeration: `7`, `1.5`, `"t"`, `true`.
	std::string literal;
};

/// One enum, declared at the package's module scope because Verse has no type inside a class.
///
/// `Thing.State` is `thing_state`, which is `gen_verse_api.py`'s transform for a mirrored enum
/// applied to a bound class. The enumerators keep Godot's order and carry its numbers, because a
/// Verse enum has no values of its own and the converters are what map them.
struct VerseBindingEnum {
	/// Godot's own name for it -- `State` -- which is what the generated comment says it came from.
	std::string godot_name;
	std::string verse_name;
	std::vector<std::pair<std::string, int64_t>> values;
};

/// Every enum a generation declares: its Verse name -> its first enumerator, which is the value a
/// converter answers when Godot hands back a number no enumerator has. Built once per package,
/// because a member of any class may name any class's enum.
using VerseBindingEnumNames = std::map<std::string, std::string>;

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
	/// Where that script lives, so a click on the binding can open the file the author really
	/// wrote. Empty for a ClassDB class, which has no source under res:// at all.
	std::string script_path;
	/// The generated Verse class name.
	std::string verse_class;
	/// The Verse class this one derives from: another binding, or a mirrored class.
	std::string verse_base;

	std::vector<VerseBindingMethod> methods;
	std::vector<VerseBindingSignal> signals;
	std::vector<VerseBindingProperty> properties;

	/// The three that are not members of the class. A static is dispatched without an object and
	/// Verse has no `static`; a constant is data on a type, which Verse has no spelling for either;
	/// an enum is a type. All three go to module scope, the first two inside `statics_module`.
	std::vector<VerseBindingMethod> statics;
	std::vector<VerseBindingConstant> constants;
	std::vector<VerseBindingEnum> enums;
	/// `ThingStatics`, or empty when there is nothing to put in one. Godot's own spelling of the
	/// class plus the suffix, which is the mirror's rule and the reason for it: `Thing` alone would
	/// make any local of that name ambiguous.
	std::string statics_module;
};

struct VerseBindings {
	/// The whole package as one Verse snippet, or empty when there is nothing to bind.
	std::string source;
	std::vector<VerseBindingClass> classes;
	/// Bindings emitted as a bare type because the script could not be loaded to describe it --
	/// which a GDScript naming a Verse class cannot be until the Verse project has built. The
	/// members fill in on a later generation; until then the class exists and is empty.
	std::vector<std::string> incomplete;
};

/// `RapierBody2D` -> `rapier_body2d`, which is `gen_verse_api.py`'s `verse_class_name` exactly.
/// The differential test in the units layer is what keeps the two from drifting.
std::string verse_binding_class_name(const std::string &p_godot_name);

/// `move_and_slide` -> `MoveAndSlide`, which is `gen_verse_api.py`'s `pascal_member_name`.
std::string verse_binding_member_name(const std::string &p_godot_name);

/// `MAX_FRAMES` -> `MaxFrames`, which is not `verse_binding_member_name`: a constant's whole name
/// is shouting, so every part but the first character of each word has to come down.
std::string verse_binding_constant_name(const std::string &p_godot_name);

/// `Thing`, `State` -> `thing_state`. The enum's own name is not enough to be unique -- two bound
/// classes may both declare a `State` -- and the package has no modules to separate them with.
std::string verse_binding_enum_name(const std::string &p_owner, const std::string &p_godot_enum);

/// Verse names for one enum's enumerators, in Godot's order.
///
/// `gen_verse_api.py`'s rule, and for its reason: the enumerators' own longest shared prefix is
/// stripped rather than the enum's name, because Godot's prefixing is only half consistent --
/// `TIMER_PROCESS_PHYSICS` and `TIMER_PROCESS_IDLE` share `TIMER_PROCESS_` where the enum is called
/// `TimerProcessCallback`. Stripping is all or nothing per enum, abandoned when a stripped name
/// would start with a digit or collide with another in the same enum, so one enum reads one way.
std::vector<std::string> verse_binding_enumerator_names(const std::vector<std::string> &p_godot_names);

/// Whether a Verse type can be a property at all.
///
/// A class-typed one cannot, for the reason an object result is `<decides>`: the getter would have
/// to answer something for "Godot has nothing here", and a class has no such value. Every other
/// type the enumeration can name has a reader and a builder.
bool verse_binding_can_be_property(const std::string &p_type);

/// Whether such a property is spelled as a Verse *member* rather than as an accessor pair.
///
/// A container type is not, and it is the only kind that is not: `string` is `[]char`, and Verse
/// asks a container-typed var for *indexed* accessor overloads -- `TagGetter(:accessor, :int):char`
/// -- so that `set Thing.Tag[0] = 'x'` could resolve. The mirror meets the same wall and skips all
/// 403 of them, which is R-INT-9's own "except where a nested struct or a container forces a
/// getter/setter pair"; there it leaves Godot's own accessors standing as methods, and here, for a
/// GDScript `var` that has none, the pair is generated.
bool verse_binding_property_is_member(const std::string &p_type);

/// Whether a `bool`-returning Godot method is a test rather than a value.
///
/// `gen_verse_api.py`'s `is_predicate_method`, minus the two tables that only a mirrored class can
/// be in: the name rule and the `set_` twin. A method with a setter twin is the read half of a
/// property and answers a value, which is why `p_sibling_names` is the class's own method list.
///
/// **Asked of a ClassDB class only.** A GDScript `func heavy() -> bool` carries no claim to being a
/// test -- GDScript has no const, and the name rule alone would make `is_alive()` a `<decides>` on
/// the strength of two letters an author never chose for that meaning.
bool verse_binding_is_predicate(const std::string &p_godot_name, const std::set<std::string> &p_sibling_names);

/// The Verse text for one class, with no trailing blank line.
///
/// `p_enums` is every enum name this generation declares, which the bodies need because an enum
/// crosses the wire as an int: a parameter of one packs through `ToInt` and a result of one is read
/// back through the converter beside the declaration.
std::string verse_emit_binding_class(const VerseBindingClass &p_class, const VerseBindingEnumNames &p_enums);

/// The enum declarations for one class, with the two converters each needs, or empty.
std::string verse_emit_binding_enums(const VerseBindingClass &p_class);

/// The class's `...Statics` module -- its constants and its static methods -- or empty.
std::string verse_emit_binding_statics(const VerseBindingClass &p_class, const VerseBindingEnumNames &p_enums);

/// The whole package: the `using` and one class per entry, in the order given.
///
/// Empty when there are no classes, which is what a project with no addons and no `class_name`
/// scripts has -- and the host wants an empty string for that rather than a package with nothing
/// in it, because an empty Verse snippet is a parse error.
std::string verse_emit_bindings(const std::vector<VerseBindingClass> &p_classes);

#endif // VERSE_BINDINGS_H
