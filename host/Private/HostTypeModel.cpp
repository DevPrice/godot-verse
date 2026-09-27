// Copyright Epic Games, Inc. All Rights Reserved.

#include "HostTypeModel.h"

// Before the generated layout, which names the ABI's variant tags without including it.
#include "HostScript.h"
#include "GodotMathLayout.gen.h"
#include "ULangUEUtils.h"
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
    return EClassOrigin::Other;
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

} // namespace GodotVerse
