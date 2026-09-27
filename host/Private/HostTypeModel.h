// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "AutoRTFM.h"
#include "Containers/Array.h"
#include "Containers/Map.h"
#include "Containers/StringView.h"
#include "Containers/UnrealString.h"
#include "HostScript.h"
#include "Templates/SharedPointer.h"

namespace uLang {
class CClass;
class CDataDefinition;
class CEnumeration;
class CNormalType;
class CSemanticProgram;
class CTypeBase;
}

namespace verse_math {
struct field;
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
/// path. A class nested inside another, or left behind by a retired generation, resolves at none of
/// them and is Other.
///
/// The first three numbers are the sidecar's `refOrigin`, which a runtime host and the interpreter
/// both read; a Binding is recorded there as Other, which is what it was before it had a name.
enum class EClassOrigin : uint8
{
    Other = 0,
    Mirrored = 1,
    Script = 2,
    Binding = 3,
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

/// Class and the unbroken run of its superclasses of Origin, base first. Empty when Class itself is
/// not of Origin. For Script, the chain a script class's own members, exports and signals live on:
/// above it is generated API whose members are Godot's own properties.
AUTORTFM_DISABLE TArray<const uLang::CClass*> ClassChainOfOrigin(const uLang::CClass& Class,
                                                                 const uLang::CSemanticProgram& Program,
                                                                 EClassOrigin Origin);

/// The nearest class of Origin in Class's own superclass chain, Class included, or null.
AUTORTFM_DISABLE const uLang::CClass* NearestAncestorOfOrigin(const uLang::CClass& Class,
                                                             const uLang::CSemanticProgram& Program,
                                                             EClassOrigin Origin);

/// `player` at the script package's root, `gameplay/player` in a module, and the bare name for a
/// class outside the script package. What every ClassNameUtf8 in the ABI carries.
AUTORTFM_DISABLE FUtf8String QualifiedNameOf(const uLang::CClass& Class);

/// The generated layout of the mirrored math struct of that Verse name, or null.
AUTORTFM_DISABLE const verse_math::layout* FindStructLayout(FUtf8StringView VerseName);

/// A mirrored struct whose value can cross, and the fields the Godot type is built from.
///
/// Order is Godot's, not the declaration's: the wire carries a tuple of numbers and the consumer
/// rebuilds a Vector2 or a Color by position, so these are the positions. Verse's own struct
/// declarations in GodotApi.native.verse happen to agree, which is convenient and not the contract.
// The math types' shapes come from the generator, which builds them from the same list that emits
// the Verse structs and their packers -- see GodotMathLayout.gen.h. Aliased rather than renamed so
// that the call sites below read as they did when there were three hand-written entries.
using FStructLayout = verse_math::layout;
using FStructField = verse_math::field;

/// What a script's class declares a member as, beyond what the value sitting in the slot can say.
///
/// Both marshalling directions need this, for the same reason in two shapes. A read cannot tell a
/// `?node2d` holding nothing from a `logic` holding false, because Verse spells an empty option and
/// false with the same cell. A write has to build a value of the member's declared class, and an
/// empty slot does not name one.
struct FMemberType
{
    /// What ClassifyDeclaredType answered, which is what the converters switch over. A sidecar does
    /// not carry it: ReadMemberType recovers it from the fields below (RecordedKind).
    EDeclaredKind Kind = EDeclaredKind::Other;
    const uLang::CDataDefinition* Member = nullptr;
    /// Whether the member was declared `var`. The *pointer* above answered this until a runtime
    /// host had to: `Member->IsVar()` is null there, which read as "every member is read-only" and
    /// silently dropped every write an exported game made to its own state.
    bool bIsVar = false;
    /// The class a reference member or parameter holds, and which package declares it. Null and
    /// Other for one of any other type.
    ///
    /// The *pointer* is the analysis's, and a runtime host has no semantic program to hold one in:
    /// everything but GetClassSignals reads only the two names, so those are carried beside it and
    /// are what a description read back out of the sidecar has.
    const uLang::CClass* ReferenceClass = nullptr;
    /// `node2d` -- what FindMirroredClass takes. Empty for a type that is not a reference, and the
    /// test for "is this a reference" everywhere the pointer is not available.
    FUtf8String ReferenceName;
    /// `/Godot.org/Godot/node2d` -- what FindGodotClass takes for a script class.
    FUtf8String ReferenceQualifiedName;
    EClassOrigin ReferenceOrigin = EClassOrigin::Other;
    /// Whether it was declared `?node2d` rather than `node2d`. An *exported member* must be optional
    /// -- the inspector can leave a slot empty, and VH_EXPORT_OBJECT_NOT_OPTIONAL says so -- but a
    /// method argument always arrives with a value, so both spellings are legal there and the
    /// difference is only whether the value handed over is wrapped.
    bool bReferenceIsOption = false;
    /// The mirrored struct a member is declared as, which is where its field names come from --
    /// there is nothing in a value to read them off. The layout is a generated table every host
    /// links, so the name beside it is enough to find it again after a round trip through JSON.
    const FStructLayout* Struct = nullptr;
    FUtf8String StructName;
    /// How many enumerators the declared enum has, or 0 for a member that is not one. The ordinal
    /// that crosses has to be checked against this, and the value in the slot cannot say: an enum
    /// over a native UEnum property is stored as the number itself.
    int32 EnumeratorCount = 0;
    /// The declared enum's decorated name -- `(/user@localhost:)exports_mode`. A member write finds
    /// the enumeration through the enumerator already in the slot; a method argument has no slot,
    /// so it has to be looked up, and this is what by.
    FUtf8String EnumerationName;
    /// What the export description makes of the same type. An array's element kind comes from here
    /// rather than from a classification of its own: the value cannot say -- an empty array has no
    /// element to look at, and the description is the answer the Godot side was already given.
    GodotVerse::FExportDesc Described;

    /// For a struct the *project* declares -- never one of Godot's sixteen, which have `Struct`
    /// above and a generated layout behind it. Held behind a pointer because the layout holds
    /// FMemberTypes of its own, which a struct with a struct field makes recursive.
    TSharedPtr<struct FUserStructLayout> UserStruct;
};

/// A project's own struct, as much of it as building one back from the wire needs.
///
/// The mirrored math types have `FStructLayout`, generated from extension_api.json and flat arrays
/// of scalars. Nothing generates anything for a struct a project declares, so this is read off the
/// semantic program instead -- and unlike the generated one it can carry any field type, because a
/// user struct can hold a string or an object where a vector2 cannot.
struct FUserStructLayout
{
    /// Decorated: `(/user@localhost:)strike_report`. What the VM knows it as.
    FUtf8String DecoratedName;
    /// Field keys and their declared types, in declaration order, base class first. The order is
    /// load-bearing twice over: it is the order Godot is told the arguments come in, and the order
    /// an inbound tuple is read back in. One walk fills both, which is why CollectStructFields
    /// exists rather than each side doing it.
    TArray<FUtf8String> FieldKeys;
    /// The field's own name, which is what Godot is told the argument is called.
    TArray<FUtf8String> FieldNames;
    TArray<FMemberType> FieldTypes;
};

/// The nearest mirrored class in Class's own superclass chain, Class included.
///
/// This is what decides how a reference slot is drawn, and a class the project declares cannot answer
/// it: ClassDB has never heard of the name that class registered with Godot, so asking whether `Mover`
/// descends from Node gets "no" and the inspector falls back to a resource picker. Its nearest
/// mirrored ancestor -- `node2d` -- is a name ClassDB does know.
///
/// Empty for a chain that reaches `object` without passing a mirror, which is a reference to something
/// Godot draws no picker for either way.
AUTORTFM_DISABLE FUtf8String NativeClassOf(const uLang::CClass& Class, const uLang::CSemanticProgram& Program);

/// The generated layout of the math struct that crosses under VariantTag, or null.
AUTORTFM_DISABLE const FStructLayout* FindStructLayoutByTag(int32 VariantTag);

/// The generated layout of the math struct whose packed array crosses under PackedArrayTag, or null.
AUTORTFM_DISABLE const FStructLayout* FindStructLayoutByPackedTag(int32 PackedArrayTag);

/// What the inspector can make of a member's declared type: the value's shape on the wire, the
/// Godot type to rebuild it as, the hint the declaration itself implies, and -- when the answer is
/// that it cannot be exported at all -- why.
///
/// The hint comes from the type wherever the type can carry it. A bounded Verse int or float is
/// already a range: `type{_X:float where 0.0 <= _X, _X <= 500.0}` normalises to bounds on the type
/// itself, and the compiler then enforces them at every assignment -- so an inspector slider built
/// from those bounds and the language agree by construction, rather than because the author wrote
/// the same two numbers twice. An enum is already a list of choices. A mirrored class is already
/// the name of the node or resource the slot will accept.
AUTORTFM_DISABLE void DescribeExportTypeOf(const FDeclaredType& Declared,
                                           const uLang::CSemanticProgram& Program,
                                           FExportDesc& OutDesc);

/// DescribeExportTypeOf, over Type's classification.
AUTORTFM_DISABLE void DescribeExportType(const uLang::CTypeBase* Type, const uLang::CSemanticProgram& Program, FExportDesc& OutDesc);

/// The class a parameter, a result or a signal argument names, in the two fields the ABI
/// carries for one -- vh_param_desc::ClassUtf8 and ClassKind, which document the rule.
///
/// The fallback is the whole of what this adds over reading the type: a script class Godot has
/// not registered is reported as its nearest *mirrored* ancestor, because a consumer can only
/// name a class Godot can resolve and `Node2D` says more than nothing. That is the same answer
/// an exported member of that type gets, and for the same reason.
AUTORTFM_DISABLE void DescribeClassOf(const FMemberType& Type, const uLang::CSemanticProgram& Program,
                                     FUtf8String& OutName, int32& OutKind);

/// The same description, for a type with no member behind it: a method's parameter or its result.
///
/// Everything DescribeMemberType knows comes from the declared type rather than from the
/// declaration, so a signature can be described exactly as a member is -- which is what lets one
/// pair of converters serve both field access and dispatch.
AUTORTFM_DISABLE FMemberType DescribeType(const uLang::CTypeBase* Type, const uLang::CSemanticProgram& Program);

/// Fills OutLayout from Struct's own fields, base class first.
///
/// One walk, used by both directions: `DescribePayload` names Godot's arguments from it and
/// `WireToValue` reads an inbound tuple back with it. Two walks would be two chances to disagree
/// about order, and a disagreement there is a silent mis-assignment rather than an error.
AUTORTFM_DISABLE void CollectStructFields(const uLang::CClass& Struct,
                                          const uLang::CSemanticProgram& Program,
                                          FUserStructLayout& OutLayout);

/// A struct a payload decomposes into arguments, or null for anything else.
///
/// The sixteen mirrored math types are structs too and are *not* decomposed: a vector2 payload is
/// one Vector2 argument, which is the whole of what Godot wants.
AUTORTFM_DISABLE const uLang::CClass* PayloadStructClass(const FDeclaredType& Payload);

/// How a `signal(t)`'s payload maps onto Godot's argument list.
///
/// Three shapes, because Verse has three answers to "what is one value carrying several things":
/// a tuple, which cannot name its elements; a struct, which can; and everything else, which is one
/// thing. The *emission* has to take the value apart the same way the descriptor put it together,
/// so one shape serves both rather than each deciding for itself.
enum class EPayloadShape : uint8
{
    /// One argument, the payload itself. `signal(int)`, `signal(node2d)`.
    Bare,
    /// One argument per element, positionally named. `tuple()` is this with no arguments.
    Tuple,
    /// One argument per top-level field, named by the field.
    Struct,
};

/// One Godot argument a payload decomposes into.
struct FPayloadArg
{
    /// What Godot is told the argument is called, and so what the connect dialog shows and what
    /// _make_function writes: `Arg0` for a tuple element, the field's own name for a struct.
    FUtf8String Name;
    /// The decorated key this field is stored under, for LoadField at emission. Empty unless the
    /// payload is a struct -- a tuple is an array and is read by index.
    FUtf8String FieldKey;
    FMemberType Type;
};

/// A payload's whole story: what it decomposes into, and -- when it decomposes into nothing usable
/// -- which argument spoiled it and why (vh_signal_reject).
struct FPayloadShape
{
    EPayloadShape Kind = EPayloadShape::Bare;
    TArray<FPayloadArg> Args;
    int32 Reject = VH_SIGNAL_OK;
    FUtf8String RejectDetail;
    /// The struct the payload decomposes, for Kind == Struct and null otherwise. Kept because the
    /// *inbound* direction has to build one back, and the semantic class is what names it.
    const uLang::CClass* StructClass = nullptr;
    /// The payload as one type, rather than as the arguments it decomposes into. What `Await`
    /// needs: an emission arrives as N Godot arguments and the event it feeds takes one `t`, so
    /// the inbound direction has to put back together exactly what DescribePayload took apart.
    FMemberType Whole;
};

/// The same thing for the *mirror's* signals, which belong to no script class: `timer.Timeout` ->
/// what a `Timer.Timeout().Await()` has to rebuild.
struct FEngineSignalTypes
{
    TMap<FUtf8String, FPayloadShape> Shapes;
};

/// The payload type of a signal class: the type argument the member's declaration instantiated it
/// with.
///
/// The declared type comes back as the *generic* `signal(t)` -- `AsCode` prints it that way
/// and its `Signal` method's parameter is still the type variable -- but the instantiation is
/// recorded on the class as a substitution table, with one entry per polarity. Both carry the same
/// type for a class this shape, so the first is the answer.
AUTORTFM_DISABLE const uLang::CTypeBase* SignalPayloadType(const uLang::CClass& Declared);

/// What a payload becomes on Godot's side (phase-4-design 6.2), and why it cannot become anything.
///
/// A `tuple()` is no arguments; a tuple of N is N; a struct is one per top-level field, named by
/// the field; anything else is one. The mapping is one level only -- a `vector2` payload is one
/// Vector2 argument, not two floats -- and it is the same list the signal descriptor reports and an
/// emission fills, so the arguments a generated handler is written for and the arguments that
/// arrive cannot disagree.
///
/// This is the one place that decides, so G1-G4's rejections are decidable from the declaration:
/// the editor reports them at the member and no emission has to discover them at runtime.
AUTORTFM_DISABLE void DescribePayload(const uLang::CTypeBase* Payload,
                                      const uLang::CSemanticProgram& Program,
                                      FPayloadShape& OutShape);

} // namespace GodotVerse
