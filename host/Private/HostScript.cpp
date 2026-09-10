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
#include "uLang/SourceProject/SourceDataProject.h"
#include "uLang/SourceProject/VerseScope.h"
#include "uLang/SourceProject/VerseVersion.h"
#include "uLang/CompilerPasses/ApiLayerInjections.h"
#include "uLang/Toolchain/ModularFeatureManager.h"
#include "uLang/Toolchain/ProgramBuildManager.h"

#include <atomic>
#include <thread>

namespace {

constexpr const char* ScriptPackageName = "SolIdeDataSources";
constexpr const char* ScriptVersePath = "/user@localhost";
constexpr const char* MainFunctionName = "Main(:[][]char,:[[]char][]char)";

/// A second package sharing the native package's verse path, so a script's existing
/// `using { /Godot.org/Godot }` reaches these definitions with no extra import.
constexpr const char* AttributePackageName = "GodotAttributes";
constexpr const char* AttributePackageVersePath = "/Godot.org/Godot";
constexpr const char* AttributeSnippetPath = "GodotAttributes.verse";

/// The attributes this bridge owns, as Verse source compiled in this process.
///
/// They cannot ship in host/Verse with the rest of the package: `class(attribute)` is refused
/// unless CScope::IsAuthoredByEpic(), which only FGodotAuthorshipInjection below grants -- and
/// that runs here, while host/Verse is compiled by VNI at UBT time, which nothing we build can
/// reach. Hence a runtime-only package for the one thing VNI will not accept.
constexpr const char* AttributePackageSource =
    "# Registers the class it is applied to as a Godot global class, the way C#'s [GlobalClass]\n"
    "# does. A marker with no argument: the name Godot registers is the class's own, which the\n"
    "# one-top-level-name-per-file rule already pins to the file stem.\n"
    "@attribscope_class\n"
    "global_class<public> := class<computes>(attribute) {}\n";

using FMainFunction = TVerseFunction<FVerseResult(
    TVerseCall<void>, const TArray<verse::string>&, const TMap<verse::string, verse::string>&)>;

TSharedPtr<ISolarisIde> GIde;
bool GProjectBuilt = false;

/// Lets `/Godot.org/` declare attributes of its own.
///
/// AddSuperType refuses `class(attribute)` unless CScope::IsAuthoredByEpic(), which tests the
/// definition's verse path against CSemanticProgram::_EpicInternalModulePrefixes -- seeded with
/// Epic's three domains by CSemanticProgram::Initialize and reachable no other way. Note this
/// grants *authorship*, which the InternalUser package scope does not: that only unlocks access
/// to epic_internal definitions, which is why `@editable` can be borrowed but not declared.
///
/// It has to run per build rather than once: CProgramBuildManager::Build calls
/// ResetSemanticProgram() before every compile and every analysis, so the program -- and its
/// prefix list -- is new each time. This is the first hook that runs after that reset and still
/// ahead of analysis. The auto-qualify pre-pass builds a second program of its own, but routes
/// through the same RunCompilerPrePass, so it is covered too.
class FGodotAuthorshipInjection : public uLang::IPreSemAnalysisInjection
{
public:
    AUTORTFM_DISABLE virtual bool Ingest(
        const Verse::Vst::TNodeRef<Verse::Vst::Project>&,
        const uLang::SProgramContext& ProgramContext,
        const uLang::SBuildContext&) override
    {
        ProgramContext._Program->_EpicInternalModulePrefixes.AddUnique("/Godot.org/");
        return false; // Do not halt the toolchain.
    }
};

/// Adds the attribute package to the IDE's source project.
///
/// Must run before the first AddDataSource. FSolarisIde::EnsureDataSourcePackageExists snapshots
/// the project's other packages as the script package's dependencies exactly once, guarded on that
/// list being empty -- so a package added after the first script is never depended on, and its
/// definitions do not resolve however the script spells the `using`.
AUTORTFM_DISABLE void AddAttributePackage(ISolarisIde& Ide)
{
    const uLang::TSPtr<uLang::CProgramBuildManager> BuildManager = Ide.GetBuildManager();
    if (!BuildManager.IsValid())
    {
        GodotVerse::ReportError(UTF8TEXT("No build manager; Godot attributes will be unavailable."));
        return;
    }

    const uLang::CSourceProject::SPackage& Package =
        BuildManager->FindOrAddSourcePackage(AttributePackageName, AttributePackageVersePath);
    Package._Package->SetVerseScope(uLang::EVerseScope::InternalUser);
    Package._Package->SetVerseVersion(Verse::Version::LatestUnstable);
    Package._Package->SetAllowExperimental(true);

    BuildManager->AddSourceSnippet(
        uLang::TSRef<uLang::CSourceDataSnippet>::New(
            uLang::CUTF8String(AttributeSnippetPath), uLang::CUTF8String(AttributePackageSource)),
        AttributePackageName,
        AttributePackageVersePath);
}

/// Whether the semantic program the IDE currently holds came from an analysis-only build.
/// Code generation hangs an IR package off every module, and the AST accessors the symbol
/// lookup walks assert rather than degrade when it finds one -- so the lookup has to be able
/// to tell the two shapes apart itself instead of trusting that a caller only asks after an
/// analysis.
bool GProgramIsAnalysisOnly = false;

/// Re-analyses the project with one file's text replaced. An empty Path replaces nothing and
/// simply re-analyses what the IDE already holds.
AUTORTFM_DISABLE bool RunCheck(const FUtf8String& Path, const FUtf8String& SourceText, TFunction<void(const FSolDiagnostic&)> Sink);

/// The buffer the program the IDE holds was last built from -- see ProgramAlreadyDescribes.
/// Written only from RunCheck, and read only after WaitForBackgroundCheck has joined the worker
/// that may have written it.
FUtf8String GAnalysedPath;
FUtf8String GAnalysedSource;

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

    // Registered for the life of the process, which is the life of the host: the handle
    // unregisters the feature when it is destroyed, and every build after that loses authorship.
    static uLang::TModularFeatureRegHandle<FGodotAuthorshipInjection> GodotAuthorship;

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
    AddAttributePackage(*Ide);
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

    // Whatever the result, the program now describes this text. Recorded so that the two
    // buffer-taking entry points -- completion and the argument hint -- can skip re-analysing
    // when the editor asks both about one keystroke, which it does on every call it is inside.
    GAnalysedPath = Path;
    GAnalysedSource = SourceText;
    return bAnalysed;
}

/// Whether the program already describes this exact buffer, so an analysis of it would be work
/// for the same answer. Only safe for a caller that wants the *program*: an analysis also reports
/// diagnostics, and skipping it skips those too.
AUTORTFM_DISABLE bool ProgramAlreadyDescribes(const FUtf8String& Path, const FUtf8String& SourceText)
{
    return !GAnalysedPath.IsEmpty() && GAnalysedPath == Path && GAnalysedSource == SourceText;
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

namespace {
/// FVerseFunction's package constructor dereferences the result of LookupPackage without checking
/// it, so asking for a function when the build failed crashes rather than returning invalid.
AUTORTFM_DISABLE bool ScriptPackageLoaded()
{
    return Verse::GlobalProgram && Verse::GlobalProgram->LookupPackage(ScriptPackageName) != nullptr;
}
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

/// True for the node types whose locus spans a whole definition rather than just a reference to
/// one. They resolve, but only against their name -- see NarrowedLocus.
AUTORTFM_DISABLE bool IsDefinitionNode(uLang::EAstNodeType NodeType)
{
    using namespace uLang;
    return NodeType == EAstNodeType::Definition_Function
        || NodeType == EAstNodeType::Definition_Data
        || NodeType == EAstNodeType::Definition_TypeAlias;
}

/// The Vst node carrying just the *name* of a definition, given the node carrying the whole thing.
///
/// Everything else a declaration is made of describes something other than the definition itself:
/// `<public>` and `<override>` are specifiers, `:float` is a type, `(Delta:float)` is a list of
/// other definitions. Only the name means "this one". Narrowing to the definition's first child is
/// not enough -- for `PhysicsUpdate<override>(Delta:float):void` that child still spans the
/// specifier, the parameters and the return type, which is why hovering any of them used to
/// describe the method.
///
/// The name is at the leading edge, so this descends first children: a TypeSpec's is what is being
/// typed, a PrePostCall's is the callee, and both bottom out at the Identifier. An Identifier's own
/// locus excludes its attributes, which hang off its Aux rather than its children.
AUTORTFM_DISABLE const Verse::Vst::Node* DefinitionNameNode(const Verse::Vst::Node& Vst)
{
    const Verse::Vst::Node* Node = &Vst;
    // Bounded rather than while(true): a malformed tree must not spin here.
    for (int32 Depth = 0; Depth < 8; ++Depth)
    {
        if (Node->IsA<Verse::Vst::Identifier>())
        {
            return Node;
        }
        if (Node->GetChildCount() == 0)
        {
            return nullptr;
        }
        Node = &*Node->GetChildren()[0];
    }
    return nullptr;
}

/// A definition's own locus runs from its first attribute to the end of its body, so matching the
/// cursor against it would resolve every blank column inside a function to that function. Only its
/// name is tight enough to mean the author pointed at it.
AUTORTFM_DISABLE const Verse::Vst::Node* NarrowedLocus(const uLang::CAstNode& AstNode, const Verse::Vst::Node& Vst)
{
    if (!IsDefinitionNode(AstNode.GetNodeType()))
    {
        return &Vst;
    }
    return Vst.GetChildCount() > 0 ? DefinitionNameNode(*Vst.GetChildren()[0]) : nullptr;
}

/// The definition an identifier node resolves to, or null for a node that is not one.
AUTORTFM_DISABLE const uLang::CDefinition* ReferencedDefinition(const uLang::CAstNode& AstNode,
                                                                const uLang::CSemanticProgram& Program,
                                                                vh_lookup_kind& OutKind)
{
    using namespace uLang;

    switch (AstNode.GetNodeType())
    {
    // A definition is its own best answer at its name: hovering `Ready` where it is declared
    // should describe Ready, not decline because nothing refers to it there.
    case EAstNodeType::Definition_Function:
        OutKind = VH_LOOKUP_FUNCTION;
        return &*static_cast<const CExprFunctionDefinition&>(AstNode)._Function;

    case EAstNodeType::Definition_Data:
        OutKind = VH_LOOKUP_DATA;
        return &*static_cast<const CExprDataDefinition&>(AstNode)._DataMember;

    case EAstNodeType::Definition_TypeAlias:
        OutKind = VH_LOOKUP_TYPE_ALIAS;
        return &*static_cast<const CExprTypeAliasDefinition&>(AstNode)._TypeAlias;

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
            const Verse::Vst::Node* Locus = NarrowedLocus(AstNode, *Vst);
            if (Locus != nullptr && LocusContains(Locus->Whence(), Row, Column)
                && FULangConversionUtils::ULangStrToFUtf8String(Vst->GetSnippetPath()).Equals(Path, ESearchCase::IgnoreCase))
            {
                vh_lookup_kind Kind = VH_LOOKUP_UNKNOWN;
                if (const uLang::CDefinition* Definition = ReferencedDefinition(AstNode, Program, Kind))
                {
                    // A child's locus is contained in its parent's, so the deepest node visited
                    // that still contains the cursor is the innermost -- last write wins.
                    Found = Definition;
                    FoundKind = Kind;
                    bFoundIsDefinition = IsDefinitionNode(AstNode.GetNodeType());
                }
            }

            // A parameter is not in the tree this walks: analysis moves it onto the function's
            // signature, leaving only its type behind in the AST. So it is asked of the function
            // whose declaration the cursor is inside, which is also the only place it can be --
            // and it is asked after the walk above, so it wins over the enclosing function.
            if (AstNode.GetNodeType() == uLang::EAstNodeType::Definition_Function
                && LocusContains(Vst->Whence(), Row, Column)
                && FULangConversionUtils::ULangStrToFUtf8String(Vst->GetSnippetPath()).Equals(Path, ESearchCase::IgnoreCase))
            {
                const uLang::CFunction& Function = *static_cast<const uLang::CExprFunctionDefinition&>(AstNode)._Function;
                for (const uLang::CDataDefinition* Param : Function._Signature.GetParams())
                {
                    const uLang::CExpressionBase* ParamAst = Param ? Param->GetAstNode() : nullptr;
                    const Verse::Vst::Node* ParamVst = ParamAst ? ParamAst->GetMappedVstNode() : nullptr;
                    const Verse::Vst::Node* ParamName = ParamVst ? DefinitionNameNode(*ParamVst) : nullptr;
                    if (ParamName != nullptr && LocusContains(ParamName->Whence(), Row, Column))
                    {
                        Found = Param;
                        FoundKind = VH_LOOKUP_DATA;
                        bFoundIsDefinition = true;
                        break;
                    }
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
    bool bFoundIsDefinition{false};
};

/// Whether a definition is one of its enclosing function's parameters. Locals live in a control
/// scope rather than the function scope, so the scope kind alone nearly answers it -- but the
/// signature is asked directly, because "nearly" is how a `where` clause's type variable would end
/// up documented as an argument.
AUTORTFM_DISABLE bool IsFunctionParameter(const uLang::CDefinition& Definition)
{
    const uLang::CDataDefinition* Data = Definition.AsNullable<uLang::CDataDefinition>();
    if (!Data || Definition._EnclosingScope.GetKind() != uLang::CScope::EKind::Function)
    {
        return false;
    }
    const uLang::CFunction& Function = static_cast<const uLang::CFunction&>(Definition._EnclosingScope);
    for (const uLang::CDataDefinition* Param : Function._Signature.GetParams())
    {
        if (Param == Data)
        {
            return true;
        }
    }
    return false;
}

/// Where a definition was written, or nothing for one compiled from a package the project does
/// not own -- the generated Godot API, Verse's own library. Those still describe fine.
AUTORTFM_DISABLE void FillLocation(const uLang::CDefinition& Definition, FUtf8String& OutPath, int32& OutLine, int32& OutColumn)
{
    if (const uLang::CExpressionBase* DefinitionNode = Definition.GetAstNode())
    {
        if (const Verse::Vst::Node* Vst = DefinitionNode->GetMappedVstNode())
        {
            const Verse::SLocus& Whence = Vst->Whence();
            OutPath = FULangConversionUtils::ULangStrToFUtf8String(Vst->GetSnippetPath());
            OutLine = (int32)Whence.BeginRow();
            OutColumn = (int32)Whence.BeginColumn();
        }
    }
}

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
    OutDesc.Owner = FUtf8String(Definition._EnclosingScope.GetScopeName().AsCString());

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

    OutDesc.bIsParameter = IsFunctionParameter(Definition);

    FillLocation(Definition, OutDesc.Path, OutDesc.Line, OutDesc.Column);

    // Only at a declaration. A call site already resolves to the implementation that will run,
    // and redirecting that to the parent would be wrong rather than merely unhelpful.
    OutDesc.bIsDefinition = Visitor.bFoundIsDefinition;
    if (OutDesc.bIsDefinition)
    {
        if (const uLang::CDefinition* Overridden = Definition.GetOverriddenDefinition())
        {
            OutDesc.OverriddenOwner = FUtf8String(Overridden->_EnclosingScope.GetScopeName().AsCString());
            FillLocation(*Overridden, OutDesc.OverriddenPath, OutDesc.OverriddenLine, OutDesc.OverriddenColumn);
        }
    }
    return true;
}

namespace {

/// Everything the completion walk needs out of one pass over the AST.
struct AUTORTFM_DISABLE FCompletionVisitor : public uLang::SAstVisitor
{
    FCompletionVisitor(const FUtf8String& InPath, uint32 InRow, uint32 InColumn, const uLang::CScope* InDefaultScope)
        : Path(InPath)
        , Row(InRow)
        , Column(InColumn)
        , Scope(InDefaultScope)
    {
    }

    virtual void Visit(const char* FieldName, uLang::CAstNode& AstNode) override { VisitElement(AstNode); }

    virtual void VisitElement(uLang::CAstNode& AstNode) override
    {
        using namespace uLang;

        const Verse::Vst::Node* Vst = AstNode.GetMappedVstNode();
        if (Vst && FULangConversionUtils::ULangStrToFUtf8String(Vst->GetSnippetPath()).Equals(Path, ESearchCase::IgnoreCase))
        {
            bSawPath = true;
            const bool bContainsCursor = LocusContains(Vst->Whence(), Row, Column);

            // The scope to complete in, and the one to test accessibility against. A definition's
            // locus spans its whole body, so this is the enclosing declaration rather than the
            // thing under the cursor -- the opposite of the narrowing the lookup wants.
            if (bContainsCursor)
            {
                if (AstNode.GetNodeType() == EAstNodeType::Definition_Function)
                {
                    Scope = &*static_cast<const CExprFunctionDefinition&>(AstNode)._Function;
                    Locals.Empty();
                }
                else if (AstNode.GetNodeType() == EAstNodeType::Definition_Class)
                {
                    Scope = &static_cast<const CExprClassDefinition&>(AstNode)._Class;
                    Locals.Empty();
                }
            }

            // Anything declared earlier in the enclosing definition. Block scoping is not
            // consulted: a local from a sibling `if` branch is offered too, which over-offers
            // rather than hiding the name the author is reaching for.
            if (AstNode.GetNodeType() == EAstNodeType::Definition_Data
                && (Vst->Whence().BeginRow() < Row || (Vst->Whence().BeginRow() == Row && Vst->Whence().BeginColumn() < Column)))
            {
                Locals.AddUnique(&*static_cast<const CExprDataDefinition&>(AstNode)._DataMember);
            }

            // The receiver of a `.`, kept innermost-wins. Definition nodes span their bodies and
            // so would swallow any cursor inside one; an error node's type is unknown by
            // construction -- but its analysed children survive it, which is exactly what lets a
            // half-typed member still name a receiver.
            if (bContainsCursor && IsReceiverCandidate(AstNode.GetNodeType()))
            {
                Expr = &static_cast<const CExpressionBase&>(AstNode);
            }
        }
        AstNode.VisitChildren(*this);
    }

    /// Everything but the containing contexts, the definitions and the error node is a
    /// CExpressionBase, and the visitor only ever sees one node type per class.
    static bool IsReceiverCandidate(uLang::EAstNodeType NodeType)
    {
        using namespace uLang;
        return !IsDefinitionNode(NodeType)
            && NodeType != EAstNodeType::Error_
            && NodeType != EAstNodeType::Context_Project
            && NodeType != EAstNodeType::Context_CompilationUnit
            && NodeType != EAstNodeType::Context_Package
            && NodeType != EAstNodeType::Context_Snippet
            && NodeType != EAstNodeType::Definition_Module
            && NodeType != EAstNodeType::Definition_Enum
            && NodeType != EAstNodeType::Definition_Class;
    }

    FUtf8String Path;
    uint32 Row;
    uint32 Column;
    const uLang::CExpressionBase* Expr{nullptr};
    const uLang::CScope* Scope{nullptr};
    TArray<const uLang::CDataDefinition*> Locals;
    /// Whether this package holds the file at all. Without it the default scope would make every
    /// other package answer with its own root module.
    bool bSawPath{false};
};

/// Strips the wrappers a value picks up on its way out of a `var` member or an `option`, none of
/// which have members of their own. `Position` reads as `^vector2`; what has an `X` is vector2.
AUTORTFM_DISABLE const uLang::CNormalType* UnwrapToMemberBearingType(const uLang::CTypeBase* Type)
{
    using namespace uLang;
    if (!Type)
    {
        return nullptr;
    }
    const CNormalType* Normal = &Type->GetNormalType();
    for (int32 Depth = 0; Depth < 8; ++Depth)
    {
        if (const CPointerType* Pointer = Normal->AsNullable<CPointerType>())
        {
            Normal = &Pointer->PositiveValueType()->GetNormalType();
        }
        else if (const CReferenceType* Reference = Normal->AsNullable<CReferenceType>())
        {
            Normal = &Reference->PositiveValueType()->GetNormalType();
        }
        else if (const COptionType* Option = Normal->AsNullable<COptionType>())
        {
            Normal = &Option->GetValueType()->GetNormalType();
        }
        else
        {
            return Normal;
        }
    }
    return Normal;
}

/// Fills one item from a definition, or returns false for a definition that is not a name the
/// author could have written: the compiler generates a constructor and an archetype per class,
/// and neither is spellable.
AUTORTFM_DISABLE bool DescribeCompletion(const uLang::CDefinition& Definition, GodotVerse::FCompleteItem& OutItem)
{
    using namespace uLang;

    if (Definition.GetName().IsNull())
    {
        return false;
    }

    if (const CDataDefinition* Data = Definition.AsNullable<CDataDefinition>())
    {
        OutItem.Kind = VH_LOOKUP_DATA;
        OutItem.bIsVar = Data->IsVar();
        if (const CTypeBase* Type = Data->GetType())
        {
            OutItem.Type = FULangConversionUtils::ULangStrToFUtf8String(Type->AsCode());
        }
    }
    else if (const CFunction* Function = Definition.AsNullable<CFunction>())
    {
        if (Function->IsConstructor())
        {
            return false;
        }
        OutItem.Kind = VH_LOOKUP_FUNCTION;
        OutItem.ParamCount = Function->_Signature.NumParams();
        if (const CFunctionType* Type = Function->_Signature.GetFunctionType())
        {
            OutItem.Type = FULangConversionUtils::ULangStrToFUtf8String(Type->AsCode());
        }
    }
    else if (Definition.AsNullable<CClass>())
    {
        OutItem.Kind = VH_LOOKUP_CLASS;
    }
    else if (Definition.AsNullable<CEnumeration>() || Definition.AsNullable<CEnumerator>())
    {
        OutItem.Kind = VH_LOOKUP_ENUM;
    }
    else if (Definition.AsNullable<CTypeAlias>())
    {
        OutItem.Kind = VH_LOOKUP_TYPE_ALIAS;
    }
    else if (Definition.AsNullable<CModule>() || Definition.AsNullable<CModuleAlias>())
    {
        OutItem.Kind = VH_LOOKUP_MODULE;
    }
    else
    {
        return false;
    }

    OutItem.Name = FUtf8String(Definition.AsNameCString());
    OutItem.Owner = FUtf8String(Definition._EnclosingScope.GetScopeName().AsCString());
    int32 UnusedColumn = -1;
    FillLocation(Definition, OutItem.Path, OutItem.Line, UnusedColumn);
    return true;
}

/// Adds every definition a scope declares that the cursor's scope is allowed to see.
AUTORTFM_DISABLE void CollectScope(const uLang::CLogicalScope& From,
                                   const uLang::CScope* AccessFrom,
                                   TArray<GodotVerse::FCompleteItem>& OutItems)
{
    for (const uLang::TSRef<uLang::CDefinition>& Definition : From.GetDefinitions())
    {
        if (AccessFrom && !Definition->IsAccessibleFrom(*AccessFrom))
        {
            continue;
        }
        GodotVerse::FCompleteItem Item;
        if (DescribeCompletion(*Definition, Item))
        {
            OutItems.Add(MoveTemp(Item));
        }
    }
}

/// A class and everything it inherits. An override is declared in both, so the subclass' copy
/// wins by arriving first and the duplicate is dropped when the results are deduplicated.
AUTORTFM_DISABLE void CollectClassAndSupers(const uLang::CClass& Class,
                                            const uLang::CScope* AccessFrom,
                                            TArray<GodotVerse::FCompleteItem>& OutItems)
{
    // An interface is a CClass too, so the same walk covers `class(a, b)` as well as a superclass
    // chain; a diamond is dropped by the deduplication downstream rather than tracked here.
    for (const uLang::CClass* Current = &Class; Current; Current = Current->GetSuperClass())
    {
        CollectScope(*Current, AccessFrom, OutItems);
        for (const uLang::CClass* Interface : Current->_SuperInterfaces)
        {
            if (Interface)
            {
                CollectScope(*Interface, AccessFrom, OutItems);
            }
        }
    }
}

} // namespace

AUTORTFM_DISABLE bool GodotVerse::Complete(FUtf8StringView Path,
                                           const FUtf8String& SourceText,
                                           int32 Line,
                                           int32 Column,
                                           vh_complete_mode Mode,
                                           TArray<FCompleteItem>& OutItems)
{
    OutItems.Empty();

    if (!GIde.IsValid() || Line < 0 || Column < 0)
    {
        return false;
    }

    WaitForBackgroundCheck();

    // The buffer is mid-edit, so this analysis reports what the author has not finished writing:
    // discarding its diagnostics is the whole reason completion runs an analysis of its own rather
    // than borrowing CheckProject's. The result is ignored for the same reason -- a buffer that
    // does not analyse cleanly is the normal case here, and uLang keeps the sub-expressions it did
    // analyse either way. Only a program with no AST at all is fatal, which the checks below catch.
    if (!ProgramAlreadyDescribes(FUtf8String(Path), SourceText))
    {
        RunCheck(FUtf8String(Path), SourceText, [](const FSolDiagnostic&) {});
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

    for (const uLang::CAstCompilationUnit* CompilationUnit : Program->_AstProject->OrderedCompilationUnits())
    {
        for (const uLang::CAstPackage* Package : CompilationUnit->Packages())
        {
            const bool bIsUserPackage = Package->_VerseScope == uLang::EVerseScope::PublicUser
                || Package->_VerseScope == uLang::EVerseScope::InternalUser;
            if (!bIsUserPackage || !Package->_RootModule || !Package->_RootModule->GetAstPackage())
            {
                continue;
            }

            // A file that declares nothing still sits in its package's root module, which is the
            // scope a cursor at the top level of it completes in.
            FCompletionVisitor Visitor(FUtf8String(Path), (uint32)Line, (uint32)Column, Package->_RootModule);
            Package->_RootModule->GetAstPackage()->VisitChildren(Visitor);
            if (!Visitor.bSawPath)
            {
                continue;
            }

            if (Mode == VH_COMPLETE_MEMBERS)
            {
                if (!Visitor.Expr)
                {
                    continue;
                }
                const uLang::CNormalType* Type = UnwrapToMemberBearingType(Visitor.Expr->GetResultType(*Program));
                if (!Type)
                {
                    continue;
                }
                if (const uLang::CClass* Class = Type->AsNullable<uLang::CClass>())
                {
                    CollectClassAndSupers(*Class, Visitor.Scope, OutItems);
                }
                else if (const uLang::CEnumeration* Enumeration = Type->AsNullable<uLang::CEnumeration>())
                {
                    CollectScope(*Enumeration, Visitor.Scope, OutItems);
                }
                else if (const uLang::CModule* Module = Type->AsNullable<uLang::CModule>())
                {
                    CollectScope(*Module, Visitor.Scope, OutItems);
                }
            }
            else
            {
                for (const uLang::CDataDefinition* Local : Visitor.Locals)
                {
                    FCompleteItem Item;
                    if (DescribeCompletion(*Local, Item))
                    {
                        OutItems.Add(MoveTemp(Item));
                    }
                }

                // Out through the enclosing class and its superclasses, then the modules above it,
                // picking up each scope's `using` along the way -- which is where the whole
                // mirrored Godot API enters, since a script reaches it through `using {/Godot.org/Godot}`.
                for (const uLang::CScope* Current = Visitor.Scope; Current; Current = Current->GetParentScope())
                {
                    if (Current->GetKind() == uLang::CScope::EKind::Class)
                    {
                        CollectClassAndSupers(static_cast<const uLang::CClass&>(*Current), Visitor.Scope, OutItems);
                    }
                    else
                    {
                        CollectScope(Current->GetLogicalScope(), Visitor.Scope, OutItems);
                    }
                    for (const uLang::CLogicalScope* Using : Current->GetUsingScopes())
                    {
                        if (Using)
                        {
                            CollectScope(*Using, Visitor.Scope, OutItems);
                        }
                    }
                }
            }

            if (!OutItems.IsEmpty())
            {
                // A name that is in scope twice -- an override, or a class member shadowing an
                // imported one -- is one completion. The walk is ordered nearest-first, so the
                // copy that survives is the one that would actually resolve.
                TSet<FUtf8String> Seen;
                OutItems.RemoveAll([&Seen](const FCompleteItem& Item) {
                    bool bAlreadySeen = false;
                    Seen.Add(Item.Name, &bAlreadySeen);
                    return bAlreadySeen;
                });
                OutItems.Sort([](const FCompleteItem& Left, const FCompleteItem& Right) { return Left.Name < Right.Name; });
                return true;
            }
        }
    }

    return false;
}

AUTORTFM_DISABLE bool GodotVerse::ClassMembers(FUtf8StringView ClassName, TArray<FCompleteItem>& OutItems)
{
    OutItems.Empty();

    if (!GIde.IsValid())
    {
        return false;
    }
    WaitForBackgroundCheck();

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

    // The class' own scope only. What it inherits is documented by the class that declares it,
    // and for a mirrored Godot class that is Godot's own documentation rather than anything here.
    CollectScope(*Class, nullptr, OutItems);
    OutItems.Sort([](const FCompleteItem& Left, const FCompleteItem& Right) { return Left.Name < Right.Name; });
    return true;
}

AUTORTFM_DISABLE bool GodotVerse::SignatureAt(FUtf8StringView Path,
                                              const FUtf8String& SourceText,
                                              int32 Line,
                                              int32 Column,
                                              FSignatureDesc& OutDesc)
{
    OutDesc = FSignatureDesc{};

    if (!GIde.IsValid() || Line < 0 || Column < 0)
    {
        return false;
    }

    WaitForBackgroundCheck();
    // Shares the analysis completion just paid for: the editor asks for both about one keystroke.
    if (!ProgramAlreadyDescribes(FUtf8String(Path), SourceText))
    {
        RunCheck(FUtf8String(Path), SourceText, [](const FSolDiagnostic&) {});
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

    // The callee resolves the way any other identifier does, so this reuses the lookup walk rather
    // than the completion one: what is wanted is the definition at a position, not a scope.
    FLookupVisitor Visitor(*Program, FUtf8String(Path), (uint32)Line, (uint32)Column);
    for (const uLang::CAstCompilationUnit* CompilationUnit : Program->_AstProject->OrderedCompilationUnits())
    {
        for (const uLang::CAstPackage* Package : CompilationUnit->Packages())
        {
            const bool bIsUserPackage = Package->_VerseScope == uLang::EVerseScope::PublicUser
                || Package->_VerseScope == uLang::EVerseScope::InternalUser;
            if (!bIsUserPackage || !Package->_RootModule || !Package->_RootModule->GetAstPackage())
            {
                continue;
            }
            Package->_RootModule->GetAstPackage()->VisitChildren(Visitor);
        }
    }

    const uLang::CFunction* Function = Visitor.Found ? Visitor.Found->AsNullable<uLang::CFunction>() : nullptr;
    if (!Function)
    {
        return false;
    }

    OutDesc.Name = FUtf8String(Function->AsNameCString());
    if (const uLang::CFunctionType* Type = Function->_Signature.GetFunctionType())
    {
        OutDesc.Result = FULangConversionUtils::ULangStrToFUtf8String(Type->GetReturnType().AsCode());
    }

    for (const uLang::CDataDefinition* Param : Function->_Signature.GetParams())
    {
        FCompleteItem Item;
        if (Param && DescribeCompletion(*Param, Item))
        {
            OutDesc.Params.Add(MoveTemp(Item));
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

AUTORTFM_DISABLE void GodotVerse::TickScripts(double BudgetSeconds)
{
    PumpEventLoop(verse::FExecutionContext::GetActiveContext(), BudgetSeconds);
}
