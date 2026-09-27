#pragma once

#include "verse_api_classes.h"

#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/string_name.hpp>

#include <cstdint>
#include <string>

// The name Godot knows a declared class by, from the two fields the ABI carries for one
// (vh_param_desc::ClassUtf8 and ClassKind, and vh_export_desc's hint pair, which say the same
// thing two ways). Empty for a class Godot has no name for, which is what leaves an argument
// drawn as a plain Object rather than filtered by a name nothing can resolve.
godot::StringName verse_godot_class_name(const godot::String &p_verse_class, int32_t p_class_kind);

// A mirrored name is resolved through the generated API's table, the only place the two spellings
// are written down together. A script's class has no entry there -- nothing generated it -- and the
// name Godot knows it by is the PascalCase form of the name it registered with, derived here rather
// than sent across the ABI: the host would have to reimplement the transform to send it, and two
// implementations of one naming rule are one too many.
const char *verse_godot_class_for(const godot::String &p_verse_class);

// The same answer as verse_godot_class_for, but only when ClassDB has heard of it -- which is the
// test every caller that walks an ancestry has to make first.
//
// That table carries the sixteen math types and `rid` beside the 1036 classes, for their Godot
// names and their documentation pages, and not one of them is a ClassDB class: a Variant type is
// not registered there. So `ClassDB::get_parent_class("Vector3")` is a failed ERR_FAIL, printing
// *"Cannot get class 'Vector3'."* per keystroke on the completion path that asks -- and answering
// an empty name, so the walk it was starting finds nothing anyway. A value type has no ancestry to
// find: its members are all its own.
const char *godot_classdb_class_for(const godot::String &p_verse_class);

// The Godot class whose documentation describes a Verse class. That is the mirrored table plus the
// one name missing from it: `vh_object`. That is Godot.native.verse's hand-written native root, the
// base Godot's own mirrored `object` derives from -- so it is not part of the generated API and not
// in the table, and absent from it a hover on one of its three lifecycle methods would report a
// local constant, with a tooltip that says nothing and nowhere for a click to go. Godot's Object is
// what those methods belong to as far as the documentation is concerned.
//
// Deliberately not folded into verse_godot_class_for: that one answers "is this name part of the
// generated API", which `vh_object` is not, and the completion path relies on the distinction.
//
// The generated type table is the rest of the package's names: `variant` is Godot's Variant and
// `godot_array` is its Array, and neither is a class the mirror generates. A row with no page --
// `signal(t)`, `connection` -- answers nothing here rather than pointing at a page that describes
// something else, and falls through to the comment above the declaration.
const char *godot_doc_class_for(const godot::String &p_verse_class);

// The Godot method a mirrored Verse method stands for, keyed by the class that declares it. The
// Verse name cannot be inverted on its own: the transform to PascalCase drops the underscores
// that separated the words, so `SetPosition` could have come from any of several spellings.
const verse_api::method_mapping *godot_method_for(const godot::String &p_verse_class, const godot::String &p_verse_method);

// The Godot function one of the mirror's globals stands for, and the page it is documented on.
// Godot documents a function belonging to no class on @GlobalScope -- a class its documentation has
// and ClassDB does not, and the one GDScript sends a click on `print(` to -- so naming it is what
// gives a global the tooltip and the jump a mirrored method already gets.
//
// Two rows of this were written out here, `Print` and `IsInstanceValid`, and the other forty-eight
// globals the mirror hand-writes had none: `Smoothstep`, `LerpAngle`, `DegToRad` and the rest of
// GodotMath's scalar half all drew "Local Constant". The table is generated from what those files
// declare now, so a global added to one of them is documented by the next generation or by nothing.
const verse_api::global_mapping *godot_global_for(const godot::String &p_verse_name);

// `operator'.Length'` -> `Length`, and empty for a name that is not an extension method's.
//
// An extension method is a *module-level* definition -- `(V:vector2).Length<public>()` declares
// `operator'.Length'` beside everything else in the file -- so the owner the compiler reports for
// one is the file it is written in, and the type it is written *on* appears nowhere but its
// signature. This is what tells the two kinds of global apart before asking about either.
godot::String verse_extension_method_name(const godot::String &p_name);

// The receiver of an extension method, read off its declared type: the first parameter of
// `type{_(:vector2,:tuple())<reads>:float}`. Depth-counted rather than split on the first comma,
// because a receiver can be parametric -- `typed_array(node)` carries one of its own.
godot::String verse_receiver_type(const godot::String &p_function_type);

// The Godot class a generated singleton accessor hands out -- GetEngine's Engine, which is the
// only thing that accessor can be said to be. gen_verse_api.py spells one as `Get` and the Godot
// class' own name, so inverting it is a lookup in the class table rather than a table of its own.
godot::String godot_singleton_class_for(const godot::String &p_verse_name);

// Godot's lookup result for each kind of member the generated table knows. Every one of them
// routes the tooltip and the click into a different corner of the class documentation, and
// `_show_symbol_tooltip` asks a different question of ClassDB for each -- has_method against
// has_signal against has_integer_constant -- so a wrong kind is an empty box rather than a
// slightly-off label.
int64_t lookup_result_for(verse_api::member_kind p_kind);

// The Godot enum a mirrored Verse enum stands for, or nullptr for a name that is not one of
// them. The transform to `node_internal_mode` drops the word boundaries `Node.InternalMode` had,
// so the generated table is the only way back.
const verse_api::enum_mapping *godot_enum_for(const godot::String &p_verse_enum);

// The Godot documentation page a `...Statics` module stands for. The mirror reaches Godot's
// constants and static methods through one module per class, named the Godot class plus the
// suffix -- `Vector2Statics.Zero` is Vector2.ZERO -- so inverting it is a lookup in the class
// table, the same shape as the singleton accessor above.
//
// `GodotStatics` is the one that is not a class. It holds what belongs to no class, which is
// exactly what Godot documents on @GlobalScope -- a page its documentation has and ClassDB does
// not, and the one GDScript sends a click on `randf_range` to.
godot::String godot_statics_class_for(const godot::String &p_verse_name);

// Verse's own primitive types, and the Godot page that documents the values each one crosses as.
// A Verse `int` *is* the Godot int -- that is what it becomes at the boundary -- so sending a
// hover on one to Godot's page is the same answer GDScript gives, and by the same reasoning:
// `Variant::get_type_by_name(p_symbol)` is the second thing its lookup_code tries.
//
// `char` is here because `string` *is* `[]char` -- one type the compiler prints two ways, and the
// spelling an author meets in a signature it did not write. Answering one page for one type is
// what keeps the two spellings from disagreeing about what they are.
//
// `void` is deliberately absent, along with `any` and the rest. Godot documents no page for them,
// GDScript answers nothing for `void` either, and the alternative -- a box reading "Local
// Constant void" with nothing in it -- is what this table exists to stop.
const char *godot_doc_class_for_primitive(const godot::String &p_verse_type);

// Whether a definition is one of the Godot package's globals, rather than a member of one of its
// classes or anything the project declares.
//
// A definition at the top level has no class to be owned by and reports the file it was written in
// instead -- a snippet scope carries its path as its name -- so the owner agreeing with the path is
// what says "top level". Which file it is then separates the package from the project, whose one
// flat scope would otherwise let a script's own Print be documented as Godot's.
bool is_godot_package_global(const godot::String &p_owner, const godot::String &p_path);

// The Verse class one of Godot's stands for, or null when that class was not mirrored -- which a
// chain walk meets whenever gen_verse_api.py was run with --classes-file.
//
// The reverse of verse_godot_class_for, and the one lookup shared verbatim with
// verse_syntax_highlighter.cpp: both walked their own copy of this exact linear scan before this
// file existed. verse_bindings_gen.cpp answers the same question with a binary search instead,
// because its table is sorted by Godot name and it is asked once per class in the roster on every
// binding generation -- kept apart rather than folded in here, and said why in place.
const char *mirrored_class(const godot::String &p_godot_class);

// The Godot class a mirrored Verse class stands for, or empty when the name is not one of them --
// which is how a superclass is told to be another script's class rather than a piece of the
// generated API.
godot::String godot_class_for(const std::string &p_verse_class);

// Only a subset of Godot's classes is mirrored, so a node whose own class was not generated
// inherits from the nearest ancestor that was. `node` is the floor: every scripted node has
// one, and a template that names a class the project does not define would not compile.
godot::String verse_base_class_for(const godot::String &p_godot_class);
