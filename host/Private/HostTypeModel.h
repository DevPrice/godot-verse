// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "AutoRTFM.h"
#include "Containers/StringView.h"
#include "Containers/UnrealString.h"

namespace uLang {
class CClass;
class CEnumeration;
class CNormalType;
class CSemanticProgram;
class CTypeBase;
}

namespace verse_math {
struct layout;
}

namespace GodotVerse {

/// The script package's verse path, pinned across generations (OQ-12).
inline constexpr const char* ScriptVersePath = "/user@localhost";
/// Where the generated Godot API lives. A class resolving under this is one of the mirrors.
inline constexpr const char* GodotVersePath = "/Godot.org/Godot";
/// The generated bindings (R-INT-7): a verse path of their own, so a binding whose name collides
/// with a mirrored one is an ambiguity at the use site rather than a redefinition in generated code.
inline constexpr const char* BindingsVersePath = "/Godot.org/Bindings";

/// Which package declares a class, proven by resolving the class's own name at that package's verse
/// path. A class nested inside another resolves at neither and is Other.
///
/// The numbers are the sidecar's `refOrigin`, which a runtime host and the interpreter both read.
enum class EClassOrigin : uint8
{
    Other = 0,
    Mirrored = 1,
    Script = 2,
};

/// What kind of type a declaration names, as the describers and converters of a declared type tell
/// them apart. Closed: each of them switches over it exhaustively, so a new kind fails the build at
/// every one that has not decided what to do with it.
enum class EDeclaredKind : uint8
{
    /// What no describer distinguishes: `void`, `any`, a function, a type, an option inside an
    /// option, and no type at all.
    Other,
    Logic,
    Int,
    Float,
    /// `char` and `char32`.
    Char,
    /// `[]char`, which is what Verse's `string` is.
    String,
    Enum,
    /// Any array but a string.
    Array,
    Map,
    Tuple,
    /// An option around anything but a Godot object; OptionOf says what it holds.
    Option,
    /// `variant`: the fixed-width lanes any Godot value crosses as.
    Variant,
    /// `rid`: a one-field struct that crosses as a scalar under VH_VARIANT_RID.
    Rid,
    /// One of the sixteen mirrored math structs, which cross as their components.
    MathStruct,
    /// A struct no generated layout describes -- the project's own.
    UserStruct,
    /// `godot_array`, `dictionary`, `callable`, `signal_ref`: a Godot value that crosses by id.
    Container,
    /// `typed_array(t)`, `typed_dictionary(k, v)`.
    TypedContainer,
    /// A subclass of the mirror's `object`, bare or optional: a handle to a Godot object.
    Reference,
    /// Any other class: `signal(t)`, `event(t)`, an interface, a class the project wrote that does
    /// not derive from `object`.
    OtherClass,
};

/// A declared type's kind, and what each kind needs in order to be described.
struct FDeclaredType
{
    EDeclaredKind Kind = EDeclaredKind::Other;
    /// For Option: what it holds, classified exactly as a bare declaration of that type would be.
    EDeclaredKind OptionOf = EDeclaredKind::Other;
    /// Whether an option was unwrapped: always for Option, and for Reference the `?node2d` spelling.
    bool bIsOption = false;

    /// The value type once a `var`'s pointer and one option are taken off. Null for no type.
    const uLang::CNormalType* Normal = nullptr;
    /// Every class kind's class, and an Option's when it holds one.
    const uLang::CClass* Class = nullptr;
    /// Which package declares Class. Not computed, so Other, for Variant and Rid.
    EClassOrigin Origin = EClassOrigin::Other;
    /// MathStruct's generated layout.
    const verse_math::layout* Layout = nullptr;
    /// Container's vh_variant_tag.
    int32 ContainerTag = 0;
    /// Enum's enumeration, and an Option's when it holds one.
    const uLang::CEnumeration* Enumeration = nullptr;
    /// Array's element type.
    const uLang::CTypeBase* Element = nullptr;
};

/// Classifies a member's, a parameter's or a result's declared type. Null is Other.
///
/// Kinds overlap as types, and the order the class tests run in is what settles each overlap:
///
///   - Reference is tested first, so a Godot class never reads as a container or a struct by name.
///   - `variant` and `rid` before any struct test, by whole verse path: both are structs, and left
///     to the struct tests `variant` is a user struct asking Godot for 22 arguments and `rid` one
///     handing Godot a one-field tuple. A project's own struct of either name is neither.
///   - Container and TypedContainer by name before the struct test, which is what the describers
///     have always keyed them on.
///   - MathStruct only for a *mirrored* struct with a layout, so a project's `vector2` is a
///     UserStruct rather than a shape read by Godot's field names.
///   - An option around anything but a Reference is Option, whatever it holds.
AUTORTFM_DISABLE FDeclaredType ClassifyDeclaredType(const uLang::CTypeBase* Type, const uLang::CSemanticProgram& Program);

/// The value type behind a declared type, with bOutIsOption saying whether one option was around
/// it. Takes off a `var`'s pointer specifically and never an array, because `string` is `[]char`.
AUTORTFM_DISABLE const uLang::CNormalType& UnwrapDeclaredType(const uLang::CTypeBase& Type, bool& bOutIsOption);

AUTORTFM_DISABLE EClassOrigin ClassOriginOf(const uLang::CClass& Class, const uLang::CSemanticProgram& Program);

/// `player` at the script package's root, `gameplay/player` in a module, and the bare name for a
/// class outside the script package. What every ClassNameUtf8 in the ABI carries.
AUTORTFM_DISABLE FUtf8String QualifiedNameOf(const uLang::CClass& Class);

/// The generated layout of the mirrored math struct of that Verse name, or null.
AUTORTFM_DISABLE const verse_math::layout* FindStructLayout(FUtf8StringView VerseName);

} // namespace GodotVerse
