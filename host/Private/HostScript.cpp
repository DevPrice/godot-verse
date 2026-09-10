// Copyright Epic Games, Inc. All Rights Reserved.

#include "HostScript.h"
#include "AutoRTFM.h"
#include "Containers/Map.h"
#include "Containers/UnrealString.h"
#include "GodotClasses.h"
#include "HostEventLoop.h"
#include "HostRuntime.h"
#include "ISolarisIde.h"
#include "ISolarisModule.h"
#include "IVerseModule.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "SolBuildDiagnostic.h"
#include "Templates/Function.h"
#include "TestUtils/PlaceholderObjectForContentScope.h"
#include "ULangUEUtils.h"
#include "VerseComputationLimitControl.h"
#include "VerseContentScope.h"
#include "VerseString.h"
#include "VerseTask.h"
#include "UObject/StrongObjectPtr.h"
#include "VerseVM/Inline/VVMRefInline.h"
#include "VerseVM/Inline/VVMValueInline.h"
#include "VerseVM/Inline/VVMVerseClassInline.h"
#include "VerseVM/VVMArray.h"
#include "VerseVM/VVMMutableArray.h"
#include "VerseVM/VVMShape.h"
#include "VerseVM/VVMNativeRef.h"
#include "VerseVM/VVMRef.h"
#include "VerseVM/VVMRestValue.h"
#include "VerseVM/VVMClass.h"
#include "VerseVM/VVMCoroutine.h"
#include "VerseVM/VVMInt.h"
#include "VerseVM/VVMOpResult.h"
#include "VerseVM/VVMVerseClass.h"
#include "VerseVM/VVMGlobalProgram.h"
#include "VerseVM/VVMNativeFunction.h"
#include "VerseVM/VVMPackage.h"
#include "VerseVM/VVMProgram.h"
#include "VerseVM/VVMContext.h"
#include "VerseVM/VVMUniqueString.h"
#include "uLang/Semantics/Attributable.h"
#include "uLang/Semantics/DataDefinition.h"
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
#include "uLang/SourceProject/VerseVersion.h"
#include "uLang/Toolchain/ProgramBuildManager.h"

#include <atomic>
#include <thread>

namespace {

constexpr const char* ScriptPackageName = "SolIdeDataSources";
constexpr const char* ScriptVersePath = "/user@localhost";
constexpr const char* MainFunctionName = "Main(:[][]char,:[[]char][]char)";

using FMainFunction = TVerseFunction<FVerseResult(
    TVerseCall<void>, const TArray<verse::string>&, const TMap<verse::string, verse::string>&)>;

TSharedPtr<ISolarisIde> GIde;
bool GProjectBuilt = false;

/// Whether the semantic program the IDE currently holds came from an analysis-only build.
/// Code generation hangs an IR package off every module, and the AST accessors the symbol
/// lookup walks assert rather than degrade when it finds one -- so the lookup has to be able
/// to tell the two shapes apart itself instead of trusting that a caller only asks after an
/// analysis.
bool GProgramIsAnalysisOnly = false;

/// Re-analyses the project with one file's text replaced. An empty Path replaces nothing and
/// simply re-analyses what the IDE already holds.
AUTORTFM_DISABLE bool RunCheck(const FUtf8String& Path, const FUtf8String& SourceText, TFunction<void(const FSolDiagnostic&)> Sink);

TArray<TSharedRef<ISolIdeDataSource>> GDataSources;
TSharedPtr<verse::FContentScope> GContentScope;
TOptional<verse::FContentScopeGuard> GContentScopeGuard;

/// A diagnostic captured on the worker, owning its strings so it outlives the analysis that
/// produced it and can be replayed on the game thread.
struct FCapturedDiagnostic
{
    vh_severity Severity;
    FUtf8String Message;
    FUtf8String FilePath;
    int32 Line;
    int32 Column;
    int32 EndLine;
    int32 EndColumn;
    int32 ReferenceCode;
};

struct FBackgroundCheck
{
    std::thread Thread;
    std::atomic<bool> bRunning{false};

    /// Written by the worker before bRunning clears, read by the game thread after joining, so
    /// the join is the whole synchronisation.
    TArray<FCapturedDiagnostic> Diagnostics;
    bool bResult = false;

    /// Joined but not yet delivered. Set by whoever joins, cleared by the poll that reports it.
    bool bResultPending = false;

    FUtf8String Path;
    FUtf8String SourceText;
};

FBackgroundCheck GBackgroundCheck;

AUTORTFM_DISABLE vh_severity ToVhSeverity(ELogVerbosity::Type Verbosity)
{
    switch (Verbosity)
    {
    case ELogVerbosity::Error:
        return VH_SEVERITY_ERROR;
    case ELogVerbosity::Warning:
        return VH_SEVERITY_WARNING;
    default:
        return VH_SEVERITY_INFO;
    }
}

AUTORTFM_DISABLE void ForwardSolDiagnostic(const FSolDiagnostic& Diagnostic)
{
    vh_severity Severity = VH_SEVERITY_INFO;
    switch (Diagnostic.Info.Severity)
    {
    case ELogVerbosity::Error:
        Severity = VH_SEVERITY_ERROR;
        break;
    case ELogVerbosity::Warning:
        Severity = VH_SEVERITY_WARNING;
        break;
    default:
        Severity = VH_SEVERITY_INFO;
        break;
    }

    const FUtf8String Message(Diagnostic.Info.Message);
    const FUtf8String FilePath(Diagnostic.Location.FilePath);

    GodotVerse::ReportDiagnostic(Severity,
                                 Message,
                                 FilePath,
                                 Diagnostic.Location.RowSpan.X,
                                 Diagnostic.Location.ColSpan.X,
                                 Diagnostic.Location.RowSpan.Y,
                                 Diagnostic.Location.ColSpan.Y,
                                 static_cast<int32>(Diagnostic.Info.ReferenceCode));
}

/// Lists what the snippet package actually defines. Callers address Verse functions by decorated
/// name, and a name that is one character off just silently fails to resolve.
AUTORTFM_DISABLE void ReportPackageDefinitions()
{
    Verse::VPackage* Package = Verse::GlobalProgram ? Verse::GlobalProgram->LookupPackage(ScriptPackageName) : nullptr;
    if (!Package)
    {
        GodotVerse::ReportInfo(UTF8TEXT("No script package is loaded."));
        return;
    }

    FUtf8String Line(UTF8TEXT("Verse definitions:"));
    const uint32 Count = Package->NumDefinitions();
    for (uint32 Index = 0; Index < Count; ++Index)
    {
        Line += UTF8TEXT("\n  ");
        Line += Package->GetDefinitionName(Index).AsStringView();
    }
    GodotVerse::ReportInfo(Line);
}

AUTORTFM_DISABLE bool EnsureIde()
{
    if (GIde.IsValid())
    {
        return true;
    }

    // `editable` is declared @customattribhandler, so evaluating a module that applies it asks
    // ICustomAttributeHandler::FindHandlerForAttribute for a handler and fails the whole build
    // with "No custom handler for attribute: editable" when there is none. The handler is
    // registered by FVerseSimulationMetadataModule::StartupModule, and in a monolithic program
    // linking the module does not run that -- nothing loads it unless asked.
    FModuleManager::Get().LoadModule(TEXT("VerseSimulationMetadata"));

    ISolarisModule& SolarisModule = ISolarisModule::Get();

    TOptional<TSharedRef<ISolIdeSourceProject>> MaybeSourceProject = SolarisModule.CreateProjectSource(
        TEXT("VerseHost"), ISolarisModule::EBuildMode::Incremental, MakeIdeDiagnostics(ForwardSolDiagnostic));
    if (!MaybeSourceProject.IsSet())
    {
        GodotVerse::ReportError(UTF8TEXT("Failed to create the Verse source project."));
        return false;
    }

    const FSolIdeConfig IdeConfig{.Flags = ESolIdeFlags::WithPackageUsage,
                                  .VersePath = ScriptVersePath,
                                  .VerseScope = uLang::EVerseScope::InternalUser,
                                  .VerseVersion = Verse::Version::LatestUnstable,
                                  .bAllowExperimental = true};

    TSharedRef<ISolarisIde> Ide = SolarisModule.MakeDevEnvironment(IdeConfig);
    Ide->SetSourceProject(*MaybeSourceProject);
    GIde = Ide;
    return true;
}

} // namespace

AUTORTFM_DISABLE bool GodotVerse::EnterContentScope()
{
    if (GContentScopeGuard.IsSet())
    {
        return true;
    }

    VerseComputationLimitControl::SetComputationLimits(false);

    // Verse needs a UObject outer to instantiate into and we have no UWorld, so synthesize one.
    UPlaceholderObjectForContentScope* PlaceholderObject = UPlaceholderObjectForContentScope::MakeRooted();
    GContentScope = verse::MakeContentScope(PlaceholderObject);
    GContentScopeGuard.Emplace(GContentScope.ToSharedRef());
    return true;
}

AUTORTFM_DISABLE void GodotVerse::LeaveContentScope()
{
    GContentScopeGuard.Reset();
    GContentScope.Reset();
}

AUTORTFM_DISABLE void GodotVerse::ResetScriptState()
{
    LeaveContentScope();
    GDataSources.Empty();
    GIde.Reset();
    GProjectBuilt = false;
}

namespace {

/// `/user@localhost/mover` for `.../scripts/mover.verse`.
AUTORTFM_DISABLE FUtf8String ModulePathFor(const FUtf8String& Path)
{
    const FString Stem = FPaths::GetBaseFilename(FString(Path));
    return FUtf8String(ScriptVersePath) + UTF8TEXT("/") + FUtf8String(Stem);
}

} // namespace

AUTORTFM_DISABLE bool GodotVerse::CompileProject(const TArray<FUtf8String>& Paths)
{
    if (GProjectBuilt)
    {
        ReportError(UTF8TEXT("The Verse program has already been built in this process. Verse "
                             "compiles a whole package at once and a second build aborts the "
                             "engine, so scripts added after startup are not picked up until "
                             "the process restarts."));
        return false;
    }

    if (!EnsureIde())
    {
        return false;
    }

    for (const FUtf8String& Path : Paths)
    {
        FString SourceText;
        if (!FFileHelper::LoadFileToString(SourceText, *FString(Path)))
        {
            ReportError(FUtf8String(TEXT("Failed to open Verse source file: ")) + Path);
            return false;
        }
        GDataSources.Add(GIde->AddDataSource(FULangConversionUtils::FUtf8StringToULangStr(Path)));
    }

    FSolIdeBuildSettings Settings{.LinkSettings = uLang::SBuildParams::ELinkParam::RequireComplete};
    const bool bBuilt = GIde->BuildAll(Settings, MakeIdeDiagnostics(ForwardSolDiagnostic));
    GProjectBuilt = true;
    if (!bBuilt)
    {
        return false;
    }

    IVerseModule::Get(); // Runs VerseModule::StartupModule; VerseCmd does the same before calling in.

    ReportPackageDefinitions();

    // The build just done generated code, which leaves an IR package on every module and puts
    // the AST out of reach. One analysis-only pass over the same sources puts it back, so a
    // symbol resolves on the first hover rather than only after the author's first edit. Its
    // diagnostics are dropped: the build above already reported every one of them.
    RunCheck(FUtf8String(), FUtf8String(), [](const FSolDiagnostic&) {});

    return true;
}

namespace {

/// The analysis itself, with the diagnostics sink left to the caller: the foreground path
/// forwards straight to Godot, the background one captures for replay on the game thread.
AUTORTFM_DISABLE bool RunCheck(const FUtf8String& Path, const FUtf8String& SourceText, TFunction<void(const FSolDiagnostic&)> Sink)
{
    if (!GProjectBuilt || !GIde.IsValid())
    {
        return false;
    }

    for (const TSharedRef<ISolIdeDataSource>& DataSource : GDataSources)
    {
        if (FUtf8String(DataSource->GetPath().AsCString()) == Path)
        {
            DataSource->ResetFromSourceTextNoBroadcasts(FULangConversionUtils::FUtf8StringToULangStr(SourceText));
            break;
        }
    }

    // What cannot happen twice in a process is a build that *generates* -- it re-notifies the
    // already-loaded native Verse packages and aborts inside the async loader. A build that only
    // analyses publishes no packages and can be run as often as the editor types.
    FSolIdeBuildSettings Settings{.LinkSettings = uLang::SBuildParams::ELinkParam::RequireComplete};
    Settings.bSemanticAnalysisOnly = true;
    Settings.bGenerateDigests = false;
    Settings.bGenerateCode = false;
    Settings.bGenerateAutoRTFMBytecode = false;

    const bool bAnalysed = GIde->BuildAll(Settings, MakeIdeDiagnostics(MoveTemp(Sink)));
    GProgramIsAnalysisOnly = GProgramIsAnalysisOnly || bAnalysed;
    return bAnalysed;
}

/// Body of the background thread. A free function rather than a lambda so it can carry
/// AUTORTFM_DISABLE like everything else that reaches Solaris.
AUTORTFM_DISABLE void BackgroundCheckMain()
{
    GBackgroundCheck.bResult = RunCheck(
        GBackgroundCheck.Path,
        GBackgroundCheck.SourceText,
        [](const FSolDiagnostic& Diagnostic) {
            GBackgroundCheck.Diagnostics.Add(FCapturedDiagnostic{
                ToVhSeverity(Diagnostic.Info.Severity),
                FUtf8String(Diagnostic.Info.Message),
                FUtf8String(Diagnostic.Location.FilePath),
                Diagnostic.Location.RowSpan.X,
                Diagnostic.Location.ColSpan.X,
                Diagnostic.Location.RowSpan.Y,
                Diagnostic.Location.ColSpan.Y,
                static_cast<int32>(Diagnostic.Info.ReferenceCode)});
        });

    // Last, so the game thread never observes bRunning false with the results half written.
    GBackgroundCheck.bRunning.store(false, std::memory_order_release);
}

} // namespace

AUTORTFM_DISABLE bool GodotVerse::CheckProject(const FUtf8String& Path, const FUtf8String& SourceText)
{
    WaitForBackgroundCheck();
    return RunCheck(Path, SourceText, [](const FSolDiagnostic& Diagnostic) { ForwardSolDiagnostic(Diagnostic); });
}

AUTORTFM_DISABLE bool GodotVerse::BeginBackgroundCheck(const FUtf8String& Path, const FUtf8String& SourceText)
{
    if (GBackgroundCheck.Thread.joinable() || GBackgroundCheck.bRunning.load(std::memory_order_acquire)
        || GBackgroundCheck.bResultPending)
    {
        return false;
    }
    if (!GProjectBuilt || !GIde.IsValid())
    {
        return false;
    }

    GBackgroundCheck.Path = Path;
    GBackgroundCheck.SourceText = SourceText;
    GBackgroundCheck.Diagnostics.Empty();
    GBackgroundCheck.bResult = false;
    GBackgroundCheck.bRunning.store(true, std::memory_order_release);
    GBackgroundCheck.Thread = std::thread(&BackgroundCheckMain);
    return true;
}

AUTORTFM_DISABLE bool GodotVerse::IsBackgroundCheckRunning()
{
    return GBackgroundCheck.bRunning.load(std::memory_order_acquire);
}

namespace {

/// Joins the worker, leaving what it captured queued for the next poll. Joining is only about
/// making it safe to touch Verse again; delivering the result is PollBackgroundCheck's job, and
/// splitting the two is what stops a wait from swallowing an analysis the caller never heard
/// about -- it would drop those diagnostics and leave the caller asking for the same analysis
/// again on the next keystroke.
AUTORTFM_DISABLE void JoinBackgroundCheck()
{
    if (GBackgroundCheck.Thread.joinable())
    {
        GBackgroundCheck.Thread.join();
        GBackgroundCheck.bResultPending = true;
    }
}

} // namespace

AUTORTFM_DISABLE bool GodotVerse::PollBackgroundCheck(bool& OutFinished)
{
    OutFinished = false;
    if (GBackgroundCheck.bRunning.load(std::memory_order_acquire))
    {
        return true;
    }
    JoinBackgroundCheck();
    if (!GBackgroundCheck.bResultPending)
    {
        return true;
    }

    for (const FCapturedDiagnostic& Diagnostic : GBackgroundCheck.Diagnostics)
    {
        GodotVerse::ReportDiagnostic(Diagnostic.Severity,
                                     Diagnostic.Message,
                                     Diagnostic.FilePath,
                                     Diagnostic.Line,
                                     Diagnostic.Column,
                                     Diagnostic.EndLine,
                                     Diagnostic.EndColumn,
                                     Diagnostic.ReferenceCode);
    }
    GBackgroundCheck.Diagnostics.Empty();
    GBackgroundCheck.bResultPending = false;

    OutFinished = true;
    return GBackgroundCheck.bResult;
}

AUTORTFM_DISABLE void GodotVerse::WaitForBackgroundCheck()
{
    JoinBackgroundCheck();
}

AUTORTFM_DISABLE GodotVerse::FScript* GodotVerse::OpenScript(const FUtf8String& Path)
{
    return new FScript{Path, ModulePathFor(Path)};
}

AUTORTFM_DISABLE GodotVerse::FScript* GodotVerse::CompileFile(const FUtf8String& Path)
{
    TArray<FUtf8String> Paths;
    Paths.Add(Path);
    return CompileProject(Paths) ? OpenScript(Path) : nullptr;
}

AUTORTFM_DISABLE void GodotVerse::ReleaseScript(FScript* Script)
{
    delete Script;
}

namespace {
/// FVerseFunction's package constructor dereferences the result of LookupPackage without checking
/// it, so asking for a function when the build failed crashes rather than returning invalid.
AUTORTFM_DISABLE bool ScriptPackageLoaded()
{
    return Verse::GlobalProgram && Verse::GlobalProgram->LookupPackage(ScriptPackageName) != nullptr;
}

/// Snippet functions are stored under a name that is already decorated with their own scope path,
/// and FVerseFunction decorates once more on lookup - so a plain `Update(:float)` resolves only
/// for some definitions. Try the bare name first, then the pre-decorated one.
AUTORTFM_DISABLE FVerseFunction LookupInScope(const FUtf8String& VersePath, FUtf8StringView DecoratedName)
{
    if (!ScriptPackageLoaded())
    {
        return FVerseFunction(EDefaultConstructVerseFunction::UnsafeDoNotUse);
    }

    const verse::FExecutionContext Context = verse::FExecutionContext::GetActiveContext();

    FVerseFunction Function(Context, ScriptPackageName, VersePath, DecoratedName);
    if (Function.IsValid())
    {
        return Function;
    }

    FUtf8String Prefixed = FUtf8String(UTF8TEXT("(")) + VersePath + UTF8TEXT(":)") + FUtf8String(DecoratedName);
    return FVerseFunction(Context, ScriptPackageName, VersePath, Prefixed);
}

/// A file that wraps itself in a module resolves under that module; one that does not resolves
/// flat. Both shapes stay supported so a single-script project need not be wrapped.
AUTORTFM_DISABLE FVerseFunction LookupFunction(const GodotVerse::FScript* Script, FUtf8StringView DecoratedName)
{
    if (Script && !Script->ModulePath.IsEmpty())
    {
        FVerseFunction Scoped = LookupInScope(Script->ModulePath, DecoratedName);
        if (Scoped.IsValid())
        {
            return Scoped;
        }
    }
    return LookupInScope(FUtf8String(ScriptVersePath), DecoratedName);
}
}

AUTORTFM_DISABLE bool GodotVerse::HasFunction(const FScript* Script, FUtf8StringView DecoratedName)
{
    return LookupFunction(Script, DecoratedName).IsValid();
}

struct GodotVerse::FInstance
{
    TStrongObjectPtr<UObject> Object;

    /// Set by the first call into the object, after which a non-var member can no longer be
    /// given a value. See WriteInstanceField.
    bool bSealed = false;
};

namespace {
/// The UClass behind a script's top-level Verse class, or null if there is no such class or it
/// does not derive from object. A class that does not derive from object has no
/// native representation at all, so `Cast<UClass>` is itself most of the check.
AUTORTFM_DISABLE UClass* FindGodotClass(FUtf8StringView ClassName)
{
    Verse::VPackage* Package = Verse::GlobalProgram ? Verse::GlobalProgram->LookupPackage(ScriptPackageName) : nullptr;
    if (!Package)
    {
        return nullptr;
    }

    const FUtf8String Decorated = FUtf8String(UTF8TEXT("(")) + ScriptVersePath + UTF8TEXT(":)") + FUtf8String(ClassName);

    UClass* Found = nullptr;
    Verse::FRunningContext Context = Verse::FRunningContextPromise{};
    Context.EnterVM([&] {
        Verse::VClass* Class = Package->LookupDefinition<Verse::VClass>(FUtf8StringView(Decorated));
        if (!Class)
        {
            return;
        }
        UClass* NativeClass = Cast<UClass>(Class->GetOrCreateNativeType(Context));
        if (NativeClass && NativeClass->IsChildOf(verse::object::StaticClass()))
        {
            Found = NativeClass;
        }
    });
    return Found;
}
}

AUTORTFM_DISABLE bool GodotVerse::HasClass(FUtf8StringView ClassName)
{
    return FindGodotClass(ClassName) != nullptr;
}

namespace {
/// Epic's own inspector attribute, borrowed rather than reimplemented: uLang guards inheriting
/// from `attribute` behind CScope::IsAuthoredByEpic(), so `/Godot.org/Godot` cannot declare one
/// of its own however it is scoped. `editable` is <public> in a PublicAPI package and carries
/// @attribscope_data, so applying it to a script's data member is legal from anywhere.
constexpr const char* EditableAttributePath = "/Verse.org/Simulation/editable";

// Metadata attributes that carry an inspector hint. Each takes a single string argument, which is
// the one attribute payload SOL-972 leaves readable. These name the attribute *class*, not the
// <constructor> function beside it: GetAttributeTextValue matches on the invocation's return type.
constexpr const char* ClampMinAttributePath = "/Verse.org/Simulation/clamp_min_attribute";
constexpr const char* ClampMaxAttributePath = "/Verse.org/Simulation/clamp_max_attribute";
constexpr const char* CategoryAttributePath = "/Verse.org/Simulation/category_attribute";

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

/// The ABI tag for a member's declared type. Verse's `string` is `[]char`, so the array case has
/// to ask about the element type before it can tell the two apart.
AUTORTFM_DISABLE vh_type VhTypeForVerseType(const uLang::CTypeBase* Type)
{
    if (!Type)
    {
        return VH_TYPE_VOID;
    }

    // A `var` member's declared type is a pointer around the value type. Unwrap that specifically
    // rather than through CNormalType::GetInnerType, which also unwraps an array -- and Verse's
    // `string` is `[]char`, so that route reports every string as a char.
    const uLang::CNormalType* Unwrapped = &Type->GetNormalType();
    while (Unwrapped->GetKind() == uLang::ETypeKind::Pointer || Unwrapped->GetKind() == uLang::ETypeKind::Reference)
    {
        Unwrapped = &static_cast<const uLang::CInvariantValueType*>(Unwrapped)->PositiveValueType()->GetNormalType();
    }

    const uLang::CNormalType& Normal = *Unwrapped;
    switch (Normal.GetKind())
    {
    case uLang::ETypeKind::Logic:
        return VH_TYPE_LOGIC;
    case uLang::ETypeKind::Int:
        return VH_TYPE_INT;
    case uLang::ETypeKind::Float:
        return VH_TYPE_FLOAT;
    case uLang::ETypeKind::Char8:
    case uLang::ETypeKind::Char32:
        return VH_TYPE_CHAR;
    case uLang::ETypeKind::Array:
        return static_cast<const uLang::CArrayType&>(Normal).IsStringType() ? VH_TYPE_STRING : VH_TYPE_ARRAY;
    case uLang::ETypeKind::Map:
        return VH_TYPE_MAP;
    case uLang::ETypeKind::Tuple:
        return VH_TYPE_TUPLE;
    case uLang::ETypeKind::Option:
        return VH_TYPE_OPTION;
    default:
        return VH_TYPE_VOID;
    }
}
} // namespace

namespace {

/// Reads FieldName off Object. Returns false for a field the shape does not carry, and for any
/// Verse type with no vh_value counterpart.
/// The decorated shape key for a member of Object's own class. Factored out because the read and
/// write paths must agree on it exactly.
AUTORTFM_DISABLE FUtf8String ShapeKeyFor(UObject* Object, FUtf8StringView FieldName)
{
    return FUtf8String(UTF8TEXT("(")) + ScriptVersePath + UTF8TEXT("/")
        + FUtf8String(Object->GetClass()->GetName()) + UTF8TEXT(":)") + FUtf8String(FieldName);
}

AUTORTFM_DISABLE bool ReadFieldOf(UObject* Object, FUtf8StringView FieldName, vh_value& OutValue, FUtf8String& OutStorage)
{
    if (!Object)
    {
        return false;
    }

    OutValue = vh_value{};
    OutValue.VariantTag = VH_VARIANT_NIL;
    OutStorage.Reset();

    bool bRead = false;
    Verse::FRunningContext Context = Verse::FRunningContextPromise{};
    Context.EnterVM([&] {
        // The shape lookup is done here rather than through LoadField's by-name overload, which
        // passes Shape.GetField straight into PeekField -- and PeekField asserts on the null that
        // a missing field returns. Asking the shape first is what makes "no such member" an
        // answer instead of a crash.
        //
        // The shape keys a data member by its *declaring* class's decorated name --
        // `(/user@localhost/exports:)Speed`, never a bare `Speed` -- the same decoration
        // FindGodotClass applies to class names and VerseScriptInstance applies to methods.
        // GetClassExports only ever harvests a class's own members, so the object's own class is
        // always the right qualifier.
        Verse::VShape& Shape = UVerseClass::GetShapeForLoadField(Context, Object->GetClass());
        Verse::VUniqueString& Name = Verse::VUniqueString::New(Context, FUtf8StringView(ShapeKeyFor(Object, FieldName)));
        const Verse::VShape::VEntry* Field = Shape.GetField(Name);
        if (Field == nullptr)
        {
            return;
        }

        // PeekField over LoadField: an unset member reads as uninitialized here, where LoadField
        // would raise a Verse runtime error. An inspector asking for a value it may not get is
        // not an error condition.
        //
        // A `var` is stored as a reference, and PeekField hands the reference back rather than
        // what it points at -- deliberately, since that is what an assignment needs. Both spellings
        // have to be followed to reach a value: FPropertyVar answers PeekField with a fresh
        // VNativeRef, and a var with no native representation holds a VRef in its VRestValue slot.
        Verse::VValue Value = Field->Type == Verse::EFieldType::FPropertyVar
            ? Verse::VNativeRef::Peek(Context, Object, Field->UProperty)
            : UVerseClass::PeekField(Context, Object, Field);
        if (Verse::VRef* Ref = Value.DynamicCast<Verse::VRef>())
        {
            Value = Ref->Get(Context);
        }
        if (Value.IsUninitialized())
        {
            return;
        }

        if (Value.IsLogic())
        {
            OutValue.Type = VH_TYPE_LOGIC;
            OutValue.Logic = Value.AsBool() ? 1 : 0;
        }
        else if (Value.IsInt())
        {
            OutValue.Type = VH_TYPE_INT;
            OutValue.Int = Value.AsInt().AsInt64();
        }
        else if (Value.IsFloat())
        {
            OutValue.Type = VH_TYPE_FLOAT;
            OutValue.Float = Value.AsFloat().AsDouble();
        }
        else if (const Verse::VArrayBase* Array = Value.DynamicCast<Verse::VArrayBase>())
        {
            // Verse `string` is `[]char`, so a string arrives as an array of char8. VArrayBase
            // rather than VArray because a `var` of a container type holds a VMutableArray -- the
            // mutability lives in the container itself, not in a reference around it.
            OutStorage = FUtf8String(Array->AsStringView());
            OutValue.Type = VH_TYPE_STRING;
            OutValue.String.Utf8 = reinterpret_cast<const char*>(*OutStorage);
            OutValue.String.Len = OutStorage.Len();
        }
        else
        {
            return;
        }

        bRead = true;
    });
    return bRead;
}

} // namespace

namespace {

/// Whether ClassName declares FieldName as a `var`, per the semantic program.
///
/// The shape cannot answer this reliably -- its EFieldType says where a field is stored, not what
/// Verse permits -- so the question goes back to the definition that declared it.
AUTORTFM_DISABLE bool IsVarMember(FUtf8StringView ClassName, FUtf8StringView FieldName)
{
    if (!GIde.IsValid())
    {
        return false;
    }
    const uLang::TSPtr<uLang::CProgramBuildManager> BuildManager = GIde->GetBuildManager();
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

    for (const uLang::TSRef<uLang::CDataDefinition>& Member : Class->GetDefinitionsOfKind<uLang::CDataDefinition>())
    {
        if (FUtf8StringView(Member->AsNameCString()).Equals(FieldName))
        {
            return Member->IsVar();
        }
    }
    return false;
}

/// Assigning writes *through* a var's reference, the way `set X = ...` does. Initializing writes
/// over the storage itself, the way the constructor does, which is the only way to give a non-var
/// a value -- and is wrong for a var, because it would replace the reference with a bare value and
/// leave the next `set` dereferencing something that is not a ref.
enum class EFieldWrite : uint8
{
    Assign,
    Initialize,
};

AUTORTFM_DISABLE bool WriteFieldOf(UObject* Object, FUtf8StringView FieldName, const vh_value& Value, EFieldWrite Mode)
{
    if (!Object)
    {
        return false;
    }

    if (Mode == EFieldWrite::Assign && !IsVarMember(FUtf8String(Object->GetClass()->GetName()), FieldName))
    {
        return false;
    }

    bool bWrote = false;
    Verse::FRunningContext Context = Verse::FRunningContextPromise{};
    Context.EnterVM([&] {
        Verse::VShape& Shape = UVerseClass::GetShapeForLoadField(Context, Object->GetClass());
        Verse::VUniqueString& Name = Verse::VUniqueString::New(Context, FUtf8StringView(ShapeKeyFor(Object, FieldName)));
        const Verse::VShape::VEntry* Field = Shape.GetField(Name);

        // A Constant entry lives in the shape itself rather than in the object, so it is shared by
        // every instance and cannot be assigned to.
        if (Field == nullptr || !Field->IsProperty())
        {
            return;
        }

        // A member's storage already holds a value in the exact representation the compiled code
        // expects, and matching it is the whole job: the VM does not re-check a slot it is told
        // holds a string, so a plausible-looking value of the wrong cell type reads back fine and
        // dies later inside the interpreter. Everything below is chosen against Current.
        Verse::VRestValue* const Slot = Field->Type == Verse::EFieldType::FVerseProperty
            ? Field->UProperty->ContainerPtrToValuePtr<Verse::VRestValue>(Object)
            : nullptr;
        const Verse::VValue Current = Slot ? Slot->Get(Context)
                                           : Verse::VNativeRef::Peek(Context, Object, Field->UProperty);

        Verse::VValue NewValue;
        switch (Value.Type)
        {
        case VH_TYPE_LOGIC:
            NewValue = Verse::VValue::FromBool(Value.Logic != 0);
            break;
        case VH_TYPE_INT:
            NewValue = Verse::VValue(Verse::VInt(Context, Value.Int));
            break;
        case VH_TYPE_FLOAT:
            NewValue = Verse::VValue(Verse::VFloat(Value.Float));
            break;
        case VH_TYPE_STRING:
        {
            // Verse hangs the mutability of a container off the container, not off a reference
            // around it: `var Label:string` holds a VMutableArray where a plain one holds a VArray.
            const FUtf8StringView Utf8(reinterpret_cast<const UTF8CHAR*>(Value.String.Utf8), Value.String.Len);
            Verse::VRef* const Box = Current.DynamicCast<Verse::VRef>();
            const Verse::VValue Inner = Box ? Box->Get(Context) : Current;
            NewValue = Inner.IsCellOfType<Verse::VMutableArray>()
                ? Verse::VValue(Verse::VMutableArray::New(Context, Utf8))
                : Verse::VValue(Verse::VArray::New(Context, Utf8));
            break;
        }
        default:
            return;
        }

        if (Slot)
        {
            // A var of a scalar type puts a VRef box in the slot and the value inside it; a var of
            // a container type puts the mutable container straight in. Writing over the box is what
            // the interpreter later dies on with "Unexpected ref type".
            if (Verse::VRef* Ref = Current.DynamicCast<Verse::VRef>())
            {
                Ref->Set(Context, NewValue);
            }
            else
            {
                Slot->Set(Context, NewValue);
            }
            bWrote = true;
        }
        else
        {
            // Both FProperty and FPropertyVar are native storage behind an FProperty, and
            // VNativeRef::Set is the write for either -- for a var it is the assignment, and for a
            // non-var it is what the interpreter itself uses to initialize one.
            const Verse::FOpResult Result = Verse::VNativeRef::New(Context, Object, Field->UProperty).Set(Context, NewValue);
            bWrote = Result.IsReturn();
        }
    });
    return bWrote;
}

} // namespace

AUTORTFM_DISABLE bool GodotVerse::WriteInstanceField(FInstance* Instance, FUtf8StringView FieldName, const vh_value& Value)
{
    if (!Instance || !Instance->Object.IsValid())
    {
        return false;
    }
    return WriteFieldOf(Instance->Object.Get(), FieldName, Value,
                        Instance->bSealed ? EFieldWrite::Assign : EFieldWrite::Initialize);
}

AUTORTFM_DISABLE bool GodotVerse::ReadInstanceField(const FInstance* Instance, FUtf8StringView FieldName, vh_value& OutValue, FUtf8String& OutStorage)
{
    if (!Instance || !Instance->Object.IsValid())
    {
        return false;
    }
    return ReadFieldOf(Instance->Object.Get(), FieldName, OutValue, OutStorage);
}

AUTORTFM_DISABLE bool GodotVerse::ReadClassDefaultField(FUtf8StringView ClassName, FUtf8StringView FieldName, vh_value& OutValue, FUtf8String& OutStorage)
{
    UClass* NativeClass = FindGodotClass(ClassName);
    if (!NativeClass)
    {
        return false;
    }
    // A transient instance, not the CDO. UVerseClass runs the Verse constructor from
    // PostInitInstance, which NewObject drives and class-default-object construction does not, so
    // a CDO's members read back uninitialized. An instance is the only place a declared default
    // actually exists. Handle is left unset: reading a plain data member never consults it.
    TStrongObjectPtr<UObject> Defaults(NewObject<UObject>(GetTransientPackage(), NativeClass));
    if (!Defaults.IsValid())
    {
        return false;
    }
    return ReadFieldOf(Defaults.Get(), FieldName, OutValue, OutStorage);
}

AUTORTFM_DISABLE bool GodotVerse::GetClassExports(FUtf8StringView ClassName, TArray<FExportDesc>& OutExports)
{
    OutExports.Reset();

    if (!GIde.IsValid())
    {
        return false;
    }

    const uLang::TSPtr<uLang::CProgramBuildManager> BuildManager = GIde->GetBuildManager();
    if (!BuildManager.IsValid())
    {
        return false;
    }
    const uLang::TSRef<uLang::CSemanticProgram>& Program = BuildManager->GetProgramContext()._Program;

    // Absent when VerseSimulationMetadata is not in the package set, which also means no script
    // could have applied the attribute -- an empty list would claim the script exports nothing,
    // so this reports "cannot answer" instead.
    const uLang::CClass* EditableAttribute =
        Program->FindDefinitionByVersePath<uLang::CClass>(EditableAttributePath);
    if (!EditableAttribute)
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

    // Absent when VerseSimulationMetadata predates these attributes; a member simply gets no hint
    // rather than the whole harvest failing, which is why these are not checked like the one above.
    const uLang::CClass* ClampMinAttribute = Program->FindDefinitionByVersePath<uLang::CClass>(ClampMinAttributePath);
    const uLang::CClass* ClampMaxAttribute = Program->FindDefinitionByVersePath<uLang::CClass>(ClampMaxAttributePath);
    const uLang::CClass* CategoryAttribute = Program->FindDefinitionByVersePath<uLang::CClass>(CategoryAttributePath);

    // Only the class's own members. An inherited export would have to be looked up through the
    // object hierarchy, and every one of those is generated API rather than script state.
    for (const uLang::TSRef<uLang::CDataDefinition>& Member : Class->GetDefinitionsOfKind<uLang::CDataDefinition>())
    {
        if (!Member->HasAttributeSubclass(EditableAttribute, *Program))
        {
            continue;
        }

        OutExports.Add(FExportDesc{
            FUtf8String(Member->AsNameCString()),
            VhTypeForVerseType(Member->GetType()),
            Member->IsVar(),
            AttributeText(*Member, ClampMinAttribute, *Program),
            AttributeText(*Member, ClampMaxAttribute, *Program),
            AttributeText(*Member, CategoryAttribute, *Program)});
    }
    return true;
}

namespace {

/// An STextRange ends exclusively, so the position one past an identifier's last byte does not
/// resolve to it. That is what makes hovering the `(` of a call miss the callee rather than hit it.
AUTORTFM_DISABLE bool LocusContains(const Verse::SLocus& Range, uint32 Row, uint32 Column)
{
    const uLang::STextPosition Position{Row, Column};
    return Range.GetBegin() <= Position && Position < Range.GetEnd();
}

/// The definition an identifier node resolves to, or null for a node that is not one.
///
/// Definition nodes are deliberately absent: a definition's locus spans its whole body, so
/// treating one as a hit would resolve every blank column inside a function to the function.
AUTORTFM_DISABLE const uLang::CDefinition* ReferencedDefinition(const uLang::CAstNode& AstNode,
                                                                const uLang::CSemanticProgram& Program,
                                                                vh_lookup_kind& OutKind)
{
    using namespace uLang;

    switch (AstNode.GetNodeType())
    {
    case EAstNodeType::Identifier_Data:
        OutKind = VH_LOOKUP_DATA;
        return &static_cast<const CExprIdentifierData&>(AstNode)._DataDefinition;

    case EAstNodeType::Identifier_Function:
        OutKind = VH_LOOKUP_FUNCTION;
        return &static_cast<const CExprIdentifierFunction&>(AstNode)._Function;

    case EAstNodeType::Identifier_OverloadedFunction:
        // No overload has been picked, and every candidate shares the name the author clicked.
        // The first is a better answer than refusing to resolve at all.
        for (const CFunction* Function : static_cast<const CExprIdentifierOverloadedFunction&>(AstNode)._FunctionOverloads)
        {
            if (Function)
            {
                OutKind = VH_LOOKUP_FUNCTION;
                return Function;
            }
        }
        return nullptr;

    case EAstNodeType::Identifier_Class:
        if (const CClass* Class = static_cast<const CExprIdentifierClass&>(AstNode).GetClass(Program))
        {
            OutKind = VH_LOOKUP_CLASS;
            return Class->_Generalized;
        }
        return nullptr;

    case EAstNodeType::Identifier_Enum:
        if (const CEnumeration* Enumeration = static_cast<const CExprEnumerationType&>(AstNode).GetEnumeration(Program))
        {
            OutKind = VH_LOOKUP_ENUM;
            return Enumeration;
        }
        return nullptr;

    case EAstNodeType::Identifier_TypeAlias:
        OutKind = VH_LOOKUP_TYPE_ALIAS;
        return &static_cast<const CExprIdentifierTypeAlias&>(AstNode)._TypeAlias;

    case EAstNodeType::Identifier_Module:
        if (const CModule* Module = static_cast<const CExprIdentifierModule&>(AstNode).GetModule(Program))
        {
            OutKind = VH_LOOKUP_MODULE;
            return Module;
        }
        return nullptr;

    case EAstNodeType::Identifier_ModuleAlias:
        OutKind = VH_LOOKUP_MODULE;
        return &static_cast<const CExprIdentifierModuleAlias&>(AstNode)._ModuleAlias;

    default:
        return nullptr;
    }
}

struct AUTORTFM_DISABLE FLookupVisitor : public uLang::SAstVisitor
{
    FLookupVisitor(const uLang::CSemanticProgram& InProgram, const FUtf8String& InPath, uint32 InRow, uint32 InColumn)
        : Program(InProgram)
        , Path(InPath)
        , Row(InRow)
        , Column(InColumn)
    {
    }

    virtual void Visit(const char* FieldName, uLang::CAstNode& AstNode) override { VisitElement(AstNode); }

    virtual void VisitElement(uLang::CAstNode& AstNode) override
    {
        if (const Verse::Vst::Node* Vst = AstNode.GetMappedVstNode())
        {
            if (LocusContains(Vst->Whence(), Row, Column)
                && FULangConversionUtils::ULangStrToFUtf8String(Vst->GetSnippetPath()).Equals(Path, ESearchCase::IgnoreCase))
            {
                vh_lookup_kind Kind = VH_LOOKUP_UNKNOWN;
                if (const uLang::CDefinition* Definition = ReferencedDefinition(AstNode, Program, Kind))
                {
                    // A child's locus is contained in its parent's, so the deepest node visited
                    // that still contains the cursor is the innermost -- last write wins.
                    Found = Definition;
                    FoundKind = Kind;
                }
            }
        }
        AstNode.VisitChildren(*this);
    }

    const uLang::CSemanticProgram& Program;
    FUtf8String Path;
    uint32 Row;
    uint32 Column;
    const uLang::CDefinition* Found{nullptr};
    vh_lookup_kind FoundKind{VH_LOOKUP_UNKNOWN};
};

} // namespace

AUTORTFM_DISABLE bool GodotVerse::LookupSymbol(FUtf8StringView Path, int32 Line, int32 Column, FLookupDesc& OutDesc)
{
    OutDesc = FLookupDesc{};

    if (!GIde.IsValid() || !GProgramIsAnalysisOnly || Line < 0 || Column < 0)
    {
        return false;
    }
    const uLang::TSPtr<uLang::CProgramBuildManager> BuildManager = GIde->GetBuildManager();
    if (!BuildManager.IsValid())
    {
        return false;
    }
    const uLang::TSRef<uLang::CSemanticProgram>& Program = BuildManager->GetProgramContext()._Program;
    if (!Program->_AstProject)
    {
        return false;
    }

    FLookupVisitor Visitor(*Program, FUtf8String(Path), (uint32)Line, (uint32)Column);
    for (const uLang::CAstCompilationUnit* CompilationUnit : Program->_AstProject->OrderedCompilationUnits())
    {
        for (const uLang::CAstPackage* Package : CompilationUnit->Packages())
        {
            // Only the project's own packages have a file the editor could jump into; the
            // generated Godot API and Verse's own library are compiled from elsewhere.
            const bool bIsUserPackage = Package->_VerseScope == uLang::EVerseScope::PublicUser
                || Package->_VerseScope == uLang::EVerseScope::InternalUser;
            if (!bIsUserPackage || !Package->_RootModule || !Package->_RootModule->GetAstPackage())
            {
                continue;
            }
            Package->_RootModule->GetAstPackage()->VisitChildren(Visitor);
        }
    }

    if (!Visitor.Found)
    {
        return false;
    }

    const uLang::CDefinition& Definition = *Visitor.Found;
    OutDesc.Name = FUtf8String(Definition.AsNameCString());
    OutDesc.Kind = Visitor.FoundKind;

    if (const uLang::CDataDefinition* Data = Definition.AsNullable<uLang::CDataDefinition>())
    {
        OutDesc.bIsVar = Data->IsVar();
        if (const uLang::CTypeBase* Type = Data->GetType())
        {
            OutDesc.Type = FULangConversionUtils::ULangStrToFUtf8String(Type->AsCode());
        }
    }
    else if (const uLang::CFunction* Function = Definition.AsNullable<uLang::CFunction>())
    {
        if (const uLang::CFunctionType* Type = Function->_Signature.GetFunctionType())
        {
            OutDesc.Type = FULangConversionUtils::ULangStrToFUtf8String(Type->AsCode());
        }
    }

    // A definition compiled from a package the project does not own has no file to point at.
    // It still describes fine, which is what the hover card wants.
    if (const uLang::CExpressionBase* DefinitionNode = Definition.GetAstNode())
    {
        if (const Verse::Vst::Node* Vst = DefinitionNode->GetMappedVstNode())
        {
            const Verse::SLocus& Whence = Vst->Whence();
            OutDesc.Path = FULangConversionUtils::ULangStrToFUtf8String(Vst->GetSnippetPath());
            OutDesc.Line = (int32)Whence.BeginRow();
            OutDesc.Column = (int32)Whence.BeginColumn();
        }
    }
    return true;
}

AUTORTFM_DISABLE GodotVerse::FInstance* GodotVerse::Instantiate(FUtf8StringView ClassName, int64 Handle)
{
    UClass* NativeClass = FindGodotClass(ClassName);
    if (!NativeClass)
    {
        ReportError(FUtf8String(UTF8TEXT("Could not instantiate ")) + FUtf8String(ClassName)
                    + UTF8TEXT(": no such class deriving from object at ") + ScriptVersePath);
        return nullptr;
    }

    // UVerseClass::PostInitInstance runs the Verse constructor from inside NewObject, so fields
    // are initialised by the time this returns.
    UObject* Instance = NewObject<UObject>(GetTransientPackage(), NativeClass);
    if (!Instance)
    {
        return nullptr;
    }

    verse::object* Shadow = CastChecked<verse::object>(Instance);
    Shadow->Handle.Init(Handle, Shadow);

    return new FInstance{TStrongObjectPtr<UObject>(Instance)};
}

AUTORTFM_DISABLE void GodotVerse::ReleaseInstance(FInstance* Instance)
{
    delete Instance;
}

namespace {
AUTORTFM_DISABLE FVerseFunction LookupMethod(const GodotVerse::FInstance* Instance, FUtf8StringView DecoratedName)
{
    if (!Instance || !Instance->Object.IsValid())
    {
        return FVerseFunction(EDefaultConstructVerseFunction::UnsafeDoNotUse);
    }
    const verse::FExecutionContext Context = verse::FExecutionContext::GetActiveContext();
    return FVerseFunction(Context, Instance->Object.Get(), DecoratedName);
}
}

/// Whether the script actually implements this lifecycle method.
///
/// `object` gives Ready, Update and PhysicsUpdate empty bodies so a script can <override>
/// them and so a script that wants only one of the three still compiles -- which means a plain
/// "does it resolve" test is true for every instance. Comparing the resolved function against
/// the one the base class resolves to is what distinguishes an override from the inherited
/// no-op, and it decides whether Godot puts this node in the per-frame process list at all.
AUTORTFM_DISABLE bool GodotVerse::InstanceHasFunction(const FInstance* Instance, FUtf8StringView DecoratedName)
{
    FVerseFunction Resolved = LookupMethod(Instance, DecoratedName);
    if (!Resolved.IsValid())
    {
        return false;
    }

    const verse::FExecutionContext Context = verse::FExecutionContext::GetActiveContext();
    FVerseFunction Base(Context, verse::object::StaticClass()->GetDefaultObject(), DecoratedName);
    return !Base.IsValid() || Base.Function.Get() != Resolved.Function.Get();
}

namespace {
template <typename FunctionType, typename... ArgTypes>
AUTORTFM_DISABLE int32 CallMethod(const GodotVerse::FInstance* Instance, FUtf8StringView DecoratedName, ArgTypes... Args)
{
    const verse::FExecutionContext Context = verse::FExecutionContext::GetActiveContext();

    FunctionType Function{LookupMethod(Instance, DecoratedName)};
    if (!Function.IsValid())
    {
        GodotVerse::ReportError(FUtf8String(UTF8TEXT("Could not resolve ")) + FUtf8String(DecoratedName)
                                + UTF8TEXT(" on the script instance."));
        return VH_ERR_NOT_FOUND;
    }

    const AutoRTFM::ETransactionResult TransactionResult =
        AutoRTFM::Transact([&] { Function(Context, Args...); });

    return TransactionResult == AutoRTFM::ETransactionResult::Committed ? VH_OK : VH_ERR_RUNTIME;
}
}

AUTORTFM_DISABLE int32 GodotVerse::InstanceCallVoid(FInstance* Instance, FUtf8StringView DecoratedName)
{
    Instance->bSealed = true;
    return CallMethod<TVerseFunction<void()>>(Instance, DecoratedName);
}

AUTORTFM_DISABLE int32 GodotVerse::InstanceCallVoidFloat(FInstance* Instance, FUtf8StringView DecoratedName, double Arg)
{
    Instance->bSealed = true;
    return CallMethod<TVerseFunction<void(double)>>(Instance, DecoratedName, Arg);
}

namespace {
// Await invokes this from a closed transactional nest, so it must stay AutoRTFM-enabled.
void OnMainFinished(FVerseTask Task)
{
    if (Task.Completed())
    {
        GodotVerse::RequestExit(GodotVerse::FRunExit::Completed());
    }
}
}

AUTORTFM_DISABLE int32 GodotVerse::RunMain(const TArray<verse::string>& Args, int64& OutExitCode)
{
    const verse::FExecutionContext Context = verse::FExecutionContext::GetActiveContext();

    FMainFunction MainFunction{ScriptPackageLoaded()
                                   ? FVerseFunction(Context, ScriptPackageName, ScriptVersePath, MainFunctionName)
                                   : FVerseFunction(EDefaultConstructVerseFunction::UnsafeDoNotUse)};
    if (!MainFunction.IsValid())
    {
        ReportError(UTF8TEXT("The script has no Main(:[]string, :[string]string) function."));
        return VH_ERR_NOT_FOUND;
    }

    const TMap<verse::string, verse::string> Env;

    EnqueueAsyncJob([MainFunction, Args, Env](const verse::FExecutionContext& ExecContext) {
        const AutoRTFM::ETransactionResult TransactionResult = AutoRTFM::Transact([&] {
            FVerseTask ScriptTask = MainFunction(ExecContext, Args, Env);
            ScriptTask.Await(ExecContext, OnMainFinished);
        });
        if (TransactionResult != AutoRTFM::ETransactionResult::Committed)
        {
            RequestExit(FRunExit::Error());
        }
    });

    PumpEventLoop(Context, 0.0);

    if (!HasPendingExit())
    {
        ReportError(UTF8TEXT("Main suspended on something the host does not drive, and never completed."));
        return VH_ERR_RUNTIME;
    }

    const FRunExit Exit = ConsumeExit();
    OutExitCode = Exit.ExitCode;
    return Exit.Reason == FRunExit::EReason::Error ? VH_ERR_RUNTIME : VH_OK;
}

namespace {
template <typename FunctionType, typename... ArgTypes>
AUTORTFM_DISABLE int32 CallFunction(const GodotVerse::FScript* Script, FUtf8StringView DecoratedName, ArgTypes... Args)
{
    const verse::FExecutionContext Context = verse::FExecutionContext::GetActiveContext();

    FunctionType Function{LookupFunction(Script, DecoratedName)};
    if (!Function.IsValid())
    {
        GodotVerse::ReportError(FUtf8String(UTF8TEXT("Could not resolve ")) + FUtf8String(DecoratedName)
                                + UTF8TEXT(" in ") + (Script ? Script->ModulePath : FUtf8String())
                                + UTF8TEXT(" or ") + ScriptVersePath);
        return VH_ERR_NOT_FOUND;
    }

    const AutoRTFM::ETransactionResult TransactionResult =
        AutoRTFM::Transact([&] { Function(Context, Args...); });

    return TransactionResult == AutoRTFM::ETransactionResult::Committed ? VH_OK : VH_ERR_RUNTIME;
}
}

AUTORTFM_DISABLE int32 GodotVerse::CallVoid(const FScript* Script, FUtf8StringView DecoratedName)
{
    return CallFunction<TVerseFunction<void()>>(Script, DecoratedName);
}

AUTORTFM_DISABLE int32 GodotVerse::CallVoidFloat(const FScript* Script, FUtf8StringView DecoratedName, double Arg)
{
    return CallFunction<TVerseFunction<void(double)>>(Script, DecoratedName, Arg);
}

AUTORTFM_DISABLE void GodotVerse::TickScripts(double BudgetSeconds)
{
    PumpEventLoop(verse::FExecutionContext::GetActiveContext(), BudgetSeconds);
}
