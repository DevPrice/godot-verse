// Copyright Epic Games, Inc. All Rights Reserved.

#include "HostTypeModel.h"

// Before the generated layout, which names the ABI's variant tags without including it.
#include "HostScript.h"
#include "GodotMathLayout.gen.h"
#include "HostEngineAdapters.h"
#include "Math/UnrealMathUtility.h"
#include "Misc/Paths.h"
#include "ULangUEUtils.h"
#include "uLang/Semantics/DataDefinition.h"
#include "uLang/Semantics/SemanticClass.h"
#include "uLang/Semantics/SemanticEnumeration.h"
#include "uLang/Semantics/SemanticProgram.h"
#include "uLang/Semantics/SemanticTypes.h"

namespace GodotVerse {
namespace {

/// The class every mirrored Godot class derives from, and so the test for a Godot object.
AUTORTFM_DISABLE const uLang::CClass* GodotObjectClass(const uLang::CSemanticProgram& Program)
{
    return Program.FindDefinitionByVersePath<uLang::CClass>("/Godot.org/Godot/object");
}

/// Whether Class is the mirror's own struct of that name, by whole verse path: a project may
/// declare a struct called `variant` or `rid` in a module of its own, and that one is an ordinary
/// user struct.
AUTORTFM_DISABLE bool IsMirroredStructNamed(const uLang::CClass& Class, const UTF8CHAR* Name)
{
    if (!Class.IsStruct())
    {
        return false;
    }
    const FUtf8String Path = FULangConversionUtils::ULangStrToFUtf8String(
        Class.GetScopePath(UTF8CHAR('/'), uLang::EPathMode::PrefixSeparator));
    return Path.Equals(FUtf8String(GodotVersePath) + UTF8TEXT("/") + Name);
}

/// The vh_variant_tag of the Godot value a container class of that Verse name names, or 0.
AUTORTFM_DISABLE int32 ReferenceVariantTag(FUtf8StringView VerseName)
{
    for (const verse_math::reference_type& Reference : verse_math::reference_types)
    {
        if (VerseName.Equals(FUtf8StringView(reinterpret_cast<const UTF8CHAR*>(Reference.verse_name))))
        {
            return Reference.variant_tag;
        }
    }
    return 0;
}

AUTORTFM_DISABLE void ClassifyClass(const uLang::CClass& Class, const uLang::CSemanticProgram& Program, FDeclaredType& Out)
{
    Out.Class = &Class;

    const uLang::CClass* const ObjectClass = GodotObjectClass(Program);
    if (ObjectClass && Class.IsSubtypeOf(*ObjectClass))
    {
        Out.Kind = EDeclaredKind::Reference;
        Out.Origin = ClassOriginOf(Class, Program);
        return;
    }
    if (IsMirroredStructNamed(Class, UTF8TEXT("variant")))
    {
        Out.Kind = EDeclaredKind::Variant;
        return;
    }
    if (IsMirroredStructNamed(Class, UTF8TEXT("rid")))
    {
        Out.Kind = EDeclaredKind::Rid;
        return;
    }

    Out.Origin = ClassOriginOf(Class, Program);
    const FUtf8StringView Name(Class.AsNameCString());
    if (const int32 Tag = ReferenceVariantTag(Name))
    {
        Out.Kind = EDeclaredKind::Container;
        Out.ContainerTag = Tag;
        return;
    }
    if (Name.StartsWith(UTF8TEXT("typed_array")) || Name.StartsWith(UTF8TEXT("typed_dictionary")))
    {
        Out.Kind = EDeclaredKind::TypedContainer;
        return;
    }
    if (Class.IsStruct())
    {
        Out.Layout = Out.Origin == EClassOrigin::Mirrored ? FindStructLayout(Name) : nullptr;
        Out.Kind = Out.Layout ? EDeclaredKind::MathStruct : EDeclaredKind::UserStruct;
        return;
    }
    Out.Kind = EDeclaredKind::OtherClass;
}

AUTORTFM_DISABLE FDeclaredType ClassifyValueType(const uLang::CNormalType& Normal, const uLang::CSemanticProgram& Program)
{
    using uLang::ETypeKind;

    FDeclaredType Result;
    Result.Normal = &Normal;

    VH_EXHAUSTIVE_SWITCH_BEGIN
    switch (Normal.GetKind())
    {
    case ETypeKind::Class:
        ClassifyClass(Normal.AsChecked<uLang::CClass>(), Program, Result);
        break;
    case ETypeKind::Enumeration:
        Result.Kind = EDeclaredKind::Enum;
        Result.Enumeration = &Normal.AsChecked<uLang::CEnumeration>();
        break;
    case ETypeKind::Logic:
        Result.Kind = EDeclaredKind::Logic;
        break;
    case ETypeKind::Int:
        Result.Kind = EDeclaredKind::Int;
        break;
    case ETypeKind::Float:
        Result.Kind = EDeclaredKind::Float;
        break;
    case ETypeKind::Char8:
    case ETypeKind::Char32:
        Result.Kind = EDeclaredKind::Char;
        break;
    case ETypeKind::Array:
    {
        const uLang::CArrayType& Array = Normal.AsChecked<uLang::CArrayType>();
        if (Array.IsStringType())
        {
            Result.Kind = EDeclaredKind::String;
        }
        else
        {
            Result.Kind = EDeclaredKind::Array;
            Result.Element = Array.GetElementType();
        }
        break;
    }
    case ETypeKind::Map:
        Result.Kind = EDeclaredKind::Map;
        break;
    case ETypeKind::Tuple:
        Result.Kind = EDeclaredKind::Tuple;
        break;

    // UnwrapDeclaredType has taken Pointer and Reference off and one Option; what is left of them
    // here is an option inside an option, which nothing crosses.
    case ETypeKind::Pointer:
    case ETypeKind::Reference:
    case ETypeKind::Option:
    case ETypeKind::Unknown:
    case ETypeKind::False:
    case ETypeKind::True:
    case ETypeKind::Void:
    case ETypeKind::Any:
    case ETypeKind::Comparable:
    case ETypeKind::Rational:
    case ETypeKind::Path:
    case ETypeKind::Range:
    case ETypeKind::Type:
    case ETypeKind::Module:
    case ETypeKind::Generator:
    case ETypeKind::Function:
    case ETypeKind::Variable:
    case ETypeKind::Named:
    case ETypeKind::Persistable:
    case ETypeKind::Castable:
    case ETypeKind::Concrete:
    case ETypeKind::Union:
    case ETypeKind::Object:
        Result.Kind = EDeclaredKind::Other;
        break;
    }
    VH_EXHAUSTIVE_SWITCH_END
    return Result;
}

} // namespace

AUTORTFM_DISABLE FDeclaredType ClassifyDeclaredType(const uLang::CTypeBase* Type, const uLang::CSemanticProgram& Program)
{
    if (!Type)
    {
        return FDeclaredType{};
    }

    bool bIsOption = false;
    FDeclaredType Result = ClassifyValueType(UnwrapDeclaredType(*Type, bIsOption), Program);
    if (bIsOption)
    {
        Result.bIsOption = true;
        if (Result.Kind != EDeclaredKind::Reference)
        {
            Result.OptionOf = Result.Kind;
            Result.Kind = EDeclaredKind::Option;
        }
    }
    return Result;
}

AUTORTFM_DISABLE const uLang::CNormalType& UnwrapDeclaredType(const uLang::CTypeBase& Type, bool& bOutIsOption)
{
    using namespace uLang;

    const CNormalType* Normal = &Type.GetNormalType();
    while (Normal->GetKind() == ETypeKind::Pointer || Normal->GetKind() == ETypeKind::Reference)
    {
        Normal = &static_cast<const CInvariantValueType*>(Normal)->PositiveValueType()->GetNormalType();
    }

    bOutIsOption = Normal->GetKind() == ETypeKind::Option;
    if (bOutIsOption)
    {
        Normal = &static_cast<const COptionType&>(*Normal).GetValueType()->GetNormalType();
    }
    return *Normal;
}

AUTORTFM_DISABLE FUtf8String QualifiedNameOf(const uLang::CClass& Class)
{
    // The class's whole verse path, with the package's prefix taken off -- rather than
    // EPathMode::PackageRelative, which reaches for the package's root module and is a fatal
    // error rather than an empty answer for a class that has no package.
    const FUtf8String Path = FULangConversionUtils::ULangStrToFUtf8String(
        Class.GetScopePath(UTF8CHAR('/'), uLang::EPathMode::PrefixSeparator));
    const FUtf8String Prefix = FUtf8String(ScriptVersePath) + UTF8TEXT("/");
    return Path.StartsWith(Prefix) ? Path.RightChop(Prefix.Len()) : FUtf8String(Class.AsNameCString());
}

AUTORTFM_DISABLE EClassOrigin ClassOriginOf(const uLang::CClass& Class, const uLang::CSemanticProgram& Program)
{
    const FUtf8String Name = QualifiedNameOf(Class);
    const auto ResolvesAt = [&Class, &Program, &Name](const char* ScopePath) {
        const FUtf8String Path = FUtf8String(ScopePath) + UTF8TEXT("/") + Name;
        return Program.FindDefinitionByVersePath<uLang::CClass>(
                   FULangConversionUtils::FUtf8StringViewToULangStringView(Path))
            == &Class;
    };

    if (ResolvesAt(GodotVersePath))
    {
        return EClassOrigin::Mirrored;
    }
    if (ResolvesAt(ScriptVersePath))
    {
        return EClassOrigin::Script;
    }
    // The bindings package is generated as one unmodularized snippet (FindBindingClass), so a
    // binding's qualified name is its bare name -- which is what QualifiedNameOf falls back to for a
    // class outside the script package.
    if (ResolvesAt(BindingsVersePath))
    {
        return EClassOrigin::Binding;
    }
    return EClassOrigin::Other;
}

AUTORTFM_DISABLE TArray<const uLang::CClass*> ClassChainOfOrigin(const uLang::CClass& Class,
                                                                 const uLang::CSemanticProgram& Program,
                                                                 EClassOrigin Origin)
{
    TArray<const uLang::CClass*> Chain;
    for (const uLang::CClass* Cursor = &Class;
         Cursor != nullptr && ClassOriginOf(*Cursor, Program) == Origin;
         Cursor = Cursor->GetSuperClass())
    {
        Chain.Insert(Cursor, 0);
    }
    return Chain;
}

AUTORTFM_DISABLE const uLang::CClass* NearestAncestorOfOrigin(const uLang::CClass& Class,
                                                             const uLang::CSemanticProgram& Program,
                                                             EClassOrigin Origin)
{
    for (const uLang::CClass* Cursor = &Class; Cursor != nullptr; Cursor = Cursor->GetSuperClass())
    {
        if (ClassOriginOf(*Cursor, Program) == Origin)
        {
            return Cursor;
        }
    }
    return nullptr;
}

AUTORTFM_DISABLE const verse_math::layout* FindStructLayout(FUtf8StringView VerseName)
{
    for (const verse_math::layout& Layout : verse_math::layouts)
    {
        if (VerseName.Equals(FUtf8StringView(reinterpret_cast<const UTF8CHAR*>(Layout.verse_name))))
        {
            return &Layout;
        }
    }
    return nullptr;
}

namespace {

/// Read here as well as by verse_scan_class_decl on the Godot side, which answers the same question
/// off the source text because Godot asks it during the filesystem scan, before a host exists. The
/// two must agree: one decides whether a reference to the class can be exported, the other decides
/// whether Godot registers the name that reference would be filtered by.
constexpr const char* GlobalClassAttributePath = "/Godot.org/Godot/global_class";

/// Whether this is the class **named after the file it is written in**, which is the bridge's rule
/// for which of a file's top-level classes Godot ever hears about: only that one can go on a node,
/// and only that one's `@global_class` registers a Godot class name (R-LANG-6, R-NODE-2).
///
/// Asked of the class's own source path rather than of the module map, because the question is
/// about the *file* and a module says nothing about which class in it is the file's. A class with
/// no recorded location -- one out of a digest, or out of a package the project does not own --
/// answers false, which is the safe direction: the consumer would not have registered it either.
AUTORTFM_DISABLE bool IsClassNamedAfterItsFile(const uLang::CDefinition& Definition)
{
    FUtf8String Path;
    int32 Line = 0;
    int32 Column = 0;
    FillLocation(Definition, Path, Line, Column);
    if (Path.IsEmpty())
    {
        return false;
    }
    const FString Stem = FPaths::GetBaseFilename(FString(Path));
    return FUtf8String(Stem) == FUtf8String(Definition.AsNameCString());
}

/// The enumerators of an enum, comma separated in declaration order, which is how Godot's enum
/// hint spells the choices it offers.
AUTORTFM_DISABLE FUtf8String EnumeratorList(const uLang::CEnumeration& Enumeration)
{
    FUtf8String List;
    for (const uLang::TSRef<uLang::CEnumerator>& Enumerator : Enumeration.GetDefinitionsOfKind<uLang::CEnumerator>())
    {
        if (!List.IsEmpty())
        {
            List += UTF8TEXT(",");
        }
        List += FUtf8String(Enumerator->AsNameCString());
    }
    return List;
}

/// Whether Godot has a name for this class at all: `@global_class`, and being the class its own
/// file is named after.
///
/// Both halves are needed, and testing only the first was a defect an author met within minutes
/// (B19). A global class is collected per *path* -- `_get_global_class_name` answers one name
/// per script -- so a second global class in one file registers nothing and has nowhere to live.
AUTORTFM_DISABLE bool RegistersWithGodot(const uLang::CClass& Class, const uLang::CSemanticProgram& Program)
{
    const uLang::CClass* const GlobalClassAttribute =
        Program.FindDefinitionByVersePath<uLang::CClass>(GlobalClassAttributePath);
    return Class.HasAttributeSubclass(GlobalClassAttribute, Program) && IsClassNamedAfterItsFile(Class);
}

AUTORTFM_DISABLE void DescribeArrayElement(const uLang::CTypeBase* ElementType,
                                           const uLang::CSemanticProgram& Program,
                                           GodotVerse::FExportDesc& OutDesc)
{
    if (!ElementType)
    {
        return;
    }
    const FDeclaredType Element = ClassifyDeclaredType(ElementType, Program);

    VH_EXHAUSTIVE_SWITCH_BEGIN
    switch (Element.Kind)
    {
    case EDeclaredKind::Logic:
        OutDesc.VariantTag = VH_VARIANT_ARRAY;
        OutDesc.ElementVariantTag = VH_VARIANT_BOOL;
        OutDesc.Reject = VH_EXPORT_OK;
        return;
    case EDeclaredKind::Int:
        OutDesc.VariantTag = VH_VARIANT_PACKED_INT64_ARRAY;
        OutDesc.Reject = VH_EXPORT_OK;
        return;
    case EDeclaredKind::Float:
        OutDesc.VariantTag = VH_VARIANT_PACKED_FLOAT64_ARRAY;
        OutDesc.Reject = VH_EXPORT_OK;
        return;
    case EDeclaredKind::String:
        OutDesc.VariantTag = VH_VARIANT_PACKED_STRING_ARRAY;
        OutDesc.Reject = VH_EXPORT_OK;
        return;
    // Each of the mirrored structs has a packed array in Godot.
    case EDeclaredKind::MathStruct:
        OutDesc.VariantTag = Element.Layout->packed_array_tag;
        OutDesc.Reject = VH_EXPORT_OK;
        return;
    // A reference is deliberately not here: `[]node2d` cannot hold the empty element an array editor
    // starts a new row as, which is the same objection VH_EXPORT_OBJECT_NOT_OPTIONAL makes about a
    // bare reference.
    case EDeclaredKind::Reference:
    case EDeclaredKind::Other:
    case EDeclaredKind::Char:
    case EDeclaredKind::Enum:
    case EDeclaredKind::Array:
    case EDeclaredKind::Map:
    case EDeclaredKind::Tuple:
    case EDeclaredKind::Option:
    case EDeclaredKind::Variant:
    case EDeclaredKind::Rid:
    case EDeclaredKind::UserStruct:
    case EDeclaredKind::Container:
    case EDeclaredKind::TypedContainer:
    case EDeclaredKind::OtherClass:
        return;
    }
    VH_EXHAUSTIVE_SWITCH_END
}

/// A Godot object reference as the inspector sees it: a handle the consumer rebuilds an object
/// from, and an optional one as an option around that.
AUTORTFM_DISABLE void DescribeReferenceExport(const FDeclaredType& Declared,
                                              const uLang::CSemanticProgram& Program,
                                              GodotVerse::FExportDesc& OutDesc)
{
    const uLang::CClass& Class = *Declared.Class;
    const EClassOrigin Origin = Declared.Origin;
    OutDesc.Type = Declared.bIsOption ? VH_TYPE_OPTION : VH_TYPE_INT;
    OutDesc.VariantTag = VH_VARIANT_OBJECT;
    OutDesc.Hint = Origin == EClassOrigin::Script ? VH_EXPORT_HINT_SCRIPT_CLASS : VH_EXPORT_HINT_CLASS;
    // Qualified for a script class, so the consumer can find the class the hint names; a
    // mirrored one is Godot's own and has no module to qualify with.
    OutDesc.HintString = Origin == EClassOrigin::Script
        ? QualifiedNameOf(Class)
        : FUtf8String(Class.AsNameCString());
    OutDesc.NativeClass = NativeClassOf(Class, Program);

    // Nothing can force a value into an inspector slot, so a member that cannot hold the empty
    // case has a declared type the scene can always violate. The Verse spelling that compiles
    // without an option, `node2d{}`, is a handle of 0: a reference dead from birth, and
    // indistinguishable from one freed later.
    if (!Declared.bIsOption)
    {
        OutDesc.Reject = VH_EXPORT_OBJECT_NOT_OPTIONAL;
        return;
    }

    VH_EXHAUSTIVE_SWITCH_BEGIN
    switch (Origin)
    {
    case EClassOrigin::Script:
    {
        // The inspector filters a slot by a Godot class name, and only a class Godot has
        // *registered* has one. Two things are needed for that and testing one of them was a
        // defect an author met within minutes: `@global_class`, and being the class **named
        // after its own file**.
        //
        // The second is Godot's constraint rather than this bridge's preference, which is worth
        // knowing before trying to lift it. A global class is collected per *path* --
        // `_get_global_class_name` is a per-path virtual answering one name
        // (`script_language_extension.h:754`), and `EditorFileSystem::_get_global_script_class`
        // takes one `info.name` from it -- and `ScriptServer` maps that name back to the path,
        // so `load(path)` has to yield that one class. A second global class in one file has
        // nowhere to live.
        //
        // **So the member is exported anyway, filtered by the nearest mirrored Godot class.**
        // Refusing it would be the bridge deciding an author may not export a Resource because
        // of where they put the class, which is not its decision to make; a `Resource` picker
        // that accepts a `.tres` of that class is worth far more than no slot at all. This is
        // GDScript's own rule -- `_find_narrowest_native_or_global_class`, the *or* being the
        // half this used to skip. `by-hand-findings.md` B19.
        const bool bRegisters = RegistersWithGodot(Class, Program);
        if (!bRegisters)
        {
            OutDesc.Hint = VH_EXPORT_HINT_CLASS;
            OutDesc.HintString = OutDesc.NativeClass;
            OutDesc.Reject = OutDesc.NativeClass.IsEmpty() ? VH_EXPORT_SCRIPT_CLASS_NOT_GLOBAL
                                                          : VH_EXPORT_OK;
            return;
        }
        OutDesc.Reject = VH_EXPORT_OK;
        return;
    }
    case EClassOrigin::Mirrored:
        OutDesc.Reject = VH_EXPORT_OK;
        break;
    // Its own reason rather than VH_EXPORT_UNSUPPORTED_TYPE's generic one: the inspector has no
    // picker for a generated-binding class, but *why* differs from an unsupported value type, and
    // NativeClassOf above has already found the native base to suggest exporting instead, when the
    // binding's chain reaches one. Out of scope to lift this by design (docs/generated-bindings.md);
    // support is deferred, not refused for good.
    case EClassOrigin::Binding:
        OutDesc.Reject = VH_EXPORT_BINDING_CLASS_UNSUPPORTED;
        break;
    case EClassOrigin::Other:
        OutDesc.Reject = VH_EXPORT_UNSUPPORTED_TYPE;
        break;
    }
    VH_EXHAUSTIVE_SWITCH_END
}

} // namespace

AUTORTFM_DISABLE FUtf8String NativeClassOf(const uLang::CClass& Class, const uLang::CSemanticProgram& Program)
{
    const uLang::CClass* const Mirrored = NearestAncestorOfOrigin(Class, Program, EClassOrigin::Mirrored);
    return Mirrored ? FUtf8String(Mirrored->AsNameCString()) : FUtf8String();
}

AUTORTFM_DISABLE const FStructLayout* FindStructLayoutByTag(int32 VariantTag)
{
    for (const FStructLayout& Layout : verse_math::layouts)
    {
        if (Layout.variant_tag == VariantTag)
        {
            return &Layout;
        }
    }
    return nullptr;
}

AUTORTFM_DISABLE const FStructLayout* FindStructLayoutByPackedTag(int32 PackedArrayTag)
{
    for (const FStructLayout& Layout : verse_math::layouts)
    {
        if (Layout.packed_array_tag != 0 && Layout.packed_array_tag == PackedArrayTag)
        {
            return &Layout;
        }
    }
    return nullptr;
}

AUTORTFM_DISABLE void DescribeExportTypeOf(const FDeclaredType& Declared,
                                           const uLang::CSemanticProgram& Program,
                                           GodotVerse::FExportDesc& OutDesc)
{
    using namespace uLang;

    OutDesc.Reject = VH_EXPORT_UNSUPPORTED_TYPE;

    VH_EXHAUSTIVE_SWITCH_BEGIN
    switch (Declared.Kind)
    {
    case EDeclaredKind::Other:
        return;

    // An empty slot Godot has no way to draw, whatever the option holds.
    case EDeclaredKind::Option:
        OutDesc.Reject = VH_EXPORT_OPTION_NOT_OBJECT;
        return;

    case EDeclaredKind::Reference:
        DescribeReferenceExport(Declared, Program, OutDesc);
        return;

    // `variant` is any Godot value at all, which is a thing to *declare* rather than a shape: what
    // crosses is whatever the variant holds, so the wire type says "anything" and the consumer
    // turns that into Godot's NIL_IS_VARIANT. Not exportable for the reason an Array is not -- the
    // inspector has no editor for a value with no type.
    case EDeclaredKind::Variant:
        OutDesc.Type = VH_TYPE_VARIANT;
        OutDesc.VariantTag = VH_VARIANT_NIL;
        return;

    // A RID is a scalar on this wire -- VH_TYPE_INT under its own variant tag -- rather than the
    // one-field tuple its Verse struct looks like. Typed here so a method taking or answering one
    // reaches Godot as a RID; not exportable, because a RID names a live entry in a server's table
    // and nothing about it survives being written to a scene.
    case EDeclaredKind::Rid:
        OutDesc.Type = VH_TYPE_INT;
        OutDesc.VariantTag = VH_VARIANT_RID;
        return;

    // A reference wrapper names a Godot type that crosses as an id rather than as a value. Typed
    // here so a method taking one reports the right argument type to Godot; rejected for *export*
    // in the same breath, because the inspector has no editor for an arbitrary Array.
    case EDeclaredKind::Container:
        OutDesc.Type = VH_TYPE_REF;
        OutDesc.VariantTag = Declared.ContainerTag;
        return;

    // A mirrored struct is a value the inspector draws with an editor of its own: a colour picker,
    // a pair of spinboxes. It crosses as the numbers it is made of, tagged with which Godot type
    // to rebuild from them.
    case EDeclaredKind::MathStruct:
        OutDesc.Type = VH_TYPE_TUPLE;
        OutDesc.VariantTag = Declared.Layout->variant_tag;
        OutDesc.Reject = VH_EXPORT_OK;
        return;

    case EDeclaredKind::UserStruct:
    case EDeclaredKind::TypedContainer:
    case EDeclaredKind::OtherClass:
        return;

    // The ordinal, which is what GDScript and C# store too -- including the trap that reordering
    // the enumerators reinterprets every scene already saved.
    case EDeclaredKind::Enum:
        OutDesc.Type = VH_TYPE_INT;
        OutDesc.VariantTag = VH_VARIANT_INT;
        OutDesc.Hint = VH_EXPORT_HINT_ENUM;
        OutDesc.HintString = EnumeratorList(*Declared.Enumeration);
        OutDesc.Reject = VH_EXPORT_OK;
        return;

    case EDeclaredKind::Logic:
        OutDesc.Type = VH_TYPE_LOGIC;
        OutDesc.VariantTag = VH_VARIANT_BOOL;
        OutDesc.Reject = VH_EXPORT_OK;
        return;

    case EDeclaredKind::Int:
    {
        OutDesc.Type = VH_TYPE_INT;
        OutDesc.VariantTag = VH_VARIANT_INT;
        OutDesc.Reject = VH_EXPORT_OK;
        const CIntType& IntType = Declared.Normal->AsChecked<CIntType>();
        OutDesc.bHasRangeMin = IntType.GetMin().IsFinite();
        OutDesc.bHasRangeMax = IntType.GetMax().IsFinite();
        OutDesc.RangeMin = OutDesc.bHasRangeMin ? (double)IntType.GetMin().GetFiniteInt() : 0.0;
        OutDesc.RangeMax = OutDesc.bHasRangeMax ? (double)IntType.GetMax().GetFiniteInt() : 0.0;
        if (OutDesc.bHasRangeMin || OutDesc.bHasRangeMax)
        {
            OutDesc.Hint = VH_EXPORT_HINT_RANGE;
        }
        return;
    }

    case EDeclaredKind::Float:
    {
        OutDesc.Type = VH_TYPE_FLOAT;
        OutDesc.VariantTag = VH_VARIANT_FLOAT;
        OutDesc.Reject = VH_EXPORT_OK;
        // Plain `float` reports an infinite minimum and a NaN maximum. Neither is finite, which is
        // the whole test -- and the reason it is asked of each bound rather than of the type.
        //
        // A strict bound needs no special case: `_X < 500.0` is the double below 500.0, and the
        // consumer rounds inward to its own step, which lands under 500 from either spelling.
        const CFloatType& FloatType = Declared.Normal->AsChecked<CFloatType>();
        OutDesc.bHasRangeMin = FMath::IsFinite(FloatType.GetMin());
        OutDesc.bHasRangeMax = FMath::IsFinite(FloatType.GetMax());
        OutDesc.RangeMin = OutDesc.bHasRangeMin ? FloatType.GetMin() : 0.0;
        OutDesc.RangeMax = OutDesc.bHasRangeMax ? FloatType.GetMax() : 0.0;
        if (OutDesc.bHasRangeMin || OutDesc.bHasRangeMax)
        {
            OutDesc.Hint = VH_EXPORT_HINT_RANGE;
        }
        return;
    }

    case EDeclaredKind::Char:
        OutDesc.Type = VH_TYPE_CHAR;
        return;

    case EDeclaredKind::String:
        OutDesc.Type = VH_TYPE_STRING;
        OutDesc.VariantTag = VH_VARIANT_STRING;
        OutDesc.Reject = VH_EXPORT_OK;
        return;

    case EDeclaredKind::Array:
        OutDesc.Type = VH_TYPE_ARRAY;
        DescribeArrayElement(Declared.Element, Program, OutDesc);
        return;

    case EDeclaredKind::Map:
        OutDesc.Type = VH_TYPE_MAP;
        return;

    case EDeclaredKind::Tuple:
        OutDesc.Type = VH_TYPE_TUPLE;
        return;
    }
    VH_EXHAUSTIVE_SWITCH_END
}

AUTORTFM_DISABLE void DescribeExportType(const uLang::CTypeBase* Type, const uLang::CSemanticProgram& Program, GodotVerse::FExportDesc& OutDesc)
{
    DescribeExportTypeOf(ClassifyDeclaredType(Type, Program), Program, OutDesc);
}

AUTORTFM_DISABLE void DescribeClassOf(const FMemberType& Type, const uLang::CSemanticProgram& Program,
                                     FUtf8String& OutName, int32& OutKind)
{
    OutName.Reset();
    OutKind = VH_CLASS_NONE;
    // ReferenceName is the test for "is this a reference"; the pointer beside it is the
    // analysis's, and both answers below are read off it. A host with no semantic program never
    // reaches here at all -- it reads its descriptions out of a sidecar, already resolved.
    if (Type.ReferenceName.IsEmpty() || Type.ReferenceClass == nullptr)
    {
        return;
    }
    if (Type.ReferenceOrigin == EClassOrigin::Script && RegistersWithGodot(*Type.ReferenceClass, Program))
    {
        OutName = QualifiedNameOf(*Type.ReferenceClass);
        OutKind = VH_CLASS_SCRIPT;
        return;
    }
    // A mirrored class answers itself here, so this is the same call for both remaining cases.
    OutName = NativeClassOf(*Type.ReferenceClass, Program);
    OutKind = OutName.IsEmpty() ? VH_CLASS_NONE : VH_CLASS_MIRRORED;
}

AUTORTFM_DISABLE FMemberType DescribeType(const uLang::CTypeBase* Type, const uLang::CSemanticProgram& Program)
{
    const FDeclaredType Declared = ClassifyDeclaredType(Type, Program);

    FMemberType Result;
    Result.Kind = Declared.Kind;
    DescribeExportTypeOf(Declared, Program, Result.Described);

    // A handle to build a wrapper from. An option around any class but `variant` and `rid` is
    // described as one, whatever the class is, because that is where the option/bare distinction
    // the converters read lives.
    const auto DescribeReference = [&Result, &Declared](bool bIsOption) {
        Result.ReferenceClass = Declared.Class;
        Result.ReferenceName = FUtf8String(Declared.Class->AsNameCString());
        Result.ReferenceQualifiedName = QualifiedNameOf(*Declared.Class);
        Result.ReferenceOrigin = Declared.Origin;
        Result.bReferenceIsOption = bIsOption;
    };
    const auto DescribeEnumeration = [&Result](const uLang::CEnumeration& Enumeration) {
        for (const uLang::TSRef<uLang::CEnumerator>& Enumerator : Enumeration.GetDefinitionsOfKind<uLang::CEnumerator>())
        {
            (void)Enumerator;
            ++Result.EnumeratorCount;
        }
        Result.EnumerationName = FUtf8String(UTF8TEXT("("))
            + FULangConversionUtils::ULangStrToFUtf8String(
                  Enumeration._EnclosingScope.GetScopePath('/', uLang::CScope::EPathMode::PrefixSeparator))
            + UTF8TEXT(":)") + FUtf8String(Enumeration.AsNameCString());
    };

    VH_EXHAUSTIVE_SWITCH_BEGIN
    switch (Declared.Kind)
    {
    case EDeclaredKind::Reference:
        DescribeReference(Declared.bIsOption);
        break;
    // Not a Godot object, and still a class the converters build a wrapper for: a `signal(t)`
    // member's payload is read off the class recorded here.
    case EDeclaredKind::OtherClass:
        DescribeReference(false);
        break;

    case EDeclaredKind::MathStruct:
        Result.Struct = Declared.Layout;
        Result.StructName = FUtf8String(Declared.Class->AsNameCString());
        break;

    // A struct the project declared. Not a reference -- it is a value, and calling it one was what
    // sent a struct-typed parameter down the handle path to be refused there.
    case EDeclaredKind::UserStruct:
        Result.UserStruct = MakeShared<FUserStructLayout>();
        Result.UserStruct->DecoratedName = DecoratedNameOf(*Declared.Class);
        CollectStructFields(*Declared.Class, Program, *Result.UserStruct);
        break;

    case EDeclaredKind::Enum:
        DescribeEnumeration(*Declared.Enumeration);
        break;

    case EDeclaredKind::Option:
        VH_EXHAUSTIVE_SWITCH_BEGIN
        switch (Declared.OptionOf)
        {
        case EDeclaredKind::MathStruct:
        case EDeclaredKind::UserStruct:
        case EDeclaredKind::Container:
        case EDeclaredKind::TypedContainer:
        case EDeclaredKind::OtherClass:
            DescribeReference(true);
            break;
        case EDeclaredKind::Enum:
            DescribeEnumeration(*Declared.Enumeration);
            break;
        // `variant` and `rid` are neither a shape nor a handle, optional or not: leaving either to
        // the reference arm had every parameter of it refused as a handle to a class Godot has
        // never heard of ("Cannot convert argument 2 from RID to RID").
        case EDeclaredKind::Variant:
        case EDeclaredKind::Rid:
        case EDeclaredKind::Reference:
        case EDeclaredKind::Option:
        case EDeclaredKind::Other:
        case EDeclaredKind::Logic:
        case EDeclaredKind::Int:
        case EDeclaredKind::Float:
        case EDeclaredKind::Char:
        case EDeclaredKind::String:
        case EDeclaredKind::Array:
        case EDeclaredKind::Map:
        case EDeclaredKind::Tuple:
            break;
        }
        VH_EXHAUSTIVE_SWITCH_END
        break;

    // Nothing beyond what the export description already says: `variant` and `rid` are neither a
    // shape to read fields off nor a handle to build a wrapper from, and a container's id is the
    // whole of its value.
    case EDeclaredKind::Variant:
    case EDeclaredKind::Rid:
    case EDeclaredKind::Container:
    case EDeclaredKind::TypedContainer:
    case EDeclaredKind::Other:
    case EDeclaredKind::Logic:
    case EDeclaredKind::Int:
    case EDeclaredKind::Float:
    case EDeclaredKind::Char:
    case EDeclaredKind::String:
    case EDeclaredKind::Array:
    case EDeclaredKind::Map:
    case EDeclaredKind::Tuple:
        break;
    }
    VH_EXHAUSTIVE_SWITCH_END
    return Result;
}

AUTORTFM_DISABLE void CollectStructFields(const uLang::CClass& Struct,
                                          const uLang::CSemanticProgram& Program,
                                          FUserStructLayout& OutLayout)
{
    // Base first and the whole chain, the way GetClassSignals walks a script class: a struct that
    // extends another carries the base's fields, and those are fields the *value* has, so leaving
    // them out would mis-align every field after them.
    TArray<const uLang::CClass*> Chain;
    for (const uLang::CClass* Cursor = &Struct; Cursor != nullptr && Cursor->IsStruct();
         Cursor = Cursor->GetSuperClass())
    {
        Chain.Insert(Cursor, 0);
    }

    for (const uLang::CClass* Link : Chain)
    {
        for (const uLang::TSRef<uLang::CDataDefinition>& Field : Link->GetDefinitionsOfKind<uLang::CDataDefinition>())
        {
            OutLayout.FieldNames.Add(FUtf8String(Field->AsNameCString()));
            OutLayout.FieldKeys.Add(DecoratedNameOf(*Field));
            OutLayout.FieldTypes.Add(DescribeType(Field->GetType(), Program));
        }
    }
}

AUTORTFM_DISABLE const uLang::CClass* PayloadStructClass(const FDeclaredType& Payload)
{
    VH_EXHAUSTIVE_SWITCH_BEGIN
    switch (Payload.Kind)
    {
    case EDeclaredKind::UserStruct:
        return Payload.Class;
    // Structs, and each one Godot value: decomposed, a `signal(variant)` delivered its 22 lanes and
    // a `signal(rid)` its `Id` as a bare int.
    case EDeclaredKind::Variant:
    case EDeclaredKind::Rid:
    case EDeclaredKind::MathStruct:
    case EDeclaredKind::Option:
    case EDeclaredKind::Reference:
    case EDeclaredKind::OtherClass:
    case EDeclaredKind::Container:
    case EDeclaredKind::TypedContainer:
    case EDeclaredKind::Other:
    case EDeclaredKind::Logic:
    case EDeclaredKind::Int:
    case EDeclaredKind::Float:
    case EDeclaredKind::Char:
    case EDeclaredKind::String:
    case EDeclaredKind::Enum:
    case EDeclaredKind::Array:
    case EDeclaredKind::Map:
    case EDeclaredKind::Tuple:
        return nullptr;
    }
    VH_EXHAUSTIVE_SWITCH_END
    return nullptr;
}

namespace {

/// The argument name Godot is told, for a payload that carries no names of its own.
///
/// Verse tuples cannot name their elements -- `tuple(Damage:int, ...)` is "Expected a type, got
/// data definition instead" -- so a tuple payload gets positional names and a bare one is named
/// for its type, which is what the connect dialog and `_make_function` then write. A struct payload
/// is the spelling that *does* carry names, and never reaches here.
AUTORTFM_DISABLE FUtf8String SignalArgName(const FMemberType& Arg, int32 Index, bool bIsTuple)
{
    if (bIsTuple)
    {
        return FUtf8String(UTF8TEXT("Arg")) + FUtf8String::FromInt(Index);
    }
    switch (Arg.Described.Type)
    {
    case VH_TYPE_LOGIC:  return FUtf8String(UTF8TEXT("Logic"));
    case VH_TYPE_INT:    return FUtf8String(UTF8TEXT("Int"));
    case VH_TYPE_FLOAT:  return FUtf8String(UTF8TEXT("Float"));
    case VH_TYPE_STRING: return FUtf8String(UTF8TEXT("Text"));
    case VH_TYPE_ARRAY:  return FUtf8String(UTF8TEXT("Items"));
    case VH_TYPE_REF:    return FUtf8String(UTF8TEXT("Ref"));
    default:             return FUtf8String(UTF8TEXT("Value"));
    }
}

/// Whether the wire can carry one payload argument of this declared type, and it is deliberately
/// *not* `Described.Reject == VH_EXPORT_OK`.
///
/// Four of the export rejections are rules about the inspector rather than about the wire, and a
/// signal argument is subject to none of them:
///
///   - VH_EXPORT_OBJECT_NOT_OPTIONAL is "the inspector can leave a slot empty". Nothing leaves a
///     signal argument empty -- the emitter supplies it -- and ValueToWire has the bare-object
///     branch for exactly this case, added when `signal(node2d)` emitted nothing.
///   - VH_EXPORT_SCRIPT_CLASS_NOT_GLOBAL and VH_EXPORT_BINDING_CLASS_UNSUPPORTED are both
///     "ClassDB cannot filter a picker by this name" -- the latter for a generated-binding class
///     rather than an unregistered project one. An emission carries a handle; nobody filters
///     anything, and DeclaredReferenceClass's FindBindingClass arm resolves the class the same way
///     an ordinary method argument does.
///   - VH_EXPORT_UNSUPPORTED_TYPE over a *reference* wrapper, a `variant` or a `rid` is "the
///     inspector has no editor for this". Each crosses perfectly well as one Godot value, which is
///     why DescribeExportType types it before rejecting it.
///
/// What is left really is unrepresentable: an option around a non-object (ValueToWire reads a
/// cleared option as a null reference, so `?int` would arrive as nothing), and a type with no lane.
AUTORTFM_DISABLE bool PayloadArgCrosses(const FMemberType& Arg)
{
    VH_EXHAUSTIVE_SWITCH_BEGIN
    switch (static_cast<vh_export_reject>(Arg.Described.Reject))
    {
    case VH_EXPORT_OK:
    case VH_EXPORT_OBJECT_NOT_OPTIONAL:
    case VH_EXPORT_SCRIPT_CLASS_NOT_GLOBAL:
    case VH_EXPORT_BINDING_CLASS_UNSUPPORTED:
        return true;
    case VH_EXPORT_UNSUPPORTED_TYPE:
        return Arg.Kind == EDeclaredKind::Container || Arg.Kind == EDeclaredKind::Variant
            || Arg.Kind == EDeclaredKind::Rid;
    case VH_EXPORT_OPTION_NOT_OBJECT:
    case VH_EXPORT_HINT_WRONG_TYPE:
        return false;
    }
    VH_EXHAUSTIVE_SWITCH_END
    return false;
}

} // namespace

AUTORTFM_DISABLE const uLang::CTypeBase* SignalPayloadType(const uLang::CClass& Declared)
{
    for (const uLang::STypeVariableSubstitution& Substitution : Declared._TypeVariableSubstitutions)
    {
        if (Substitution._PositiveType)
        {
            return Substitution._PositiveType;
        }
    }
    return nullptr;
}

AUTORTFM_DISABLE void DescribePayload(const uLang::CTypeBase* Payload,
                                      const uLang::CSemanticProgram& Program,
                                      FPayloadShape& OutShape)
{
    OutShape = FPayloadShape{};
    if (!Payload)
    {
        return;
    }

    const auto AddArg = [&OutShape](FUtf8String Name, FUtf8String FieldKey, FMemberType Type) {
        FPayloadArg& Arg = OutShape.Args.AddDefaulted_GetRef();
        Arg.Name = MoveTemp(Name);
        Arg.FieldKey = MoveTemp(FieldKey);
        Arg.Type = MoveTemp(Type);
    };

    const FDeclaredType Declared = ClassifyDeclaredType(Payload, Program);

    // The payload as one value, for the direction that has to reassemble it. A tuple has no
    // description of its own -- DescribeType would answer "nothing" for it -- so Kind and Args are
    // what the tuple case is rebuilt from and this is only read for Bare and Struct.
    OutShape.Whole = DescribeType(Payload, Program);

    if (const uLang::CTupleType* Tuple = Declared.Normal->AsNullable<uLang::CTupleType>())
    {
        OutShape.Kind = EPayloadShape::Tuple;
        for (const uLang::CTypeBase* Element : Tuple->GetElements())
        {
            FMemberType Described = DescribeType(Element, Program);
            AddArg(SignalArgName(Described, OutShape.Args.Num(), true), FUtf8String(), MoveTemp(Described));
        }
    }
    else if (const uLang::CClass* const Struct = PayloadStructClass(Declared))
    {
        OutShape.Kind = EPayloadShape::Struct;
        OutShape.StructClass = Struct;

        // The same walk the inbound direction uses, so the order Godot is told the arguments come
        // in and the order they are read back cannot disagree.
        FUserStructLayout Layout;
        Layout.DecoratedName = DecoratedNameOf(*Struct);
        CollectStructFields(*Struct, Program, Layout);

        for (int32 Index = 0; Index < Layout.FieldNames.Num(); ++Index)
        {
            if (Layout.FieldTypes[Index].UserStruct.IsValid())
            {
                // One level, and no more. Godot has no argument shape for "a struct", so the second
                // level has nothing to decompose into and silently dropping it would be the
                // accepted-but-broken failure this whole pass exists to remove.
                OutShape.Reject = VH_SIGNAL_PAYLOAD_NESTED_STRUCT;
                OutShape.RejectDetail = Layout.FieldNames[Index];
                return;
            }
            AddArg(Layout.FieldNames[Index], Layout.FieldKeys[Index], Layout.FieldTypes[Index]);
        }
    }
    else
    {
        OutShape.Kind = EPayloadShape::Bare;
        FMemberType Described = DescribeType(Payload, Program);
        AddArg(SignalArgName(Described, 0, false), FUtf8String(), MoveTemp(Described));
    }

    for (const FPayloadArg& Arg : OutShape.Args)
    {
        if (!PayloadArgCrosses(Arg.Type))
        {
            OutShape.Reject = VH_SIGNAL_PAYLOAD_UNSUPPORTED;
            OutShape.RejectDetail = Arg.Name;
            return;
        }
    }
}

} // namespace GodotVerse
