// Copyright Epic Games, Inc. All Rights Reserved.

#include "HostSnapshot.h"
#include "HostBuild.h"
#include "HostScript.h"
#include "HostCallbacks.h"
#include "HostEngineAdapters.h"
#include "HostInstances.h"
#include "HostMarshal.h"
#include "HostPeers.h"
#include "HostScriptState.h"
#include "HostSignals.h"
#include "HostTypeModel.h"
#include "HostVerseEntry.h"
#include "AutoRTFM.h"
#include "Containers/Map.h"
#include "Containers/UnrealString.h"
#include "GodotClasses.h"
#include "GodotClassNames.gen.h"
#include "GodotMathLayout.gen.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformTime.h"
#include "Misc/Paths.h"
#include "HostDebug.h"
#include "HostEventLoop.h"
#include "HostRuntime.h"
#include "ISolarisIde.h"
#include "ISolarisModule.h"
#include "IVerseModule.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "HostSidecar.h"
#include "Dom/JsonValue.h"
#include "HAL/IConsoleManager.h"
#include "Misc/FileHelper.h"
#include "Modules/ModuleManager.h"
#include "SolBuildDiagnostic.h"
#include "Templates/Function.h"
#include "TestUtils/PlaceholderObjectForContentScope.h"
#include "ULangUEUtils.h"
#include "VerseComputationLimitControl.h"
#include "VerseContentScope.h"
#include "VerseEvent.h"
#include "VerseString.h"
#include "VerseTask.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectHash.h"
#include "VerseVM/Inline/VVMRefInline.h"
#include "VerseVM/Inline/VVMValueInline.h"
#include "VerseVM/Inline/VVMValueObjectInline.h"
#include "VerseVM/Inline/VVMVerseClassInline.h"
#include "VerseVM/VVMArray.h"
#include "VerseVM/VVMMutableArray.h"
#include "VerseVM/VVMShape.h"
#include "VerseVM/VVMNativeRef.h"
#include "VerseVM/VVMRef.h"
#include "VerseVM/VVMRestValue.h"
#include "VerseVM/VVMClass.h"
#include "VerseVM/VVMCoroutine.h"
#include "VerseVM/Inline/VVMEnumerationInline.h"
#include "VerseVM/VVMEnumerator.h"
#include "VerseVM/VVMFalse.h"
#include "VerseVM/VVMOption.h"
#include "VerseVM/VVMInt.h"
#include "VerseVM/VVMFloat.h"
#include "VerseVM/VVMValueObject.h"
#include "VerseVM/VVMOpResult.h"
#include "VerseVM/VVMVerseClass.h"
#include "VerseVM/VVMGlobalProgram.h"
#include "VerseVM/VVMNativeConverter.h"
#include "VerseVM/VVMNativeFunction.h"
#include "VerseVM/VVMNativeStruct.h"
#include "VerseVM/VVMNamedType.h"
#include "VerseVM/VVMPackage.h"
#include "VerseVM/VVMProgram.h"
#include "VerseVM/VVMContext.h"
#include "VerseVM/VVMTaskGroup.h"
#include "VerseVM/VVMUniqueString.h"
#include "uLang/Diagnostics/Diagnostics.h"
#include "uLang/Semantics/Attributable.h"
#include "uLang/Semantics/DataDefinition.h"
#include "uLang/Semantics/Definition.h"
#include "uLang/Semantics/Expression.h"
#include "uLang/Semantics/FilteredDefinitionRange.h"
#include "uLang/Semantics/ModuleAlias.h"
#include "uLang/Semantics/SemanticClass.h"
#include "uLang/Semantics/SemanticEnumeration.h"
#include "uLang/Semantics/SemanticFunction.h"
#include "uLang/Semantics/SemanticProgram.h"
#include "uLang/Semantics/SemanticTypes.h"
#include "uLang/Semantics/TypeAlias.h"
#include "uLang/Syntax/VstNode.h"
#include "uLang/SourceProject/PackageRole.h"
#include "uLang/SourceProject/SourceDataProject.h"
#include "uLang/SourceProject/UploadedAtFNVersion.h"
#include "uLang/SourceProject/VerseScope.h"
#include "uLang/SourceProject/VerseVersion.h"
#include "uLang/CompilerPasses/ApiLayerInjections.h"
#include "uLang/CompilerPasses/IParserPass.h"
#include "uLang/Toolchain/ModularFeatureManager.h"
#include "uLang/Toolchain/ProgramBuildManager.h"

#include <atomic>
#include <cstdio>
#include <thread>

using GodotVerse::AnalysisTraceEnabled;
using GodotVerse::BindingsVersePath;
using GodotVerse::ClassChainOfOrigin;
using GodotVerse::ClassMembersLive;
using GodotVerse::ClassOverrideCandidatesLive;
using GodotVerse::ClassOwnMembers;
using GodotVerse::ClassifyDeclaredType;
using GodotVerse::ClassOriginOf;
using GodotVerse::CollectStructFields;
using GodotVerse::DescribeClassOf;
using GodotVerse::DescribeExportType;
using GodotVerse::DescribeMemberType;
using GodotVerse::DescribePayload;
using GodotVerse::DescribeType;
using GodotVerse::EClassOrigin;
using GodotVerse::EDeclaredKind;
using GodotVerse::EPayloadShape;
using GodotVerse::EnterVerse;
using GodotVerse::FVerseEntry;
using GodotVerse::DecoratedNameOf;
using GodotVerse::ExtensionMethodDecoratedName;
using GodotVerse::FCallbackTarget;
using GodotVerse::FDeclaredType;
using GodotVerse::FillLocation;
using GodotVerse::FindBindingClass;
using GodotVerse::FindGodotClass;
using GodotVerse::FindMirroredClass;
using GodotVerse::FindMirroredVClass;
using GodotVerse::FindReferenceClass;
using GodotVerse::FindScriptClassLive;
using GodotVerse::FindStructLayout;
using GodotVerse::FMemberType;
using GodotVerse::FMethodSignatureTypes;
using GodotVerse::ForgetCachedClasses;
using GodotVerse::ForgetMirrorDefinitions;
using GodotVerse::FPayloadArg;
using GodotVerse::FPayloadShape;
using GodotVerse::FStructLayout;
using GodotVerse::FUserStructLayout;
using GodotVerse::GodotVersePath;
using GodotVerse::HasIde;
using GodotVerse::IdeBuildManager;
using GodotVerse::PublishedScriptPackageName;
using GodotVerse::MirrorDefinitionsRecorded;
using GodotVerse::NativeClassOf;
using GodotVerse::NearestAncestorOfOrigin;
using GodotVerse::NewArrayValue;
using GodotVerse::NewDefaultsObject;
using GodotVerse::NewReferenceWrapper;
using GodotVerse::NewStructValue;
using GodotVerse::PayloadStructClass;
using GodotVerse::QualifiedNameOf;
using GodotVerse::ReadDefaultFieldOf;
using GodotVerse::ReferenceOption;
using GodotVerse::ValueToWire;
using GodotVerse::WireToValue;
using GodotVerse::RecordMirrorDefinitions;
using GodotVerse::ScriptVersePath;
using GodotVerse::SignalPayloadType;
using GodotVerse::SubjectTypeOfDiagnostic;
using GodotVerse::UnwrapDeclaredType;

namespace {

/// Declared in HostSnapshot.h since the sidecar and the readers in other units name it too.
/// Everything below was written when it was a file-local type, and still reads that way.
using GodotVerse::FAnalysisSnapshot;

/// What the read entry points answer from, and what the next analysis is building. Swapped on the
/// game thread; the current one is shared rather than double-buffered by index, so a descriptor
/// handed out over the ABI keeps its bytes alive for as long as its holder keeps its share.
TSharedPtr<const FAnalysisSnapshot> GSnapshot;
TSharedPtr<FAnalysisSnapshot> GPendingSnapshot;

/// Records every name Module declares against Module's own path, then recurses. Path is relative
/// to the package root, and an empty one is the root module itself -- which is skipped, because it
/// is in scope from everywhere and so is never the answer to "what should I import".
///
/// Built once per analysis and inverted, where ResolveUnknownName used to walk the AST per query:
/// the walk reads uLang's AST project, which the background worker rebuilds under it, so asking at
/// query time was a race as well as a cost.
AUTORTFM_DISABLE void CollectModuleDeclarations(const uLang::CModule& Module,
                                                const FUtf8String& Path,
                                                TMap<FUtf8String, TArray<FUtf8String>>& Out)
{
    if (!Path.IsEmpty())
    {
        for (const uLang::TSRef<uLang::CDefinition>& Definition : Module.GetDefinitions())
        {
            Out.FindOrAdd(FUtf8String(Definition->AsNameCString())).AddUnique(Path);
        }
    }

    for (const uLang::CModule* Submodule : Module.GetDefinitionsOfKind<uLang::CModule>())
    {
        const FUtf8String Name8 = FUtf8String(Submodule->AsNameCString());
        CollectModuleDeclarations(*Submodule, Path.IsEmpty() ? Name8 : Path + UTF8TEXT("/") + Name8, Out);
    }
}

} // namespace

AUTORTFM_DISABLE bool GodotVerse::ResolveUnknownName(FUtf8StringView Name, TArray<FUtf8String>& OutModules)
{
    OutModules.Empty();
    if (!GSnapshot || !GSnapshot->bAstAvailable || Name.IsEmpty())
    {
        return false;
    }
    if (const TArray<FUtf8String>* const Modules = GSnapshot->ModulesDeclaring.Find(FUtf8String(Name)))
    {
        OutModules = *Modules;
    }
    return true;
}

AUTORTFM_DISABLE bool GodotVerse::HasClass(FUtf8StringView ClassName)
{
    if (const TResult<const FAnalysisSnapshot::FClass*> Found = FindSnapshotClass(ClassName))
    {
        return Found.GetValue()->bInPublishedProgram;
    }

    // A name the last analysis did not declare is not the same as one the published generation does
    // not carry: renaming a class in an unsaved buffer leaves the old name in the VM and takes it
    // out of the program. Asking the VM is the honest answer, and it is a lookup -- but it is an
    // entry into the VM, which a running build forbids.
    return !IsBackgroundCheckRunning() && FindGodotClass(ClassName) != nullptr;
}

namespace {
/// The bridge's own attributes, declared in HostBuild.cpp's AttributePackageSource and so sharing
/// the verse path a script already imports. The section ones name the attribute *class* rather
/// than the `<constructor>` function beside it: GetAttributeTextValue matches on the invocation's
/// return type, and a single string argument is the one attribute payload SOL-972 leaves readable.
constexpr const char* ExportAttributePath = "/Godot.org/Godot/export";
constexpr const char* ExportSignalAttributePath = "/Godot.org/Godot/export_signal";
constexpr const char* StaticsAttributePath = "/Godot.org/Godot/statics_attribute";
constexpr const char* RpcAttributePath = "/Godot.org/Godot/rpc_attribute";
constexpr const char* ExportFileAttributePath = "/Godot.org/Godot/export_file_attribute";
constexpr const char* ExportDirAttributePath = "/Godot.org/Godot/export_dir";
constexpr const char* ExportMultilineAttributePath = "/Godot.org/Godot/export_multiline";
constexpr const char* ExportFlagsAttributePath = "/Godot.org/Godot/export_flags_attribute";
constexpr const char* ExportNodePathAttributePath = "/Godot.org/Godot/export_node_path_attribute";
constexpr const char* ExportCategoryAttributePath = "/Godot.org/Godot/export_category_attribute";
constexpr const char* ExportGroupAttributePath = "/Godot.org/Godot/export_group_attribute";
constexpr const char* ExportSubgroupAttributePath = "/Godot.org/Godot/export_subgroup_attribute";

/// The string a single-argument metadata attribute was spelled with, or empty when the member
/// does not carry it.
AUTORTFM_DISABLE FUtf8String AttributeText(const uLang::CDataDefinition& Member, const uLang::CClass* AttributeClass, const uLang::CSemanticProgram& Program)
{
    if (!AttributeClass)
    {
        return FUtf8String();
    }
    const uLang::TOptional<uLang::CUTF8String> Text = Member.GetAttributes().GetAttributeTextValue(AttributeClass, Program);
    return Text.IsSet() ? FULangConversionUtils::ULangStrToFUtf8String(*Text) : FUtf8String();
}

} // namespace

namespace {

/// The class's recorded tables, or null. What every runtime lookup falls back to, and the whole of
/// what a host with no semantic program has.
AUTORTFM_DISABLE const GodotVerse::FDeclaredTypes* RecordedTypes(FUtf8StringView ClassName);

} // namespace

AUTORTFM_DISABLE FMemberType GodotVerse::DescribeMemberType(FUtf8StringView ClassName, FUtf8StringView FieldName)
{
    FMemberType Result;
    if (!HasIde())
    {
        // A runtime host's whole answer. The table was recorded by the cook's own analysis and read
        // back out of the sidecar; without it every field read and write in an exported game is a
        // description of nothing, which reads as "wrong type" rather than as "no compiler".
        if (const GodotVerse::FDeclaredTypes* const Recorded = RecordedTypes(ClassName))
        {
            if (const FMemberType* const Found = Recorded->Members.Find(FUtf8String(FieldName)))
            {
                return *Found;
            }
        }
        return Result;
    }
    const uLang::TSPtr<uLang::CProgramBuildManager> BuildManager = IdeBuildManager();
    if (!BuildManager.IsValid())
    {
        return Result;
    }
    const uLang::TSRef<uLang::CSemanticProgram>& Program = BuildManager->GetProgramContext()._Program;

    const FUtf8String ClassPath = FUtf8String(ScriptVersePath) + UTF8TEXT("/") + FUtf8String(ClassName);
    const uLang::CClass* Class = Program->FindDefinitionByVersePath<uLang::CClass>(
        FULangConversionUtils::FUtf8StringViewToULangStringView(ClassPath));
    if (!Class)
    {
        return Result;
    }

    // Up the chain, because one script class may derive from another and the member may be the
    // base's. The walk stops at the first class outside the script package: above that is generated
    // API, whose members are Godot's own properties rather than script state.
    const TArray<const uLang::CClass*> Chain = ClassChainOfOrigin(*Class, *Program, EClassOrigin::Script);
    for (int32 Link = Chain.Num() - 1; Link >= 0; --Link)
    {
        for (const uLang::TSRef<uLang::CDataDefinition>& Member : Chain[Link]->GetDefinitionsOfKind<uLang::CDataDefinition>())
        {
            if (!FUtf8StringView(Member->AsNameCString()).Equals(FieldName))
            {
                continue;
            }
            Result = DescribeType(Member->GetType(), *Program);
            Result.Member = &*Member;
            Result.bIsVar = Member->IsVar();
            return Result;
        }
    }
    return Result;
}

AUTORTFM_DISABLE const TMap<FUtf8String, FPayloadShape>* GodotVerse::RecordedSignalShapes(FUtf8StringView ClassName)
{
    const FDeclaredTypes* const Recorded = RecordedTypes(ClassName);
    return Recorded ? &Recorded->Signals : nullptr;
}

AUTORTFM_DISABLE bool GodotVerse::RecordedMethodTypes(FUtf8StringView ClassName,
                                                      FUtf8StringView DecoratedName,
                                                      TArray<FMemberType>& OutParams,
                                                      FMemberType& OutResult)
{
    if (const GodotVerse::FDeclaredTypes* const Recorded = RecordedTypes(ClassName))
    {
        if (const FMethodSignatureTypes* const Signature = Recorded->Methods.Find(FUtf8String(DecoratedName)))
        {
            OutParams = Signature->Params;
            OutResult = Signature->Result;
            return true;
        }
    }
    return false;
}

namespace {

/// Godot's name for the virtual a Verse method of this name overrides: `Ready` -> `_ready`,
/// `PhysicsProcess` -> `_physics_process`.
///
/// The exact inverse of the rule gen_verse_api.py names a mirrored virtual by, which is what makes
/// this a rule rather than a table: every virtual the generator emits round-trips through it, so a
/// virtual added by a future Godot version needs no change here.
AUTORTFM_DISABLE FUtf8String GodotVirtualNameOf(FUtf8StringView VerseName)
{
    FUtf8String Result;
    for (int32 Index = 0; Index < VerseName.Len(); ++Index)
    {
        const UTF8CHAR Char = VerseName[Index];
        if (Char >= UTF8CHAR('A') && Char <= UTF8CHAR('Z'))
        {
            // One separator, never two: since Phase 4 a virtual is spelled `_Ready`, so the
            // underscore Godot's own name starts with is already in hand and adding another would
            // ask Godot about `__ready`.
            if (Result.IsEmpty() || Result[Result.Len() - 1] != UTF8CHAR('_'))
            {
                Result.AppendChar(UTF8CHAR('_'));
            }
            Result.AppendChar(UTF8CHAR(Char - 'A' + 'a'));
        }
        else
        {
            Result.AppendChar(Char);
        }
    }
    return Result;
}

/// Whether the definition this function ultimately overrides was declared in the generated Godot
/// mirror, which is what makes it one of Godot's virtuals rather than the script's own method.
AUTORTFM_DISABLE bool OverridesMirroredDefinition(const uLang::CFunction& Function)
{
    const uLang::CFunction* Base = Function.GetBaseOverriddenDefinition().GetPrototypeDefinition();
    if (!Base || Base == &Function)
    {
        return false;
    }
    const FUtf8String ScopePath = FULangConversionUtils::ULangStrToFUtf8String(
        Base->_EnclosingScope.GetScopePath('/', uLang::CScope::EPathMode::PrefixSeparator));
    return FUtf8StringView(ScopePath).StartsWith(FUtf8StringView(GodotVersePath));
}

} // namespace

namespace {

AUTORTFM_DISABLE bool GetClassMethodsLive(FUtf8StringView ClassName, TArray<GodotVerse::FMethodDesc>& OutMethods)
{
    using GodotVerse::FMethodDesc;
    using GodotVerse::FParamDesc;

    OutMethods.Reset();

    if (!HasIde())
    {
        return false;
    }
    const uLang::TSPtr<uLang::CProgramBuildManager> BuildManager = IdeBuildManager();
    if (!BuildManager.IsValid())
    {
        return false;
    }
    const uLang::TSRef<uLang::CSemanticProgram>& Program = BuildManager->GetProgramContext()._Program;

    const FUtf8String ClassPath = FUtf8String(ScriptVersePath) + UTF8TEXT("/") + FUtf8String(ClassName);
    const uLang::CClass* Class = Program->FindDefinitionByVersePath<uLang::CClass>(
        FULangConversionUtils::FUtf8StringViewToULangStringView(ClassPath));
    if (!Class)
    {
        return false;
    }

    // Only the class's own declarations. What it inherits from the mirrored API is Godot's to
    // dispatch, not the script's -- and an override *is* a declaration, so a script's Ready is
    // here while the empty one it overrides is not.
    for (const uLang::TSRef<uLang::CFunction>& Function : Class->GetDefinitionsOfKind<uLang::CFunction>())
    {
        const uLang::CFunctionType* const Type = Function->_Signature.GetFunctionType();
        if (!Type)
        {
            continue;
        }

        FMethodDesc Desc;
        Desc.Name = FUtf8String(Function->AsNameCString());
        Desc.DecoratedName = FULangConversionUtils::ULangStrToFUtf8String(Function->GetDecoratedName());

        const uLang::SEffectSet Effects = Function->_Signature.GetEffects();
        Desc.bCanFail = Effects[uLang::EEffect::decides];
        Desc.bSuspends = Effects[uLang::EEffect::suspends];

        for (const uLang::CDataDefinition* Param : Function->_Signature.GetParams())
        {
            if (!Param)
            {
                continue;
            }
            const FMemberType ParamType = DescribeType(Param->GetType(), *Program);
            FParamDesc& Out = Desc.Params.AddDefaulted_GetRef();
            Out.Name = FUtf8String(Param->AsNameCString());
            Out.Type = ParamType.Described.Type;
            // A Verse array names no single Godot type: `[]float` is a PackedFloat32Array, a
            // PackedFloat64Array or a plain Array, and the converters take any of them. Reporting
            // one would have Godot refuse the other two before the call was ever made, so the
            // parameter is reported untyped and checked where it is converted instead.
            Out.VariantTag = ParamType.Described.Type == VH_TYPE_ARRAY
                ? VH_VARIANT_NIL
                : ParamType.Described.VariantTag;
            DescribeClassOf(ParamType, *Program, Out.ClassName, Out.ClassKind);
            Out.bHasDefault = Param->HasInitializer();
            if (!Out.bHasDefault)
            {
                Desc.RequiredParamCount = Desc.Params.Num();
            }
        }

        const FMemberType ResultType = DescribeType(&Type->GetReturnType(), *Program);
        Desc.ResultType = ResultType.Described.Type;
        Desc.ResultVariantTag = ResultType.Described.VariantTag;
        DescribeClassOf(ResultType, *Program, Desc.ResultClassName, Desc.ResultClassKind);

        if (OverridesMirroredDefinition(*Function))
        {
            Desc.GodotVirtual = GodotVirtualNameOf(FUtf8StringView(Desc.Name));
        }

        FUtf8String DeclaredIn;
        FillLocation(*Function, DeclaredIn, Desc.Line, Desc.Column);

        OutMethods.Add(MoveTemp(Desc));
    }
    return true;
}

AUTORTFM_DISABLE bool GetClassExportsLive(FUtf8StringView ClassName, TArray<GodotVerse::FExportDesc>& OutExports)
{
    using GodotVerse::FExportDesc;

    OutExports.Reset();

    if (!HasIde())
    {
        return false;
    }

    const uLang::TSPtr<uLang::CProgramBuildManager> BuildManager = IdeBuildManager();
    if (!BuildManager.IsValid())
    {
        return false;
    }
    const uLang::TSRef<uLang::CSemanticProgram>& Program = BuildManager->GetProgramContext()._Program;

    // Absent when the attribute package did not make it into the program, which also means no
    // script could have applied the attribute -- an empty list would claim the script exports
    // nothing, so this reports "cannot answer" instead.
    const uLang::CClass* ExportAttribute =
        Program->FindDefinitionByVersePath<uLang::CClass>(ExportAttributePath);
    if (!ExportAttribute)
    {
        return false;
    }

    const FUtf8String ClassPath = FUtf8String(ScriptVersePath) + UTF8TEXT("/") + FUtf8String(ClassName);
    const uLang::CClass* Class = Program->FindDefinitionByVersePath<uLang::CClass>(
        FULangConversionUtils::FUtf8StringViewToULangStringView(ClassPath));
    if (!Class)
    {
        return false;
    }

    // R-EXP-1's five, which say what a `string` or an `int` is *for* -- the one thing a declared
    // type cannot. Looked up once for the whole class rather than per member.
    struct FHintAttribute
    {
        const char* Path;
        int32 Hint;
        /// The vh_type the member must be declared as. An attribute that describes a path on an
        /// int describes nothing, and this is the only place that pairing can be checked.
        int32 RequiredType;
        /// Whether the attribute carries a hint string, or is a marker like `@tool`.
        bool bHasText;
    };
    const FHintAttribute HintAttributes[] = {
        {ExportFileAttributePath, VH_EXPORT_HINT_FILE, VH_TYPE_STRING, true},
        {ExportDirAttributePath, VH_EXPORT_HINT_DIR, VH_TYPE_STRING, false},
        {ExportMultilineAttributePath, VH_EXPORT_HINT_MULTILINE, VH_TYPE_STRING, false},
        {ExportFlagsAttributePath, VH_EXPORT_HINT_FLAGS, VH_TYPE_INT, true},
        {ExportNodePathAttributePath, VH_EXPORT_HINT_NODE_PATH, VH_TYPE_STRING, true},
    };

    const uLang::CClass* CategoryAttribute = Program->FindDefinitionByVersePath<uLang::CClass>(ExportCategoryAttributePath);
    const uLang::CClass* GroupAttribute = Program->FindDefinitionByVersePath<uLang::CClass>(ExportGroupAttributePath);
    const uLang::CClass* SubgroupAttribute = Program->FindDefinitionByVersePath<uLang::CClass>(ExportSubgroupAttributePath);

    // The class's own members *and* those of any script class it derives from, base first -- which
    // is the order an inspector draws them in, and the order a reader expects.
    //
    // The walk stops at the first class outside the script package, because everything above that is
    // generated API: a mirrored class' members are Godot's own properties, which Godot already draws
    // and which carry no @export. Before script-to-script inheritance was tested there was no class
    // between the two, and this loop read only the one class.
    const TArray<const uLang::CClass*> Chain = ClassChainOfOrigin(*Class, *Program, EClassOrigin::Script);

    TArray<const uLang::TSRef<uLang::CDataDefinition>> Members;
    for (const uLang::CClass* Link : Chain)
    {
        for (const uLang::TSRef<uLang::CDataDefinition>& Member : Link->GetDefinitionsOfKind<uLang::CDataDefinition>())
        {
            Members.Add(Member);
        }
    }

    for (const uLang::TSRef<uLang::CDataDefinition>& Member : Members)
    {
        if (!Member->HasAttributeSubclass(ExportAttribute, *Program))
        {
            continue;
        }

        FExportDesc Desc;
        Desc.Name = FUtf8String(Member->AsNameCString());
        Desc.bIsVar = Member->IsVar();
        DescribeExportType(Member->GetType(), *Program, Desc);

        // The outermost section a member opens wins, since a member cannot be the first of a group
        // and the first of the category above it at once -- and the deeper one would be drawn
        // inside whatever came before, which is not what naming a category asks for.
        const FUtf8String Category = AttributeText(*Member, CategoryAttribute, *Program);
        const FUtf8String Group = AttributeText(*Member, GroupAttribute, *Program);
        const FUtf8String Subgroup = AttributeText(*Member, SubgroupAttribute, *Program);
        if (Member->HasAttributeSubclass(CategoryAttribute, *Program))
        {
            Desc.GroupKind = VH_EXPORT_GROUP_CATEGORY;
            Desc.GroupName = Category;
        }
        else if (Member->HasAttributeSubclass(GroupAttribute, *Program))
        {
            Desc.GroupKind = VH_EXPORT_GROUP_GROUP;
            Desc.GroupName = Group;
        }
        else if (Member->HasAttributeSubclass(SubgroupAttribute, *Program))
        {
            Desc.GroupKind = VH_EXPORT_GROUP_SUBGROUP;
            Desc.GroupName = Subgroup;
        }

        // One of R-EXP-1's five, which *replaces* whatever the type implied: the whole reason to
        // write one is that the type said nothing useful. Applied after the type description rather
        // than before it, so that the range a bounded int would have got is the thing overridden
        // and not the other way round.
        //
        // Only the first is taken. Two of these on one member is two descriptions of one field and
        // Godot draws one, so the alternative is choosing silently between them.
        for (const FHintAttribute& Attribute : HintAttributes)
        {
            const uLang::CClass* const HintClass =
                Program->FindDefinitionByVersePath<uLang::CClass>(Attribute.Path);
            if (!HintClass || !Member->HasAttributeSubclass(HintClass, *Program))
            {
                continue;
            }

            // Recorded even when the type is wrong, because the consumer's message names the
            // attribute the author wrote and there is nowhere else to carry it.
            Desc.Hint = Attribute.Hint;
            Desc.HintString = Attribute.bHasText ? AttributeText(*Member, HintClass, *Program) : FUtf8String();
            if (Desc.Reject == VH_EXPORT_OK && Desc.Type != Attribute.RequiredType)
            {
                Desc.Reject = VH_EXPORT_HINT_WRONG_TYPE;
            }
            break;
        }

        FUtf8String DeclaredIn;
        FillLocation(*Member, DeclaredIn, Desc.Line, Desc.Column);

        OutExports.Add(MoveTemp(Desc));
    }
    return true;
}

} // namespace

AUTORTFM_DISABLE bool GodotVerse::GetClassMethods(FUtf8StringView ClassName, TArray<FMethodDesc>& OutMethods)
{
    OutMethods.Reset();
    const TResult<const FAnalysisSnapshot::FClass*> Described = FindSnapshotClass(ClassName);
    const FAnalysisSnapshot::FClass* const Found = Described ? Described.GetValue() : nullptr;
    if (!Found)
    {
        return false;
    }
    OutMethods = Found->Methods;
    return true;
}

AUTORTFM_DISABLE bool GodotVerse::GetClassExports(FUtf8StringView ClassName, TArray<FExportDesc>& OutExports)
{
    OutExports.Reset();
    const TResult<const FAnalysisSnapshot::FClass*> Described = FindSnapshotClass(ClassName);
    const FAnalysisSnapshot::FClass* const Found = Described ? Described.GetValue() : nullptr;
    if (!Found || !Found->bExportsHarvested)
    {
        return false;
    }
    OutExports = Found->Exports;
    return true;
}

namespace {

/// Whether a declared type is a `signal(...)`: a class whose chain reaches the native
/// `vh_signal`, which is the one thing every signal type has in common and the one thing a script
/// cannot accidentally be.
AUTORTFM_DISABLE bool IsSignalClass(const uLang::CClass& Declared)
{
    for (const uLang::CClass* Cursor = &Declared; Cursor != nullptr; Cursor = Cursor->GetSuperClass())
    {
        if (FUtf8StringView(Cursor->AsNameCString()).Equals(UTF8TEXT("vh_signal")))
        {
            return true;
        }
    }
    return false;
}

/// Whether a declared type is a `/Verse.org/Verse` `event(t)`: a class whose chain reaches
/// `event_base_intrnl`, which is the root Epic gave the event family for exactly this kind of test.
///
/// The `@export_signal` half of R-SIG-1. Keyed on the base rather than on `event` itself so a
/// subclass of one still counts, and on the *name* rather than on an interface: `listenable(t)` is
/// `awaitable` + `subscribable` and does not extend `signalable`, so it can neither be signalled
/// into nor heard out of (docs/signal-declaration.md 3).
AUTORTFM_DISABLE bool IsEventClass(const uLang::CClass& Declared)
{
    for (const uLang::CClass* Cursor = &Declared; Cursor != nullptr; Cursor = Cursor->GetSuperClass())
    {
        if (FUtf8StringView(Cursor->AsNameCString()).Equals(UTF8TEXT("event_base_intrnl")))
        {
            return true;
        }
    }
    return false;
}

} // namespace

AUTORTFM_DISABLE const TSharedPtr<const GodotVerse::FAnalysisSnapshot>& GodotVerse::GetAnalysisSnapshot()
{
    return GSnapshot;
}

AUTORTFM_DISABLE void GodotVerse::SetAnalysisSnapshot(TSharedRef<const GodotVerse::FAnalysisSnapshot> Snapshot)
{
    GSnapshot = Snapshot;
}

AUTORTFM_DISABLE GodotVerse::TResult<const GodotVerse::FAnalysisSnapshot::FClass*> GodotVerse::FindSnapshotClass(
    FUtf8StringView ClassName)
{
    if (!GSnapshot)
    {
        return EHostFailure::NotAnalysed;
    }
    const FAnalysisSnapshot::FClass* const Found = GSnapshot->Classes.Find(FUtf8String(ClassName));
    if (!Found)
    {
        return EHostFailure::NoSuchClass;
    }
    return Found;
}

AUTORTFM_DISABLE bool GodotVerse::IsClassAbstract(FUtf8StringView ClassName)
{
    const TResult<const FAnalysisSnapshot::FClass*> Described = FindSnapshotClass(ClassName);
    const FAnalysisSnapshot::FClass* const Found = Described ? Described.GetValue() : nullptr;
    return Found != nullptr && Found->bAbstract;
}

/// The two things `@statics` was chosen over a naming convention to make checkable (R-NODE-4).
///
/// A convention produces a silently empty statics module when it is mistyped, and that was the whole
/// argument for a declared association -- so an association naming a class that does not exist, and
/// two modules claiming one class, have to actually say so or the attribute bought nothing.
///
/// Run once per analysis rather than from GetClassStatics, which is asked about one class at a time
/// and so can never see a module naming a class that is not there.
AUTORTFM_DISABLE void GodotVerse::ReportStaticsDiagnostics()
{
    if (!HasIde())
    {
        return;
    }
    const uLang::TSPtr<uLang::CProgramBuildManager> BuildManager = IdeBuildManager();
    if (!BuildManager.IsValid())
    {
        return;
    }
    const uLang::TSRef<uLang::CSemanticProgram>& Program = BuildManager->GetProgramContext()._Program;

    const uLang::CClass* const StaticsAttribute =
        Program->FindDefinitionByVersePath<uLang::CClass>(StaticsAttributePath);
    const uLang::CModule* const Root = Program->FindDefinitionByVersePath<uLang::CModule>(ScriptVersePath);
    if (!StaticsAttribute || !Root)
    {
        return;
    }

    // Claimed class name -> the module that claimed it first, so the second one has something to
    // name. Modules are walked in declaration order, so "first" is stable across analyses.
    TMap<FUtf8String, FUtf8String> ClaimedBy;

    for (const uLang::TSRef<uLang::CModule>& Module : Root->GetDefinitionsOfKind<uLang::CModule>())
    {
        const uLang::TOptional<uLang::CUTF8String> Text =
            Module->GetAttributes().GetAttributeTextValue(StaticsAttribute, *Program);
        if (!Text.IsSet())
        {
            continue;
        }
        const FUtf8String ClassName = FULangConversionUtils::ULangStrToFUtf8String(*Text);
        const FUtf8String ModuleName = FUtf8String(Module->AsNameCString());

        FUtf8String Path;
        int32 Line = -1;
        int32 Column = -1;
        FillLocation(*Module, Path, Line, Column);

        const FUtf8String ClassPath = FUtf8String(ScriptVersePath) + UTF8TEXT("/") + ClassName;
        if (!Program->FindDefinitionByVersePath<uLang::CClass>(
                FULangConversionUtils::FUtf8StringViewToULangStringView(ClassPath)))
        {
            GodotVerse::ReportDiagnostic(VH_SEVERITY_ERROR,
                FUtf8StringView(FUtf8String(UTF8TEXT("`@statics(\"")) + ClassName + UTF8TEXT("\")` on module `")
                    + ModuleName + UTF8TEXT("` names a class no script declares, so nothing will ever read these ")
                    + UTF8TEXT("constants. The name is the class's Verse name, module-qualified the way every ")
                    + UTF8TEXT("other one is -- `gameplay/player` for a class in a module.")),
                FUtf8StringView(Path), Line, Column, Line, Column, FUtf8StringView(), 0);
            continue;
        }

        if (const FUtf8String* const First = ClaimedBy.Find(ClassName))
        {
            GodotVerse::ReportDiagnostic(VH_SEVERITY_ERROR,
                FUtf8StringView(FUtf8String(UTF8TEXT("Modules `")) + *First + UTF8TEXT("` and `") + ModuleName
                    + UTF8TEXT("` both declare themselves the statics of `") + ClassName
                    + UTF8TEXT("`. A class has one statics module: Godot asks it for a single constant map, so ")
                    + UTF8TEXT("which of the two answers is not a question the bridge can decide. Merge them.")),
                FUtf8StringView(Path), Line, Column, Line, Column, FUtf8StringView(), 0);
            continue;
        }
        ClaimedBy.Add(ClassName, ModuleName);
    }
}

namespace {

/// The walk GetClassStatics used to be, off the semantic program and the published package. Still
/// the only implementation: what changed is that it runs once per analysis, on the game thread,
/// rather than once per ask -- reading a constant's value enters the VM, which a running build
/// forbids and which is why this used to join the worker first.
AUTORTFM_DISABLE bool GetClassStaticsLive(FUtf8StringView ClassName,
                                          TArray<GodotVerse::FStaticDesc>& OutStatics,
                                          TArray<vh_value>& OutValues,
                                          TArray<GodotVerse::FFieldStorage>& OutStorage)
{
    using GodotVerse::FStaticDesc;

    OutStatics.Reset();
    OutValues.Reset();
    OutStorage.Reset();

    if (!HasIde())
    {
        return false;
    }
    const uLang::TSPtr<uLang::CProgramBuildManager> BuildManager = IdeBuildManager();
    if (!BuildManager.IsValid())
    {
        return false;
    }
    const uLang::TSRef<uLang::CSemanticProgram>& Program = BuildManager->GetProgramContext()._Program;

    const FUtf8String ClassPath = FUtf8String(ScriptVersePath) + UTF8TEXT("/") + FUtf8String(ClassName);
    if (!Program->FindDefinitionByVersePath<uLang::CClass>(
            FULangConversionUtils::FUtf8StringViewToULangStringView(ClassPath)))
    {
        return false;
    }

    const uLang::CClass* const StaticsAttribute =
        Program->FindDefinitionByVersePath<uLang::CClass>(StaticsAttributePath);
    const uLang::CModule* const Root = Program->FindDefinitionByVersePath<uLang::CModule>(ScriptVersePath);
    if (!StaticsAttribute || !Root)
    {
        // No attribute package means no script could have applied the attribute, so "no statics"
        // is the truthful answer rather than a refusal.
        return true;
    }

    // The undecorated class name, because that is what `@statics("player")` carries: the module
    // path is the script's own, and a module in a Verse module says `gameplay/player` the way every
    // other ClassNameUtf8 in the ABI does.
    for (const uLang::TSRef<uLang::CModule>& Module : Root->GetDefinitionsOfKind<uLang::CModule>())
    {
        const uLang::TOptional<uLang::CUTF8String> Text =
            Module->GetAttributes().GetAttributeTextValue(StaticsAttribute, *Program);
        if (!Text.IsSet())
        {
            continue;
        }
        if (!FUtf8StringView(FULangConversionUtils::ULangStrToFUtf8String(*Text)).Equals(ClassName))
        {
            continue;
        }

        for (const uLang::TSRef<uLang::CFunction>& Function : Module->GetDefinitionsOfKind<uLang::CFunction>())
        {
            FStaticDesc Desc;
            Desc.Name = FUtf8String(Function->AsNameCString());
            Desc.bIsFunction = true;
            FUtf8String DeclaredIn;
            FillLocation(*Function, DeclaredIn, Desc.Line, Desc.Column);
            OutStatics.Add(MoveTemp(Desc));
            OutValues.AddZeroed();
            OutStorage.AddDefaulted();
        }

        for (const uLang::TSRef<uLang::CDataDefinition>& Member : Module->GetDefinitionsOfKind<uLang::CDataDefinition>())
        {
            FStaticDesc Desc;
            Desc.Name = FUtf8String(Member->AsNameCString());
            Desc.bIsFunction = false;
            FUtf8String DeclaredIn;
            FillLocation(*Member, DeclaredIn, Desc.Line, Desc.Column);

            const int32 Slot = OutStatics.Add(MoveTemp(Desc));
            // Zeroed, not defaulted: ValueToWire writes only the lanes the value fills, so an int
            // or a string left its VariantTag as whatever the heap held, and the cook wrote that.
            OutValues.AddZeroed();
            OutStorage.AddDefaulted();

            // The value, read out of the published package rather than evaluated: a module's
            // constants are definitions of that package, looked up by the same decorated path the
            // enum reader builds. A build that has not happened yet has no package, and the name
            // is still reported -- which is the same bargain an `@export` default makes.
            const FUtf8String Decorated = FUtf8String(UTF8TEXT("("))
                + FULangConversionUtils::ULangStrToFUtf8String(
                      Member->_EnclosingScope.GetScopePath('/', uLang::CScope::EPathMode::PrefixSeparator))
                + UTF8TEXT(":)") + FUtf8String(Member->AsNameCString());

            const FMemberType Declared = DescribeType(Member->GetType(), *Program);
            Verse::FRunningContext Context = Verse::FRunningContextPromise{};
            EnterVerse(Context, [&] {
                Verse::VPackage* const Package =
                    Verse::GlobalProgram ? Verse::GlobalProgram->LookupPackage(PublishedScriptPackageName()) : nullptr;
                if (!Package)
                {
                    return;
                }
                const Verse::VValue Value = Package->LookupDefinition(FUtf8StringView(Decorated));
                if (Value.IsUninitialized())
                {
                    return;
                }
                ValueToWire(Context, Value, Declared, OutStorage[Slot], OutValues[Slot]);
            });
        }
    }
    return true;
}

AUTORTFM_DISABLE bool GetClassSignalsLive(FUtf8StringView ClassName, TArray<GodotVerse::FSignalDesc>& OutSignals)
{
    using GodotVerse::FParamDesc;
    using GodotVerse::FSignalDesc;

    OutSignals.Reset();

    if (!HasIde())
    {
        return false;
    }
    const uLang::TSPtr<uLang::CProgramBuildManager> BuildManager = IdeBuildManager();
    if (!BuildManager.IsValid())
    {
        return false;
    }
    const uLang::TSRef<uLang::CSemanticProgram>& Program = BuildManager->GetProgramContext()._Program;

    // Absent when the attribute package did not make it into the program, which also means no
    // script could have applied it. Unlike GetClassExportsLive's, this is not a reason to refuse
    // the whole answer: the `signal(t)` spelling needs no attribute, so the list is still correct
    // for every class that has not adopted the newer one.
    const uLang::CClass* const ExportSignalAttribute =
        Program->FindDefinitionByVersePath<uLang::CClass>(ExportSignalAttributePath);

    const FUtf8String ClassPath = FUtf8String(ScriptVersePath) + UTF8TEXT("/") + FUtf8String(ClassName);
    const uLang::CClass* Class = Program->FindDefinitionByVersePath<uLang::CClass>(
        FULangConversionUtils::FUtf8StringViewToULangStringView(ClassPath));
    if (!Class)
    {
        return false;
    }

    // Whether anything ever hands this class a handle. A handle arrives exactly one way -- Godot
    // attaching the script to an object it made -- and Instantiate refuses a class that does not
    // derive from `object`, so a signal on one is a member that can never be registered, connected
    // or emitted. GDScript has no equivalent of this because every GDScript class extends Object
    // and so carries a signal table of its own; a plain Verse class is a VM object with no Godot
    // counterpart at all. Asked of the leaf, which is the class Godot instantiates.
    const bool bHasGodotOwner = !NativeClassOf(*Class, *Program).IsEmpty();

    // Base first, and the whole chain: **signals inherit**. Phase 2 shipped exactly this bug once
    // already, for @export on a base script class, in two places that had been correct right up
    // until a script could derive from a script.
    const TArray<const uLang::CClass*> Chain = ClassChainOfOrigin(*Class, *Program, EClassOrigin::Script);

    for (const uLang::CClass* Link : Chain)
    {
        for (const uLang::TSRef<uLang::CDataDefinition>& Member : Link->GetDefinitionsOfKind<uLang::CDataDefinition>())
        {
            bool bIsOption = false;
            const uLang::CTypeBase* const MemberType = Member->GetType();
            const uLang::CNormalType* const Normal = MemberType ? &UnwrapDeclaredType(*MemberType, bIsOption) : nullptr;
            const uLang::CClass* const Declared = Normal ? Normal->AsNullable<uLang::CClass>() : nullptr;

            // `@export_signal` is what registers a member with Godot, whichever type declares it --
            // the same bargain `@export` makes for the inspector (R-SIG-1). What the two types
            // differ in is what *silence* means, which is why they are told apart here rather than
            // folded into one test:
            //
            //   - an `event(t)` is useful purely between Verse tasks, so one without the attribute
            //     is not a signal and not a complaint. It is absent from the list entirely.
            //   - a `signal(t)` has no purpose but Godot, so one without the attribute is far
            //     likelier to have forgotten it than to have meant it. It is listed and refused,
            //     which puts the sentence at the member's own line instead of leaving the Node
            //     panel empty for no stated reason.
            const bool bIsSignalType = Declared && IsSignalClass(*Declared);
            const bool bIsEventType = Declared && IsEventClass(*Declared);
            if (!bIsSignalType && !bIsEventType)
            {
                continue;
            }
            const bool bCarriesAttribute = ExportSignalAttribute
                && Member->GetAttributes().HasAttributeClass(ExportSignalAttribute, *Program);
            if (bIsEventType && !bCarriesAttribute)
            {
                continue;
            }

            FSignalDesc Desc;
            Desc.Name = FUtf8String(Member->AsNameCString());

            FPayloadShape Shape;
            DescribePayload(SignalPayloadType(*Declared), *Program, Shape);
            for (const FPayloadArg& Arg : Shape.Args)
            {
                FParamDesc Param;
                Param.Name = Arg.Name;
                Param.Type = Arg.Type.Described.Type;
                Param.VariantTag = Arg.Type.Described.VariantTag;
                DescribeClassOf(Arg.Type, *Program, Param.ClassName, Param.ClassKind);
                Desc.Args.Add(MoveTemp(Param));
            }

            // Member first, payload second: "this cannot be a signal at all" is a better sentence
            // than "its third argument has no Godot type", and the author fixes the member either
            // way. Order within the two is declaration order.
            //
            // The missing attribute sits after those two and before the payload, and both sides of
            // that are deliberate. A member with no Godot owner is not fixed by an attribute, so
            // telling the author to write one there would be the wrong edit; and complaining about
            // the payload of a member Godot was never told about is noise before the edit that
            // matters.
            //
            // **The member's access level is not tested, and VH_SIGNAL_NOT_PUBLIC is retired.** It
            // read as a third rung here, on the reading that connecting is something done from
            // outside the class. Connecting is not done from Verse at all: a designer connects in
            // the Node panel and GDScript connects by string name, and neither consults a Verse
            // specifier. Nothing else in the bridge tests one either -- GetClassExportsLive and
            // GetClassMethodsLive never have, so a non-public `@export` member has always reached
            // the inspector and a non-public method has always been callable from Godot. What the
            // specifier still governs is which *Verse* code may name the member, which is the whole
            // of what it ever promised. tests/verse_probe/signal_access_probe.verse is the
            // measurement, and signal_shadow_probe.verse is why binding by name is still safe: the
            // compiler refuses a member shadowing an inaccessible one of the same name (3593), so
            // two members of one name cannot reach the list.
            if (!bHasGodotOwner)
            {
                Desc.Reject = VH_SIGNAL_NO_GODOT_OWNER;
            }
            else if (Member->IsVar())
            {
                Desc.Reject = VH_SIGNAL_IS_VAR;
            }
            else if (!bCarriesAttribute)
            {
                Desc.Reject = VH_SIGNAL_NEEDS_ATTRIBUTE;
            }
            else
            {
                Desc.Reject = Shape.Reject;
                Desc.RejectDetail = Shape.RejectDetail;
            }

            FUtf8String DeclaredIn;
            FillLocation(*Member, DeclaredIn, Desc.Line, Desc.Column);
            OutSignals.Add(MoveTemp(Desc));
        }
    }
    return true;
}

} // namespace

AUTORTFM_DISABLE bool GodotVerse::GetClassStatics(FUtf8StringView ClassName, TSharedPtr<const FClassStatics>& OutStatics)
{
    OutStatics.Reset();
    const TResult<const FAnalysisSnapshot::FClass*> Described = FindSnapshotClass(ClassName);
    const FAnalysisSnapshot::FClass* const Found = Described ? Described.GetValue() : nullptr;
    if (!Found || !Found->Statics)
    {
        return false;
    }
    OutStatics = Found->Statics;
    return true;
}

namespace {

/// One `@rpc` word, and which of the three categories it settles.
///
/// Godot's own seven and nothing else, matched exactly as GDScript matches them: the words are the
/// API here, and a near miss has to be a diagnostic rather than a default quietly applied.
struct FRpcWord
{
    const char* Word;
    /// 0 permission, 1 locality, 2 transfer mode. Two words of one category is an error, which is
    /// the only reason the category is carried rather than just the value.
    int32 Category;
    int32 Value;
};

constexpr FRpcWord RpcWords[] = {
    {"any_peer", 0, 1},   // MultiplayerAPI::RPC_MODE_ANY_PEER
    {"authority", 0, 2},  // MultiplayerAPI::RPC_MODE_AUTHORITY
    {"call_local", 1, 1},
    {"call_remote", 1, 0},
    {"unreliable", 2, 0}, // MultiplayerPeer::TRANSFER_MODE_UNRELIABLE
    {"unreliable_ordered", 2, 1},
    {"reliable", 2, 2},
};

constexpr const char* RpcCategoryNames[] = {
    "the permission (any_peer/authority)",
    "the locality (call_local/call_remote)",
    "the transfer mode (reliable/unreliable/unreliable_ordered)",
};

/// Splits an `@rpc` config string into its words. Spaces and commas both separate, so a GDScript
/// author who writes Godot's own `"any_peer", "call_local"` inside one string still gets what they
/// meant rather than an unknown word with quotes in it.
AUTORTFM_DISABLE void SplitRpcWords(const FUtf8String& Config, ::TArray<FUtf8String>& OutWords)
{
    FUtf8String Current;
    const auto Flush = [&Current, &OutWords] {
        if (!Current.IsEmpty())
        {
            OutWords.Add(Current);
            Current.Reset();
        }
    };
    for (int32 Index = 0; Index < Config.Len(); ++Index)
    {
        const UTF8CHAR Ch = Config[Index];
        if (Ch == UTF8CHAR(' ') || Ch == UTF8CHAR(',') || Ch == UTF8CHAR('\t')
            || Ch == UTF8CHAR('"') || Ch == UTF8CHAR('\n') || Ch == UTF8CHAR('\r'))
        {
            Flush();
            continue;
        }
        Current.AppendChar(Ch);
    }
    Flush();
}

/// Reads one `@rpc`'s config string into a description.
///
/// The words are matched rather than positional, which is GDScript's rule too, and a number is the
/// channel wherever it appears -- GDScript reads position 3 as the channel, but position means
/// nothing once the four arguments are one string, and "a number is the channel" is the only rule
/// left that a reader can state in one line.
AUTORTFM_DISABLE void ReadRpcConfig(const FUtf8String& Config, GodotVerse::FRpcDesc& OutDesc)
{
    ::TArray<FUtf8String> Words;
    SplitRpcWords(Config, Words);

    bool bCategorySeen[3] = {false, false, false};
    bool bChannelSeen = false;
    for (const FUtf8String& Word : Words)
    {
        // A number is the channel. Tested before the word table so that a Godot release adding a
        // numeric-looking word would be a compile failure here rather than a silent reinterpretation.
        bool bAllDigits = !Word.IsEmpty();
        for (int32 Index = 0; Index < Word.Len(); ++Index)
        {
            if (Word[Index] < UTF8CHAR('0') || Word[Index] > UTF8CHAR('9'))
            {
                bAllDigits = false;
                break;
            }
        }
        if (bAllDigits)
        {
            if (bChannelSeen)
            {
                OutDesc.Reject = VH_RPC_DUPLICATE_CATEGORY;
                OutDesc.RejectDetail = UTF8TEXT("the channel");
                return;
            }
            bChannelSeen = true;
            OutDesc.Channel = FCStringUtf8::Atoi(*Word);
            continue;
        }

        const FRpcWord* Matched = nullptr;
        for (const FRpcWord& Candidate : RpcWords)
        {
            if (Word.Equals(FUtf8String(Candidate.Word)))
            {
                Matched = &Candidate;
                break;
            }
        }
        if (!Matched)
        {
            OutDesc.Reject = VH_RPC_UNKNOWN_ARGUMENT;
            OutDesc.RejectDetail = Word;
            return;
        }
        if (bCategorySeen[Matched->Category])
        {
            OutDesc.Reject = VH_RPC_DUPLICATE_CATEGORY;
            OutDesc.RejectDetail = FUtf8String(RpcCategoryNames[Matched->Category]);
            return;
        }
        bCategorySeen[Matched->Category] = true;

        switch (Matched->Category)
        {
        case 0: OutDesc.RpcMode = Matched->Value; break;
        case 1: OutDesc.bCallLocal = Matched->Value != 0; break;
        default: OutDesc.TransferMode = Matched->Value; break;
        }
    }
}


/// Every method of ClassName carrying an `@rpc`, out of the program the analysis just built.
AUTORTFM_DISABLE bool GetClassRpcsLive(FUtf8StringView ClassName, TArray<GodotVerse::FRpcDesc>& OutRpcs)
{
    using namespace uLang;

    OutRpcs.Reset();
    if (!HasIde())
    {
        return false;
    }
    const TSPtr<CProgramBuildManager> BuildManager = IdeBuildManager();
    if (!BuildManager.IsValid())
    {
        return false;
    }
    const TSRef<CSemanticProgram>& Program = BuildManager->GetProgramContext()._Program;
    const CClass* const RpcAttribute = Program->FindDefinitionByVersePath<CClass>(RpcAttributePath);
    if (!RpcAttribute)
    {
        // No attribute package in this program, which is the state before the first analysis --
        // not "this class has no RPCs", so it is false rather than an empty list.
        return false;
    }

    const FUtf8String ClassPath = FUtf8String(ScriptVersePath) + UTF8TEXT("/") + FUtf8String(ClassName);
    const CClass* const Class = Program->FindDefinitionByVersePath<CClass>(
        FULangConversionUtils::FUtf8StringViewToULangStringView(ClassPath));
    if (!Class)
    {
        return false;
    }

    for (const TSRef<CFunction>& Function : Class->GetDefinitionsOfKind<CFunction>())
    {
        if (!Function->GetAttributes().HasAttributeClass(RpcAttribute, *Program))
        {
            continue;
        }

        GodotVerse::FRpcDesc Desc;
        Desc.Name = FUtf8String(Function->AsNameCString());
        if (OverridesMirroredDefinition(*Function))
        {
            // The name Godot dispatches by, for the reason the method list reports one: an author
            // who writes `@rpc` over `_Process` means Godot's `_process`, and a config keyed by the
            // Verse spelling would name a method Godot never looks for.
            Desc.Name = GodotVirtualNameOf(FUtf8StringView(Desc.Name));
        }
        FUtf8String DeclaredIn;
        FillLocation(*Function, DeclaredIn, Desc.Line, Desc.Column);

        // One string, so GetAttributeTextValue reads it -- that function refuses anything whose
        // argument is a MakeTuple, which is every attribute of more than one argument, and is
        // the whole reason the words travel together rather than as GDScript's four.
        const uLang::TOptional<uLang::CUTF8String> Config =
            Function->GetAttributes().GetAttributeTextValue(RpcAttribute, *Program);
        if (Config.IsSet())
        {
            ReadRpcConfig(FULangConversionUtils::ULangStrToFUtf8String(*Config), Desc);
        }
        OutRpcs.Add(MoveTemp(Desc));
    }
    return true;
}

} // namespace

AUTORTFM_DISABLE bool GodotVerse::GetClassRpcs(FUtf8StringView ClassName, TArray<FRpcDesc>& OutRpcs)
{
    OutRpcs.Reset();
    const TResult<const FAnalysisSnapshot::FClass*> Described = FindSnapshotClass(ClassName);
    const FAnalysisSnapshot::FClass* const Found = Described ? Described.GetValue() : nullptr;
    if (!Found)
    {
        return false;
    }
    OutRpcs = Found->Rpcs;
    return true;
}

AUTORTFM_DISABLE bool GodotVerse::GetClassSignals(FUtf8StringView ClassName, TArray<FSignalDesc>& OutSignals)
{
    OutSignals.Reset();
    const TResult<const FAnalysisSnapshot::FClass*> Described = FindSnapshotClass(ClassName);
    const FAnalysisSnapshot::FClass* const Found = Described ? Described.GetValue() : nullptr;
    if (!Found)
    {
        return false;
    }
    OutSignals = Found->Signals;
    return true;
}

namespace {

/// The `ToString` extension method the project wrote for this class, as a decorated name.
///
/// R-NODE-10's first hook, and it is looked for in the class's *enclosing scope* rather than among
/// the class's own definitions because that is where Verse puts it. `(X:my_class).ToString()` is an
/// **extension method**: a module-level `operator'.ToString'(:my_class, :tuple())`, the receiver
/// first and the call's own arguments as a tuple second. A class member of that name cannot be
/// written at all -- glitch 3532 against /Verse.org/Verse's own ToString, which is reachable as an
/// extension method itself -- so no class method list will ever carry one and GetClassMethodsLive
/// is the wrong place to look. `tests/verse_probe/tostring_probe.verse` is the record.
///
/// The receiver is matched against the class *and its bases*, so a ToString written once for a base
/// class serves every script deriving from it, which is what anything method-shaped should do.
///
/// Empty when the project declares none, which is the common case: Godot then keeps `<Node2D#27>`.
AUTORTFM_DISABLE FUtf8String FindToStringExtensionLive(FUtf8StringView ClassName)
{
    const uLang::CClass* const Class = FindScriptClassLive(ClassName);
    if (!Class)
    {
        return FUtf8String();
    }

    const uLang::CLogicalScope& Scope = Class->_EnclosingScope.GetLogicalScope();
    for (const uLang::TSRef<uLang::CFunction>& Function : Scope.GetDefinitionsOfKind<uLang::CFunction>())
    {
        if (!FUtf8StringView(Function->AsNameCString()).Equals(UTF8TEXTVIEW("operator'.ToString'")))
        {
            continue;
        }

        // The receiver is parameter 0; the call's own arguments are the tuple in parameter 1.
        const uLang::SSignature::ParamDefinitions& Params = Function->_Signature.GetParams();
        if (Params.IsEmpty() || !Params[0])
        {
            continue;
        }
        const uLang::CDataDefinition* const Receiver = Params[0];
        const uLang::CTypeBase* const ParamType = Receiver->GetType();
        if (!ParamType)
        {
            continue;
        }
        const uLang::CClass* const ReceiverClass = ParamType->GetNormalType().AsNullable<uLang::CClass>();
        if (!ReceiverClass)
        {
            continue;
        }
        for (const uLang::CClass* Cursor = Class; Cursor != nullptr; Cursor = Cursor->GetSuperClass())
        {
            if (Cursor == ReceiverClass)
            {
                return ExtensionMethodDecoratedName(*Function);
            }
        }
    }
    return FUtf8String();
}

} // namespace

namespace {

/// Every class the script package declares, module-qualified the way every ClassNameUtf8 in the ABI
/// is: `player` at the root, `gameplay/player` under a `.vmodule` marker.
///
/// Modules are recursed and classes are not. A class nested inside a class resolves by verse path
/// and so used to be describable, but nothing addresses one -- only the class named after its file
/// can go on a node -- and recursing would harvest the archetype the compiler generates per class
/// along with it.
AUTORTFM_DISABLE void CollectSnapshotClassNames(const uLang::CModule& Module,
                                                const FUtf8String& Path,
                                                TArray<FUtf8String>& Out)
{
    for (const uLang::TSRef<uLang::CClass>& Class : Module.GetDefinitionsOfKind<uLang::CClass>())
    {
        const FUtf8String Name = FUtf8String(Class->AsNameCString());
        Out.Add(Path.IsEmpty() ? Name : Path + UTF8TEXT("/") + Name);
    }

    for (const uLang::TSRef<uLang::CModule>& Submodule : Module.GetDefinitionsOfKind<uLang::CModule>())
    {
        const FUtf8String Name = FUtf8String(Submodule->AsNameCString());
        CollectSnapshotClassNames(*Submodule, Path.IsEmpty() ? Name : Path + UTF8TEXT("/") + Name, Out);
    }
}


} // namespace

AUTORTFM_DISABLE TSharedPtr<GodotVerse::FEngineSignalTypes> GodotVerse::CollectEngineSignalTypes()
{
    const uLang::TSPtr<uLang::CProgramBuildManager> BuildManager = IdeBuildManager();
    if (!BuildManager.IsValid())
    {
        return nullptr;
    }
    const uLang::TSRef<uLang::CSemanticProgram>& Program = BuildManager->GetProgramContext()._Program;
    const uLang::CModule* const Mirror = Program->FindDefinitionByVersePath<uLang::CModule>(GodotVersePath);
    if (!Mirror)
    {
        return nullptr;
    }

    TSharedPtr<FEngineSignalTypes> Types = MakeShared<FEngineSignalTypes>();
    for (const uLang::TSRef<uLang::CClass>& Class : Mirror->GetDefinitionsOfKind<uLang::CClass>())
    {
        const FUtf8String ClassName(Class->AsNameCString());
        for (const uLang::TSRef<uLang::CFunction>& Function : Class->GetDefinitionsOfKind<uLang::CFunction>())
        {
            const uLang::CFunctionType* const Type = Function->_Signature.GetFunctionType();
            bool bIsOption = false;
            const uLang::CNormalType* const Returned =
                Type ? &UnwrapDeclaredType(Type->GetReturnType(), bIsOption) : nullptr;
            const uLang::CClass* const Signal = Returned ? Returned->AsNullable<uLang::CClass>() : nullptr;
            if (!Signal || !IsSignalClass(*Signal))
            {
                continue;
            }
            FPayloadShape Shape;
            DescribePayload(SignalPayloadType(*Signal), *Program, Shape);
            Types->Shapes.Add(ClassName + UTF8TEXT(".") + FUtf8String(Function->AsNameCString()),
                              MoveTemp(Shape));
        }
    }
    return Types;
}

namespace {

/// The declared types of one class, recorded while a semantic program still exists.
///
/// Everything here is re-derived per call in an editor host, off the program the last analysis
/// built. A runtime host has no program and no way to make one, so this runs once per analysis and
/// the sidecar carries the answer into the exported game. The three tables are the three questions
/// the VM cannot answer for itself: what type is this member, what does this method take and
/// answer, and what does this signal's payload decompose into.
AUTORTFM_DISABLE void CollectDeclaredTypes(FUtf8StringView ClassName, GodotVerse::FDeclaredTypes& Out)
{
    if (!HasIde())
    {
        return;
    }
    const uLang::TSPtr<uLang::CProgramBuildManager> BuildManager = IdeBuildManager();
    if (!BuildManager.IsValid())
    {
        return;
    }
    const uLang::TSRef<uLang::CSemanticProgram>& Program = BuildManager->GetProgramContext()._Program;

    const FUtf8String ClassPath = FUtf8String(ScriptVersePath) + UTF8TEXT("/") + FUtf8String(ClassName);
    const uLang::CClass* const Class = Program->FindDefinitionByVersePath<uLang::CClass>(
        FULangConversionUtils::FUtf8StringViewToULangStringView(ClassPath));
    if (!Class)
    {
        return;
    }

    // The same chain DescribeMemberType walks, and stopping where it stops: above the script
    // package the members are Godot's own properties, which the mirror describes and this does not.
    // Derived class first, so a member a subclass redeclares wins the way a lookup from the
    // subclass would have found it.
    const TArray<const uLang::CClass*> Chain = ClassChainOfOrigin(*Class, *Program, EClassOrigin::Script);
    for (int32 Link = Chain.Num() - 1; Link >= 0; --Link)
    {
        for (const uLang::TSRef<uLang::CDataDefinition>& Member : Chain[Link]->GetDefinitionsOfKind<uLang::CDataDefinition>())
        {
            const FUtf8String Name(Member->AsNameCString());
            if (Out.Members.Contains(Name))
            {
                continue;
            }
            FMemberType Described = DescribeType(Member->GetType(), *Program);
            Described.bIsVar = Member->IsVar();
            // Both spellings, and unconditionally rather than only for a member carrying
            // `@export_signal`: this table is what a runtime host reads *instead of* a semantic
            // program, and recording a shape for a member that turns out not to be registered
            // costs a map entry, where missing one costs an emission that silently carries nothing.
            if (Described.ReferenceClass
                && (IsSignalClass(*Described.ReferenceClass) || IsEventClass(*Described.ReferenceClass)))
            {
                FPayloadShape Shape;
                DescribePayload(SignalPayloadType(*Described.ReferenceClass), *Program, Shape);
                Out.Signals.Add(Name, MoveTemp(Shape));
            }
            Out.Members.Add(Name, MoveTemp(Described));
        }
    }

    // The class's own functions, which is the set InstanceCall looks a method up in.
    for (const uLang::TSRef<uLang::CFunction>& Function : Class->GetDefinitionsOfKind<uLang::CFunction>())
    {
        const uLang::CFunctionType* const Type = Function->_Signature.GetFunctionType();
        if (!Type)
        {
            continue;
        }
        FMethodSignatureTypes Signature;
        for (const uLang::CDataDefinition* Param : Function->_Signature.GetParams())
        {
            Signature.Params.Add(Param ? DescribeType(Param->GetType(), *Program) : FMemberType{});
        }
        Signature.Result = DescribeType(&Type->GetReturnType(), *Program);
        Out.Methods.Add(FULangConversionUtils::ULangStrToFUtf8String(Function->GetDecoratedName()),
                        MoveTemp(Signature));
    }
}

AUTORTFM_DISABLE const GodotVerse::FDeclaredTypes* RecordedTypes(FUtf8StringView ClassName)
{
    const GodotVerse::TResult<const GodotVerse::FAnalysisSnapshot::FClass*> Entry =
        GodotVerse::FindSnapshotClass(ClassName);
    return Entry ? Entry.GetValue()->Types.Get() : nullptr;
}

/// Carries every class LastGood describes into Pending, whose analysis described nothing, marked
/// with Pending's reason; answers how many. Without it a buffer that does not parse answered "no
/// such class" for every class in the project until the author finished the line: live instances
/// lost their methods, completion its override candidates, and every hover its documentation.
///
/// The statics a carried class holds stay as they were, because reading them needs the program
/// this analysis did not produce; its defaults and bInPublishedProgram are the VM's, and are read
/// again like any other class's.
AUTORTFM_DISABLE int32 CarryLastGoodDescriptions(const FAnalysisSnapshot& LastGood, FAnalysisSnapshot& Pending)
{
    const GodotVerse::EStaleReason Reason = Pending.Stale.GetValue();
    int32 Carried = 0;
    for (const TPair<FUtf8String, FAnalysisSnapshot::FClass>& Pair : LastGood.Classes)
    {
        if (Pending.Classes.Contains(Pair.Key))
        {
            continue;
        }
        Pending.Classes.Add(Pair.Key, Pair.Value).Stale = Reason;
        ++Carried;
    }
    if (Pending.BindingMembers.IsEmpty())
    {
        Pending.BindingMembers = LastGood.BindingMembers;
    }
    if (!Pending.bAstAvailable)
    {
        Pending.ModulesDeclaring = LastGood.ModulesDeclaring;
        Pending.bAstAvailable = LastGood.bAstAvailable;
    }
    return Carried;
}

} // namespace

AUTORTFM_DISABLE void GodotVerse::TakeAnalysisSnapshot()
{
    const double Started = FPlatformTime::Seconds();

    TSharedRef<FAnalysisSnapshot> Snapshot = MakeShared<FAnalysisSnapshot>();
    const uLang::TSPtr<uLang::CProgramBuildManager> BuildManager = IdeBuildManager();
    if (!BuildManager.IsValid())
    {
        // No program to describe. The empty snapshot still replaces whatever was current, because
        // the alternative is answering about a program that no longer exists.
        VH_UNREPORTED("TakeAnalysisSnapshot: the IDE has no build manager, so the snapshot is empty");
        GPendingSnapshot = Snapshot;
        return;
    }

    const uLang::TSRef<uLang::CSemanticProgram>& Program = BuildManager->GetProgramContext()._Program;

    // A program the semantic analyzer never saw has no AST project: the analyzer's first act is to
    // desugar one, and CProgramBuildManager::Build starts every build from a new program.
    if (!Program->_AstProject)
    {
        Snapshot->Stale = EStaleReason::DidNotParse;
    }

    TArray<FUtf8String> ClassNames;
    if (const uLang::CModule* const Root = Program->FindDefinitionByVersePath<uLang::CModule>(ScriptVersePath))
    {
        CollectSnapshotClassNames(*Root, FUtf8String(), ClassNames);
    }

    int32 Candidates = 0;
    double CandidateSeconds = 0.0;

    for (const FUtf8String& ClassName : ClassNames)
    {
        FAnalysisSnapshot::FClass& Entry = Snapshot->Classes.Add(ClassName);

        const uLang::CClass* const Class = FindScriptClassLive(ClassName);
        Entry.bAbstract = Class != nullptr && Class->IsAbstract();

        GetClassMethodsLive(ClassName, Entry.Methods);
        GetClassSignalsLive(ClassName, Entry.Signals);
        GetClassRpcsLive(ClassName, Entry.Rpcs);
        Entry.ToStringDecorated = FindToStringExtensionLive(ClassName);

        // The declared types, which are the analysis's to record and nothing else's to re-derive.
        Entry.Types = MakeShared<GodotVerse::FDeclaredTypes>();
        CollectDeclaredTypes(FUtf8StringView(ClassName), *Entry.Types);
        Entry.bExportsHarvested = GetClassExportsLive(ClassName, Entry.Exports);
        ClassMembersLive(ClassName, Entry.Members);

        // Timed apart from the rest: it describes the whole inherited surface of a mirrored Godot
        // class, which is a different order of work from the handful of names beside it.
        const double CandidatesStarted = FPlatformTime::Seconds();
        ClassOverrideCandidatesLive(ClassName, Entry.Members, Entry.OverrideCandidates);
        CandidateSeconds += FPlatformTime::Seconds() - CandidatesStarted;
        Candidates += Entry.OverrideCandidates.Num();
    }

    // The generated bindings, which the walk above does not reach: it is of the script package
    // alone, so `main_script{}.` opened an empty popup and filled only once the analysis for
    // that one keystroke had landed.
    //
    // Members alone. None of the describing work above applies to a binding -- it declares no
    // exports, no RPCs and no signals of its own, and nothing overrides one -- and
    // ClassOverrideCandidatesLive is the expensive half of that loop, which is why this is not
    // the same pass with a different module handed to it.
    if (const uLang::CModule* const BindingsModule =
            Program->FindDefinitionByVersePath<uLang::CModule>(BindingsVersePath))
    {
        for (const uLang::TSRef<uLang::CClass>& Binding : BindingsModule->GetDefinitionsOfKind<uLang::CClass>())
        {
            TArray<GodotVerse::FCompleteItem> Members;
            ClassOwnMembers(*Binding, Members);
            Snapshot->BindingMembers.Add(FUtf8String(Binding->AsNameCString()), MoveTemp(Members));
        }
    }

    if (Program->_AstProject)
    {
        Snapshot->bAstAvailable = true;
        for (const uLang::CAstCompilationUnit* CompilationUnit : Program->_AstProject->OrderedCompilationUnits())
        {
            for (const uLang::CAstPackage* Package : CompilationUnit->Packages())
            {
                // The project's own packages only. A name that needs an import from the Godot
                // mirror is a different question, and the mirror is one module the whole project
                // already imports.
                const bool bIsUserPackage = Package->_VerseScope == uLang::EVerseScope::PublicUser
                    || Package->_VerseScope == uLang::EVerseScope::InternalUser;
                if (!bIsUserPackage || !Package->_RootModule)
                {
                    continue;
                }
                if (const uLang::CModule* const Root = Package->_RootModule->GetModule())
                {
                    CollectModuleDeclarations(*Root, FUtf8String(), Snapshot->ModulesDeclaring);
                }
            }
        }
    }

    GPendingSnapshot = Snapshot;

    if (AnalysisTraceEnabled())
    {
        fprintf(stderr,
                "[vh-trace]   snapshot: %d class(es), %d name(s), %.2f ms semantic"
                " (%d override candidate(s), %.2f ms)\n",
                Snapshot->Classes.Num(),
                Snapshot->ModulesDeclaring.Num(),
                (FPlatformTime::Seconds() - Started) * 1000.0,
                Candidates,
                CandidateSeconds * 1000.0);
        fflush(stderr);
    }
}

AUTORTFM_DISABLE void GodotVerse::PublishAnalysisSnapshot()
{
    if (!GPendingSnapshot)
    {
        return;
    }

    const double Started = FPlatformTime::Seconds();
    int32 Defaults = 0;
    int32 Constants = 0;

    const int32 Carried = GPendingSnapshot->Stale.IsSet() && GSnapshot
        ? CarryLastGoodDescriptions(*GSnapshot, *GPendingSnapshot)
        : 0;

    for (TPair<FUtf8String, FAnalysisSnapshot::FClass>& Pair : GPendingSnapshot->Classes)
    {
        FAnalysisSnapshot::FClass& Entry = Pair.Value;
        // A carried entry's defaults are the last publish's, and a build may have published since.
        Entry.Defaults.Reset();

        // The published generation's view, which is not the analysis's: a class the buffer has
        // renamed is in one and not the other, and an inspector default is generated code and so
        // only ever comes from a build.
        Entry.bInPublishedProgram = FindGodotClass(Pair.Key) != nullptr;
        if (Entry.bInPublishedProgram && !Entry.Exports.IsEmpty())
        {
            // One transient instance for the whole class rather than one per member, which is what
            // makes reading every default cost about what reading one used to.
            TStrongObjectPtr<UObject> DefaultsObject(NewDefaultsObject(Pair.Key));
            for (const GodotVerse::FExportDesc& Export : Entry.Exports)
            {
                Entry.Defaults.Add(Export.Name, ReadDefaultFieldOf(DefaultsObject.Get(), Export.Name));
                ++Defaults;
            }
        }

        TSharedRef<GodotVerse::FClassStatics> Statics = MakeShared<GodotVerse::FClassStatics>();
        if (GetClassStaticsLive(Pair.Key, Statics->Statics, Statics->Values, Statics->Storage))
        {
            Constants += Statics->Statics.Num();
            Entry.Statics = Statics;
        }
    }

    GSnapshot = GPendingSnapshot;
    GPendingSnapshot.Reset();

    if (AnalysisTraceEnabled())
    {
        fprintf(stderr,
                "[vh-trace]   snapshot: %d default(s), %d static(s), %d class(es) carried stale,"
                " %.2f ms vm\n",
                Defaults,
                Constants,
                Carried,
                (FPlatformTime::Seconds() - Started) * 1000.0);
        fflush(stderr);
    }
}

AUTORTFM_DISABLE void GodotVerse::ForgetAnalysisSnapshots()
{
    GSnapshot.Reset();
    GPendingSnapshot.Reset();
}
