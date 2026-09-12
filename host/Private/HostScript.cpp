// Copyright Epic Games, Inc. All Rights Reserved.

#include "HostScript.h"
#include "AutoRTFM.h"
#include "Containers/Map.h"
#include "Containers/UnrealString.h"
#include "GodotClasses.h"
#include "GodotMathLayout.gen.h"
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

/// What every generation's package name starts with; the generation number finishes it.
///
/// Not ISolIdeDataSource::DefaultDataSourceName, which is what ISolarisIde::AddDataSource would
/// have picked: the name has to be the host's to choose, because publishing a package marks its
/// exports LoaderImport and publishing that same package again asserts on the flag. A generation
/// is therefore a name no publish has used, and a name the IDE owns cannot be one.
constexpr const char* ScriptPackageBaseName = "GodotScripts";
constexpr const char* ScriptVersePath = "/user@localhost";
constexpr const char* MainFunctionName = "Main(:[][]char,:[[]char][]char)";

/// Where the generated Godot API lives. A class resolving under this is one of the mirrors, which
/// is what separates a reference ClassDB already knows the name of from one the project declared.
constexpr const char* GodotVersePath = "/Godot.org/Godot";

/// A second package sharing the native package's verse path, so a script's existing
/// `using { /Godot.org/Godot }` reaches these definitions with no extra import.
constexpr const char* AttributePackageName = "GodotAttributes";
constexpr const char* AttributePackageVersePath = GodotVersePath;
constexpr const char* AttributeSnippetPath = "GodotAttributes.verse";

/// The attributes this bridge owns, as Verse source compiled in this process.
///
/// They cannot ship in host/Verse with the rest of the package: `class(attribute)` is refused
/// unless CScope::IsAuthoredByEpic(), which only FGodotAuthorshipInjection below grants -- and
/// that runs here, while host/Verse is compiled by VNI at UBT time, which nothing we build can
/// reach. Hence a runtime-only package for the one thing VNI will not accept.
///
/// None of them carries `@customattribhandler`. Epic's `editable` does, which is why applying it
/// obliges the host to load VerseSimulationMetadata and submit to that handler's rules -- the ones
/// written for UEFN's inspector, which refuse a struct that is not concrete and so refuse `color`.
/// An attribute with no handler is just a type applied to a definition, which is all this needs.
constexpr const char* AttributePackageSource =
    "# Registers the class it is applied to as a Godot global class, the way C#'s [GlobalClass]\n"
    "# does. A marker with no argument: the name Godot registers is the class's own, which the\n"
    "# one-top-level-name-per-file rule already pins to the file stem.\n"
    "@attribscope_class\n"
    "global_class<public> := class<computes>(attribute) {}\n"
    "\n"
    "# Sends the member to Godot's inspector, the way C#'s [Export] does. What kind of field it\n"
    "# gets is read off the member's declared type rather than said again here: a bounded number\n"
    "# is a range, an enum is a list of choices, a mirrored class is the node or resource a slot\n"
    "# will accept.\n"
    "@attribscope_data\n"
    "export<public> := class<computes>(attribute) {}\n"
    "\n"
    "# The inspector section the member opens, which every member declared after it joins until\n"
    "# one opens another -- Godot has no per-property section, so the section is a position in the\n"
    "# list. [ExportCategory], [ExportGroup] and [ExportSubgroup] in C#, and the same three nested\n"
    "# depths: a category is a heading, a group folds under it, a subgroup folds under that.\n"
    "@attribscope_data\n"
    "export_category_attribute<public> := class<computes>(attribute):\n"
    "    Name<public>:string\n"
    "\n"
    "export_category<public><constructor>(Name:string)<computes> := export_category_attribute:\n"
    "    Name := Name\n"
    "\n"
    "@attribscope_data\n"
    "export_group_attribute<public> := class<computes>(attribute):\n"
    "    Name<public>:string\n"
    "\n"
    "export_group<public><constructor>(Name:string)<computes> := export_group_attribute:\n"
    "    Name := Name\n"
    "\n"
    "@attribscope_data\n"
    "export_subgroup_attribute<public> := class<computes>(attribute):\n"
    "    Name<public>:string\n"
    "\n"
    "export_subgroup<public><constructor>(Name:string)<computes> := export_subgroup_attribute:\n"
    "    Name := Name\n";

/// One .verse file, as the toolchain wants it: a path, its text, and somewhere to cache the
/// parse.
///
/// The IDE's own ISolIdeDataSource is the thing this replaces, and the one capability it had
/// that matters is the one kept here -- text that can be replaced in place, which is what an
/// editor's unsaved buffer is. Everything else it carried (VPL widgets, disk round-trips,
/// mutation broadcasts) has no caller on this side.
class FHostSourceSnippet : public uLang::ISourceSnippet
{
public:
    FHostSourceSnippet(uLang::CUTF8String&& InPath, uLang::CUTF8String&& InText)
        : Path(uLang::Move(InPath))
        , Text(uLang::Move(InText))
    {
    }

    virtual uLang::CUTF8String GetPath() const override { return Path; }
    virtual void SetPath(const uLang::CUTF8String& InPath) override { Path = InPath; }
    virtual bool IsInMemoryOnly() const override { return true; }
    virtual uLang::TOptional<uLang::CUTF8String> GetText() const override { return Text; }
    virtual uLang::TOptional<Verse::Vst::TNodeRef<Verse::Vst::Snippet>> GetVst() const override { return Vst; }
    virtual void SetVst(Verse::Vst::TNodeRef<Verse::Vst::Snippet> Snippet) override { Vst = Snippet; }

    /// Replacing the text drops the cached parse with it. CToolchain::ProcessSnippet clones a
    /// cached VST it considers valid instead of reparsing, so a VST kept across a text change
    /// would have the build analyse the text that was just replaced.
    void SetText(uLang::CUTF8String&& NewText)
    {
        Text = uLang::Move(NewText);
        Vst.Reset();
    }

private:
    uLang::CUTF8String Path;
    uLang::CUTF8String Text;
    uLang::TOptional<Verse::Vst::TNodeRef<Verse::Vst::Snippet>> Vst;
};

using FMainFunction = TVerseFunction<FVerseResult(
    TVerseCall<void>, const TArray<verse::string>&, const TMap<verse::string, verse::string>&)>;

TSharedPtr<ISolarisIde> GIde;

/// The project source the IDE builds, kept because IncrementalizeProjectSource is asked for it
/// by the caller rather than reachable from the IDE.
TOptional<TSharedRef<ISolIdeSourceProject>> GSourceProject;

/// How many generations have been published, and the name of the newest one's package. Zero and
/// empty until the first build: every lookup that names the package answers nothing until then,
/// which is the state the editor is in before its first Play.
int32 GScriptGeneration = 0;
FUtf8String GScriptPackageName;

/// The package the *source project* currently holds, which is not the same thing: a build that
/// failed published nothing but left its source behind for analysis to report against, so this
/// is the name the next build has to retire while GScriptPackageName stays where it was.
FUtf8String GScriptSourcePackageName;

/// Whether a generation has ever been published. Not a latch against a second one -- that is the
/// point of the phase -- but the guard on everything that reads a semantic program, which does
/// not exist until the first build makes one.
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

/// Creates the package this project's scripts are built into, and makes it depend on everything
/// else the project holds.
///
/// The dependency list is the half of AddDataSource that is easy to miss: without it the native
/// package's definitions do not resolve however a script spells the `using`. Taken fresh for the
/// package named rather than once for the project, which is what lets a later generation depend
/// on the same set without inheriting an earlier generation's entry.
AUTORTFM_DISABLE void AddScriptPackage(uLang::CProgramBuildManager& BuildManager, const FUtf8String& PackageName)
{
    const uLang::CSourceProject::SPackage& Package =
        BuildManager.FindOrAddSourcePackage(
            FULangConversionUtils::FUtf8StringToULangStr(PackageName), ScriptVersePath);
    Package._Package->SetVerseScope(uLang::EVerseScope::InternalUser);
    Package._Package->SetVerseVersion(Verse::Version::LatestUnstable);
    Package._Package->SetAllowExperimental(true);

    uLang::TArray<uLang::CUTF8String> Dependencies;
    for (const uLang::CSourceProject::SPackage& Other : BuildManager.GetSourceProject()->_Packages)
    {
        if (&Other != &Package)
        {
            Dependencies.Add(Other._Package->GetName());
        }
    }
    Package._Package->SetDependencyPackages(uLang::Move(Dependencies));
}

/// Drops a generation's package out of the source project.
///
/// The generation that published it is still live in the VM and its instances still run; what
/// goes is only the *source* the next build compiles. Without this every class in the project
/// would be declared twice at the same verse path -- the retired generation's snippets and the
/// new one's -- and the build would report a redefinition for each.
AUTORTFM_DISABLE void RemoveScriptPackage(uLang::CProgramBuildManager& BuildManager, const FUtf8String& PackageName)
{
    if (PackageName.IsEmpty())
    {
        return;
    }

    uLang::TArray<uLang::CSourceProject::SPackage>& Packages = BuildManager.GetSourceProject()->_Packages;
    for (int32 Index = Packages.Num() - 1; Index >= 0; --Index)
    {
        if (FUtf8String(Packages[Index]._Package->GetName().AsCString()) == PackageName)
        {
            Packages.RemoveAt(Index);
        }
    }
}

/// The module a source file's definitions go into, creating it and every module on the way.
///
/// An empty path is the package's root module, which is where a project with no `.vmodule`
/// markers puts every one of its files.
AUTORTFM_DISABLE uLang::CSourceModule& FindOrAddModule(uLang::CSourceModule& Root, const FUtf8String& ModulePath)
{
    uLang::CSourceModule* Module = &Root;
    FUtf8String Remaining = ModulePath;
    while (!Remaining.IsEmpty())
    {
        FUtf8String Name;
        int32 Slash = INDEX_NONE;
        if (Remaining.FindChar(UTF8CHAR('/'), Slash))
        {
            Name = Remaining.Left(Slash);
            Remaining.RightChopInline(Slash + 1);
        }
        else
        {
            Name = Remaining;
            Remaining.Empty();
        }

        if (Name.IsEmpty())
        {
            continue;
        }

        const uLang::CUTF8String ULangName = FULangConversionUtils::FUtf8StringToULangStr(Name);
        if (uLang::TOptional<uLang::TSRef<uLang::CSourceModule>> Existing = Module->FindSubmodule(ULangName))
        {
            Module = Existing.GetValue().Get();
            continue;
        }

        uLang::TSRef<uLang::CSourceModule> Added = uLang::TSRef<uLang::CSourceModule>::New(ULangName);
        Module->_Submodules.Add(Added);
        Module = Added.Get();
    }
    return *Module;
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

/// The project's source files, in the order vh_compile_project listed them. Owned here rather
/// than by the IDE because the package is: see ScriptPackageName.
TArray<uLang::TSRef<FHostSourceSnippet>> GScriptSnippets;
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
    Verse::VPackage* Package = Verse::GlobalProgram ? Verse::GlobalProgram->LookupPackage(GScriptPackageName) : nullptr;
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

    // Epic's metadata attributes are declared @customattribhandler, so evaluating a module that
    // applies one asks ICustomAttributeHandler::FindHandlerForAttribute for a handler and fails
    // the whole build with "No custom handler for attribute: editable" when there is none. The
    // bridge's own attributes need no handler, but a script is free to import
    // /Verse.org/Simulation and apply one of Epic's. The handler is registered by
    // FVerseSimulationMetadataModule::StartupModule, and in a monolithic program linking the
    // module does not run that -- nothing loads it unless asked.
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
    GSourceProject = MaybeSourceProject;
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
    GScriptSnippets.Empty();
    GSourceProject.Reset();
    GIde.Reset();
    GScriptGeneration = 0;
    GScriptPackageName.Empty();
    GScriptSourcePackageName.Empty();
    GProjectBuilt = false;
}

AUTORTFM_DISABLE bool GodotVerse::CompileProject(const TArray<FScriptSource>& Sources, int32& OutGeneration)
{
    if (!EnsureIde())
    {
        return false;
    }

    const uLang::TSPtr<uLang::CProgramBuildManager> BuildManager = GIde->GetBuildManager();
    if (!BuildManager.IsValid())
    {
        ReportError(UTF8TEXT("No build manager; the project cannot be built."));
        return false;
    }

    // Read every file before touching the project, so a missing one leaves the previous
    // generation's source exactly as it was rather than half replaced.
    TArray<FUtf8String> Texts;
    Texts.Reserve(Sources.Num());
    for (const FScriptSource& Source : Sources)
    {
        FString SourceText;
        if (!FFileHelper::LoadFileToString(SourceText, *FString(Source.Path)))
        {
            ReportError(FUtf8String(TEXT("Failed to open Verse source file: ")) + Source.Path);
            return false;
        }
        Texts.Add(FUtf8String(SourceText));
    }

    RemoveScriptPackage(*BuildManager, GScriptSourcePackageName);
    GScriptSnippets.Empty(Sources.Num());

    const int32 Generation = GScriptGeneration + 1;
    const FUtf8String PackageName =
        FUtf8String(FString::Printf(TEXT("%hs_%d"), ScriptPackageBaseName, Generation));

    AddScriptPackage(*BuildManager, PackageName);
    const uLang::CSourceProject::SPackage& Package =
        BuildManager->FindOrAddSourcePackage(
            FULangConversionUtils::FUtf8StringToULangStr(PackageName), ScriptVersePath);

    for (int32 Index = 0; Index < Sources.Num(); ++Index)
    {
        uLang::TSRef<FHostSourceSnippet> Snippet = uLang::TSRef<FHostSourceSnippet>::New(
            FULangConversionUtils::FUtf8StringToULangStr(Sources[Index].Path),
            FULangConversionUtils::FUtf8StringToULangStr(Texts[Index]));
        GScriptSnippets.Add(Snippet);
        FindOrAddModule(*Package._Package->_RootModule, Sources[Index].ModulePath).AddSnippet(Snippet);
    }

    // Without this the build republishes the native VNI packages -- which are already loaded --
    // and aborts inside the async loader. It marks everything already compiled External so that
    // only the new generation's package is built.
    ISolarisModule::Get().IncrementalizeProjectSource(
        GSourceProject.GetValue(),
        uLang::SBuildParams{._LinkType = uLang::SBuildParams::ELinkParam::RequireComplete});

    FSolIdeBuildSettings Settings{.LinkSettings = uLang::SBuildParams::ELinkParam::RequireComplete};
    const bool bBuilt = GIde->BuildAll(Settings, MakeIdeDiagnostics(ForwardSolDiagnostic));
    GScriptSourcePackageName = PackageName;
    GProjectBuilt = true;
    if (!bBuilt)
    {
        // Nothing was published, so GScriptPackageName still names the last generation that was:
        // its classes keep resolving and its instances keep running, which is R-ITER-5. The
        // failed generation's source stays in the project, because it is what the next analysis
        // reports diagnostics against.
        return false;
    }

    GScriptGeneration = Generation;
    GScriptPackageName = PackageName;
    OutGeneration = Generation;

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

    for (const uLang::TSRef<FHostSourceSnippet>& Snippet : GScriptSnippets)
    {
        if (FUtf8String(Snippet->GetPath().AsCString()) == Path)
        {
            Snippet->SetText(FULangConversionUtils::FUtf8StringToULangStr(SourceText));
            break;
        }
    }

    // What cannot happen twice in a process is a build that *generates*. Publishing a package marks
    // each of its exports with EInternalObjectFlags::LoaderImport, and publishing that package again
    // asserts on the flag the previous publish left -- for any package, not just the native ones,
    // which are merely the first to get there. README.md's first constraint has the whole finding,
    // including why WITH_EDITOR is what decides it. A build that only analyses publishes nothing and
    // can be run as often as the editor types.
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

namespace {

/// Records Module's path if it declares Name, then recurses. Path is relative to the package
/// root, and an empty one is the root module itself -- which is skipped, because it is in scope
/// from everywhere and so is never the answer to "what should I import".
AUTORTFM_DISABLE void CollectModulesDeclaring(const uLang::CModule& Module,
                                              const FUtf8String& Path,
                                              FUtf8StringView Name,
                                              TArray<FUtf8String>& Out)
{
    if (!Path.IsEmpty())
    {
        for (const uLang::TSRef<uLang::CDefinition>& Definition : Module.GetDefinitions())
        {
            if (FUtf8StringView(FUtf8String(Definition->AsNameCString())).Equals(Name))
            {
                Out.AddUnique(Path);
                break;
            }
        }
    }

    for (const uLang::CModule* Submodule : Module.GetDefinitionsOfKind<uLang::CModule>())
    {
        const FUtf8String Name8 = FUtf8String(Submodule->AsNameCString());
        CollectModulesDeclaring(*Submodule, Path.IsEmpty() ? Name8 : Path + UTF8TEXT("/") + Name8, Name, Out);
    }
}

} // namespace

AUTORTFM_DISABLE bool GodotVerse::ResolveUnknownName(FUtf8StringView Name, TArray<FUtf8String>& OutModules)
{
    OutModules.Empty();
    if (!GIde.IsValid() || Name.IsEmpty())
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

    for (const uLang::CAstCompilationUnit* CompilationUnit : Program->_AstProject->OrderedCompilationUnits())
    {
        for (const uLang::CAstPackage* Package : CompilationUnit->Packages())
        {
            // The project's own packages only. A name that needs an import from the Godot mirror
            // is a different question, and the mirror is one module the whole project already
            // imports.
            const bool bIsUserPackage = Package->_VerseScope == uLang::EVerseScope::PublicUser
                || Package->_VerseScope == uLang::EVerseScope::InternalUser;
            if (!bIsUserPackage || !Package->_RootModule)
            {
                continue;
            }
            if (const uLang::CModule* Root = Package->_RootModule->GetModule())
            {
                CollectModulesDeclaring(*Root, FUtf8String(), Name, OutModules);
            }
        }
    }
    return true;
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
    return Verse::GlobalProgram && Verse::GlobalProgram->LookupPackage(GScriptPackageName) != nullptr;
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
    Verse::VPackage* Package = Verse::GlobalProgram ? Verse::GlobalProgram->LookupPackage(GScriptPackageName) : nullptr;
    if (!Package)
    {
        return nullptr;
    }

    // A qualified name decorates as (scope:)leaf, and the module is part of the scope:
    // `gameplay/player` is `(/user@localhost/gameplay:)player`. A root-module class has no module
    // to add and decorates exactly as it did before modules existed.
    FUtf8String Scope(ScriptVersePath);
    FUtf8String Leaf(ClassName);
    int32 LastSlash = INDEX_NONE;
    if (Leaf.FindLastChar(UTF8CHAR('/'), LastSlash))
    {
        Scope += UTF8TEXT("/");
        Scope += Leaf.Left(LastSlash);
        Leaf.RightChopInline(LastSlash + 1);
    }

    const FUtf8String Decorated = FUtf8String(UTF8TEXT("(")) + Scope + UTF8TEXT(":)") + Leaf;

    UClass* Found = nullptr;
    Verse::FRunningContext Context = Verse::FRunningContextPromise{};
    Context.EnterVM([&] {
        Verse::VClass* Class = Package->LookupDefinition<Verse::VClass>(FUtf8StringView(Decorated));
        if (!Class)
        {
            return;
        }
        UClass* NativeClass = Cast<UClass>(Class->GetOrCreateNativeType(Context));
        if (NativeClass && NativeClass->IsChildOf(verse::vh_object::StaticClass()))
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
/// The bridge's own attributes, declared in AttributePackageSource above and so sharing the verse
/// path a script already imports. The section ones name the attribute *class* rather than the
/// `<constructor>` function beside it: GetAttributeTextValue matches on the invocation's return
/// type, and a single string argument is the one attribute payload SOL-972 leaves readable.
constexpr const char* ExportAttributePath = "/Godot.org/Godot/export";
constexpr const char* ExportCategoryAttributePath = "/Godot.org/Godot/export_category_attribute";
constexpr const char* ExportGroupAttributePath = "/Godot.org/Godot/export_group_attribute";
constexpr const char* ExportSubgroupAttributePath = "/Godot.org/Godot/export_subgroup_attribute";

/// Read here as well as by verse_scan_class_decl on the Godot side, which answers the same question
/// off the source text because Godot asks it during the filesystem scan, before a host exists. The
/// two must agree: one decides whether a reference to the class can be exported, the other decides
/// whether Godot registers the name that reference would be filtered by.
constexpr const char* GlobalClassAttributePath = "/Godot.org/Godot/global_class";

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

/// The class every mirrored Godot class derives from. A member typed as one of its subclasses
/// holds a *reference* the scene fills in, not a value the script owns, which is the whole of why
/// such a member is treated differently from every other below.
AUTORTFM_DISABLE const uLang::CClass* GodotObjectClass(const uLang::CSemanticProgram& Program)
{
    return Program.FindDefinitionByVersePath<uLang::CClass>("/Godot.org/Godot/object");
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

/// The value type behind a member's declared type, with bOutIsOption saying whether an `option`
/// was wrapped around it.
///
/// A `var` member's declared type is a pointer around the value type. That is unwrapped
/// specifically rather than through CNormalType::GetInnerType, which also unwraps an array -- and
/// Verse's `string` is `[]char`, so that route reports every string as a char.
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

/// Which package declares a class, as far as a reference to it is concerned.
///
/// The two kinds that can be exported differ in how Godot knows the class at all: a mirrored class
/// names one ClassDB already has, while a class the project declares is known to Godot only if it
/// registered itself with `@global_class`. Asking the program to resolve the class's own path is
/// what proves which it is, and the third answer matters too -- a class nested inside another
/// resolves as neither, and has no name an inspector slot could be filtered by.
enum class EClassOrigin : uint8
{
    Other,
    Mirrored,
    Script,
};

AUTORTFM_DISABLE EClassOrigin ClassOriginOf(const uLang::CClass& Class, const uLang::CSemanticProgram& Program);

/// The nearest mirrored class in Class's own superclass chain, Class included.
///
/// This is what decides how a reference slot is drawn, and a class the project declares cannot answer
/// it: ClassDB has never heard of the name that class registered with Godot, so asking whether `Mover`
/// descends from Node gets "no" and the inspector falls back to a resource picker. Its nearest
/// mirrored ancestor -- `node2d` -- is a name ClassDB does know.
///
/// Empty for a chain that reaches `object` without passing a mirror, which is a reference to something
/// Godot draws no picker for either way.
AUTORTFM_DISABLE FUtf8String NativeClassOf(const uLang::CClass& Class, const uLang::CSemanticProgram& Program)
{
    for (const uLang::CClass* Current = &Class; Current != nullptr; Current = Current->GetSuperClass())
    {
        if (ClassOriginOf(*Current, Program) == EClassOrigin::Mirrored)
        {
            return FUtf8String(Current->AsNameCString());
        }
    }
    return FUtf8String();
}

AUTORTFM_DISABLE EClassOrigin ClassOriginOf(const uLang::CClass& Class, const uLang::CSemanticProgram& Program)
{
    const FUtf8String Name = FUtf8String(Class.AsNameCString());
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

AUTORTFM_DISABLE const FStructLayout* FindStructLayout(FUtf8StringView VerseName)
{
    for (const FStructLayout& Layout : verse_math::layouts)
    {
        if (VerseName.Equals(FUtf8StringView(reinterpret_cast<const UTF8CHAR*>(Layout.verse_name))))
        {
            return &Layout;
        }
    }
    return nullptr;
}

/// The layout of the struct a nested field holds.
/// The Godot type a reference wrapper class names -- `godot_array` is an Array -- or 0.
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

/// The decorated shape key of one of a mirrored struct's fields -- `(/Godot.org/Godot/vector2:)X`.
AUTORTFM_DISABLE FUtf8String StructFieldKey(const FStructLayout& Layout, const char* Field)
{
    return FUtf8String(UTF8TEXT("(")) + GodotVersePath + UTF8TEXT("/") + Layout.verse_name
        + UTF8TEXT(":)") + Field;
}

/// How many scalars a struct occupies on the wire, counting through its nested fields.
///
/// Not the field count: a transform3d has two fields and twelve components, and it is components
/// that cross.
AUTORTFM_DISABLE int32 StructFieldCount(const FStructLayout& Layout)
{
    int32 Count = 0;
    for (int32 Index = 0; Index < Layout.field_count; ++Index)
    {
        const FStructField& Field = Layout.fields[Index];
        if (Field.nested_tag == 0)
        {
            ++Count;
        }
        else if (const FStructLayout* Nested = FindStructLayoutByTag(Field.nested_tag))
        {
            Count += StructFieldCount(*Nested);
        }
    }
    return Count;
}

AUTORTFM_DISABLE void DescribeArrayElement(const uLang::CTypeBase* ElementType,
                                           const uLang::CSemanticProgram& Program,
                                           GodotVerse::FExportDesc& OutDesc)
{
    using namespace uLang;

    if (!ElementType)
    {
        return;
    }
    const CNormalType& Element = ElementType->GetNormalType();

    switch (Element.GetKind())
    {
    case ETypeKind::Logic:
        OutDesc.VariantTag = VH_VARIANT_ARRAY;
        OutDesc.ElementVariantTag = VH_VARIANT_BOOL;
        OutDesc.Reject = VH_EXPORT_OK;
        return;
    case ETypeKind::Int:
        OutDesc.VariantTag = VH_VARIANT_PACKED_INT64_ARRAY;
        OutDesc.Reject = VH_EXPORT_OK;
        return;
    case ETypeKind::Float:
        OutDesc.VariantTag = VH_VARIANT_PACKED_FLOAT64_ARRAY;
        OutDesc.Reject = VH_EXPORT_OK;
        return;
    case ETypeKind::Array:
        if (static_cast<const CArrayType&>(Element).IsStringType())
        {
            OutDesc.VariantTag = VH_VARIANT_PACKED_STRING_ARRAY;
            OutDesc.Reject = VH_EXPORT_OK;
        }
        return;
    default:
        break;
    }

    // A mirrored struct, each of which Godot has a packed array for. A reference is deliberately not
    // here: `[]node2d` cannot hold the empty element an array editor starts a new row as, which is
    // the same objection VH_EXPORT_OBJECT_NOT_OPTIONAL makes about a bare reference.
    if (const CClass* Class = Element.AsNullable<CClass>())
    {
        const FStructLayout* Layout = ClassOriginOf(*Class, Program) == EClassOrigin::Mirrored
            ? FindStructLayout(FUtf8StringView(Class->AsNameCString()))
            : nullptr;
        if (Layout)
        {
            OutDesc.VariantTag = Layout->packed_array_tag;
            OutDesc.Reject = VH_EXPORT_OK;
        }
    }
}

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
AUTORTFM_DISABLE void DescribeExportType(const uLang::CTypeBase* Type, const uLang::CSemanticProgram& Program, GodotVerse::FExportDesc& OutDesc)
{
    using namespace uLang;

    OutDesc.Reject = VH_EXPORT_UNSUPPORTED_TYPE;
    if (!Type)
    {
        return;
    }

    bool bIsOption = false;
    const CNormalType* Normal = &UnwrapDeclaredType(*Type, bIsOption);

    if (const CClass* Class = Normal->AsNullable<CClass>())
    {
        const CClass* ObjectClass = GodotObjectClass(Program);
        if (!ObjectClass || !Class->IsSubtypeOf(*ObjectClass))
        {
            // Not a reference, so an option around it is asking for an empty slot Godot has no way
            // to draw -- that much is true of a struct and of a class the script wrote alike.
            if (bIsOption)
            {
                OutDesc.Reject = VH_EXPORT_OPTION_NOT_OBJECT;
                return;
            }

            // A reference wrapper -- godot_array, dictionary, callable, signal_ref -- names a
            // Godot type that crosses as an id rather than as a value. Typed here so a method
            // taking one reports the right argument type to Godot; rejected for *export* in the
            // same breath, because the inspector has no editor for an arbitrary Array.
            if (const int32 ReferenceTag = ReferenceVariantTag(FUtf8StringView(Class->AsNameCString())))
            {
                OutDesc.Type = VH_TYPE_REF;
                OutDesc.VariantTag = ReferenceTag;
                OutDesc.Reject = VH_EXPORT_UNSUPPORTED_TYPE;
                return;
            }

            // A mirrored struct is a value the inspector draws with an editor of its own: a colour
            // picker, a pair of spinboxes. It crosses as the numbers it is made of, tagged with
            // which Godot type to rebuild from them.
            const FStructLayout* Layout = ClassOriginOf(*Class, Program) == EClassOrigin::Mirrored
                ? FindStructLayout(FUtf8StringView(Class->AsNameCString()))
                : nullptr;
            if (Layout)
            {
                OutDesc.Type = VH_TYPE_TUPLE;
                OutDesc.VariantTag = Layout->variant_tag;
                OutDesc.Reject = VH_EXPORT_OK;
                return;
            }

            OutDesc.Reject = VH_EXPORT_UNSUPPORTED_TYPE;
            return;
        }

        // A reference crosses as the handle it is, which is an int the consumer rebuilds as an
        // object; an optional one crosses as an option around that.
        const EClassOrigin Origin = ClassOriginOf(*Class, Program);
        OutDesc.Type = bIsOption ? VH_TYPE_OPTION : VH_TYPE_INT;
        OutDesc.VariantTag = VH_VARIANT_OBJECT;
        OutDesc.Hint = Origin == EClassOrigin::Script ? VH_EXPORT_HINT_SCRIPT_CLASS : VH_EXPORT_HINT_CLASS;
        OutDesc.HintString = FUtf8String(Class->AsNameCString());
        OutDesc.NativeClass = NativeClassOf(*Class, Program);

        // Nothing can force a value into an inspector slot, so a member that cannot hold the empty
        // case has a declared type the scene can always violate. The Verse spelling that compiles
        // without an option, `node2d{}`, is a handle of 0: a reference dead from birth, and
        // indistinguishable from one freed later.
        if (!bIsOption)
        {
            OutDesc.Reject = VH_EXPORT_OBJECT_NOT_OPTIONAL;
            return;
        }

        if (Origin == EClassOrigin::Script)
        {
            // Verse will let a member be typed as any class in the project, but the inspector
            // filters a slot by a Godot class name, and only `@global_class` gives the class one.
            const CClass* GlobalClassAttribute = Program.FindDefinitionByVersePath<CClass>(GlobalClassAttributePath);
            OutDesc.Reject = Class->HasAttributeSubclass(GlobalClassAttribute, Program)
                ? VH_EXPORT_OK
                : VH_EXPORT_SCRIPT_CLASS_NOT_GLOBAL;
            return;
        }

        OutDesc.Reject = Origin == EClassOrigin::Mirrored ? VH_EXPORT_OK : VH_EXPORT_UNSUPPORTED_TYPE;
        return;
    }

    if (bIsOption)
    {
        OutDesc.Reject = VH_EXPORT_OPTION_NOT_OBJECT;
        return;
    }

    if (const CEnumeration* Enumeration = Normal->AsNullable<CEnumeration>())
    {
        // The ordinal, which is what GDScript and C# store too -- including the trap that reordering
        // the enumerators reinterprets every scene already saved.
        OutDesc.Type = VH_TYPE_INT;
        OutDesc.VariantTag = VH_VARIANT_INT;
        OutDesc.Hint = VH_EXPORT_HINT_ENUM;
        OutDesc.HintString = EnumeratorList(*Enumeration);
        OutDesc.Reject = VH_EXPORT_OK;
        return;
    }

    switch (Normal->GetKind())
    {
    case ETypeKind::Logic:
        OutDesc.Type = VH_TYPE_LOGIC;
        OutDesc.VariantTag = VH_VARIANT_BOOL;
        OutDesc.Reject = VH_EXPORT_OK;
        break;

    case ETypeKind::Int:
    {
        OutDesc.Type = VH_TYPE_INT;
        OutDesc.VariantTag = VH_VARIANT_INT;
        OutDesc.Reject = VH_EXPORT_OK;
        const CIntType& IntType = static_cast<const CIntType&>(*Normal);
        OutDesc.bHasRangeMin = IntType.GetMin().IsFinite();
        OutDesc.bHasRangeMax = IntType.GetMax().IsFinite();
        OutDesc.RangeMin = OutDesc.bHasRangeMin ? (double)IntType.GetMin().GetFiniteInt() : 0.0;
        OutDesc.RangeMax = OutDesc.bHasRangeMax ? (double)IntType.GetMax().GetFiniteInt() : 0.0;
        if (OutDesc.bHasRangeMin || OutDesc.bHasRangeMax)
        {
            OutDesc.Hint = VH_EXPORT_HINT_RANGE;
        }
        break;
    }

    case ETypeKind::Float:
    {
        OutDesc.Type = VH_TYPE_FLOAT;
        OutDesc.VariantTag = VH_VARIANT_FLOAT;
        OutDesc.Reject = VH_EXPORT_OK;
        // Plain `float` reports an infinite minimum and a NaN maximum. Neither is finite, which is
        // the whole test -- and the reason it is asked of each bound rather than of the type.
        //
        // A strict bound needs no special case: `_X < 500.0` is the double below 500.0, and the
        // consumer rounds inward to its own step, which lands under 500 from either spelling.
        const CFloatType& FloatType = static_cast<const CFloatType&>(*Normal);
        OutDesc.bHasRangeMin = FMath::IsFinite(FloatType.GetMin());
        OutDesc.bHasRangeMax = FMath::IsFinite(FloatType.GetMax());
        OutDesc.RangeMin = OutDesc.bHasRangeMin ? FloatType.GetMin() : 0.0;
        OutDesc.RangeMax = OutDesc.bHasRangeMax ? FloatType.GetMax() : 0.0;
        if (OutDesc.bHasRangeMin || OutDesc.bHasRangeMax)
        {
            OutDesc.Hint = VH_EXPORT_HINT_RANGE;
        }
        break;
    }

    case ETypeKind::Char8:
    case ETypeKind::Char32:
        OutDesc.Type = VH_TYPE_CHAR;
        break;

    case ETypeKind::Array:
    {
        const CArrayType& ArrayType = static_cast<const CArrayType&>(*Normal);
        if (ArrayType.IsStringType())
        {
            OutDesc.Type = VH_TYPE_STRING;
            OutDesc.VariantTag = VH_VARIANT_STRING;
            OutDesc.Reject = VH_EXPORT_OK;
            break;
        }
        OutDesc.Type = VH_TYPE_ARRAY;
        DescribeArrayElement(ArrayType.GetElementType(), Program, OutDesc);
        break;
    }

    case ETypeKind::Map:
        OutDesc.Type = VH_TYPE_MAP;
        break;

    case ETypeKind::Tuple:
        OutDesc.Type = VH_TYPE_TUPLE;
        break;

    default:
        break;
    }
}
} // namespace

namespace {

/// What a script's class declares a member as, beyond what the value sitting in the slot can say.
///
/// Both marshalling directions need this, for the same reason in two shapes. A read cannot tell a
/// `?node2d` holding nothing from a `logic` holding false, because Verse spells an empty option and
/// false with the same cell. A write has to build a value of the member's declared class, and an
/// empty slot does not name one.
struct FMemberType
{
    const uLang::CDataDefinition* Member = nullptr;
    /// The class a reference member or parameter holds, and which package declares it. Null and
    /// Other for one of any other type.
    const uLang::CClass* ReferenceClass = nullptr;
    EClassOrigin ReferenceOrigin = EClassOrigin::Other;
    /// Whether it was declared `?node2d` rather than `node2d`. An *exported member* must be optional
    /// -- the inspector can leave a slot empty, and VH_EXPORT_OBJECT_NOT_OPTIONAL says so -- but a
    /// method argument always arrives with a value, so both spellings are legal there and the
    /// difference is only whether the value handed over is wrapped.
    bool bReferenceIsOption = false;
    /// The mirrored struct a member is declared as, which is where its field names come from --
    /// there is nothing in a value to read them off.
    const FStructLayout* Struct = nullptr;
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
};

/// The same description, for a type with no member behind it: a method's parameter or its result.
///
/// Everything DescribeMemberType knows comes from the declared type rather than from the
/// declaration, so a signature can be described exactly as a member is -- which is what lets one
/// pair of converters serve both field access and dispatch.
AUTORTFM_DISABLE FMemberType DescribeType(const uLang::CTypeBase* Type, const uLang::CSemanticProgram& Program)
{
    FMemberType Result;

    bool bIsOption = false;
    const uLang::CNormalType* Normal = Type ? &UnwrapDeclaredType(*Type, bIsOption) : nullptr;
    DescribeExportType(Type, Program, Result.Described);
    if (const uLang::CClass* Declared = Normal ? Normal->AsNullable<uLang::CClass>() : nullptr)
    {
        // Three things in the mirror are spelled as a class and are not a Godot *object*: the
        // sixteen math types, which are structs; the container wrappers, which carry a reference id;
        // and the parametric typed containers over them. Only what is left is a handle to build a
        // wrapper from, and only for it does the option/bare distinction below mean anything.
        const FUtf8StringView Name = FUtf8StringView(Declared->AsNameCString());
        const bool bIsContainer = ReferenceVariantTag(Name) != 0
            || Name.StartsWith(UTF8TEXT("typed_array"))
            || Name.StartsWith(UTF8TEXT("typed_dictionary"));
        const FStructLayout* const Layout = bIsOption ? nullptr : FindStructLayout(Name);
        if (Layout)
        {
            Result.Struct = Layout;
        }
        else if (bIsOption || !bIsContainer)
        {
            Result.ReferenceClass = Declared;
            Result.ReferenceOrigin = ClassOriginOf(*Declared, Program);
            Result.bReferenceIsOption = bIsOption;
        }
    }
    else if (const uLang::CEnumeration* Enumeration = Normal ? Normal->AsNullable<uLang::CEnumeration>() : nullptr)
    {
        for (const uLang::TSRef<uLang::CEnumerator>& Enumerator : Enumeration->GetDefinitionsOfKind<uLang::CEnumerator>())
        {
            (void)Enumerator;
            ++Result.EnumeratorCount;
        }
        Result.EnumerationName = FUtf8String(UTF8TEXT("("))
            + FULangConversionUtils::ULangStrToFUtf8String(
                  Enumeration->_EnclosingScope.GetScopePath('/', uLang::CScope::EPathMode::PrefixSeparator))
            + UTF8TEXT(":)") + FUtf8String(Enumeration->AsNameCString());
    }
    return Result;
}

AUTORTFM_DISABLE FMemberType DescribeMemberType(FUtf8StringView ClassName, FUtf8StringView FieldName)
{
    FMemberType Result;
    if (!GIde.IsValid())
    {
        return Result;
    }
    const uLang::TSPtr<uLang::CProgramBuildManager> BuildManager = GIde->GetBuildManager();
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
    for (const uLang::CClass* Cursor = Class;
         Cursor != nullptr && ClassOriginOf(*Cursor, *Program) == EClassOrigin::Script;
         Cursor = Cursor->GetSuperClass())
    {
        for (const uLang::TSRef<uLang::CDataDefinition>& Member : Cursor->GetDefinitionsOfKind<uLang::CDataDefinition>())
        {
            if (!FUtf8StringView(Member->AsNameCString()).Equals(FieldName))
            {
                continue;
            }
            Result = DescribeType(Member->GetType(), *Program);
            Result.Member = &*Member;
            return Result;
        }
    }
    return Result;
}

/// Reads Layout's fields off a struct value into a fresh block of OutStorage, in Layout's order, and
/// points OutValue at it. False if any field is missing or is not a number, which would otherwise
/// hand the consumer a tuple it cannot rebuild.
/// Appends a struct's scalar components to OutItems, walking into nested fields.
///
/// Recursive because Godot's math types are: a transform3d is a basis and a vector3, and the basis
/// is three vector3s. The wire carries the leaves in this order and nothing else, so the walk here
/// and the packer gen_verse_api.py emits have to agree -- which is why both come from one layout.
AUTORTFM_DISABLE bool ReadStructComponents(Verse::FRunningContext Context,
                                           Verse::VValueObject& Struct,
                                           const FStructLayout& Layout,
                                           TArray<vh_value>& OutItems)
{
    for (int32 Index = 0; Index < Layout.field_count; ++Index)
    {
        const FStructField& Field = Layout.fields[Index];
        Verse::VUniqueString& Key = Verse::VUniqueString::New(Context, FUtf8StringView(StructFieldKey(Layout, Field.name)));
        const Verse::FOpResult Read = Struct.LoadField(Context, Key);
        if (!Read.IsReturn())
        {
            return false;
        }

        if (Field.nested_tag != 0)
        {
            const FStructLayout* const Nested = FindStructLayoutByTag(Field.nested_tag);
            Verse::VValueObject* const Inner = Read.Value.DynamicCast<Verse::VValueObject>();
            if (!Nested || !Inner || !ReadStructComponents(Context, *Inner, *Nested, OutItems))
            {
                return false;
            }
            continue;
        }

        vh_value Item{};
        if (Field.is_int)
        {
            if (!Read.Value.IsInt())
            {
                return false;
            }
            Item.Type = VH_TYPE_INT;
            Item.Int = Read.Value.AsInt().AsInt64();
        }
        else
        {
            if (!Read.Value.IsFloat())
            {
                return false;
            }
            Item.Type = VH_TYPE_FLOAT;
            Item.Float = Read.Value.AsFloat().AsDouble();
        }
        OutItems.Add(Item);
    }
    return true;
}

AUTORTFM_DISABLE bool ReadStructValue(Verse::FRunningContext Context,
                                      Verse::VValueObject& Struct,
                                      const FStructLayout& Layout,
                                      GodotVerse::FFieldStorage& OutStorage,
                                      vh_value& OutValue)
{
    const int32 BlockIndex = OutStorage.Blocks.AddDefaulted();
    OutStorage.Blocks[BlockIndex].Reserve(StructFieldCount(Layout));
    if (!ReadStructComponents(Context, Struct, Layout, OutStorage.Blocks[BlockIndex]))
    {
        return false;
    }

    OutValue.Type = VH_TYPE_TUPLE;
    OutValue.VariantTag = Layout.variant_tag;
    OutValue.Seq.Items = OutStorage.Blocks[BlockIndex].GetData();
    OutValue.Seq.Count = OutStorage.Blocks[BlockIndex].Num();
    return true;
}

AUTORTFM_DISABLE bool ReadArrayValue(Verse::FRunningContext Context,
                                     const Verse::VArrayBase& Array,
                                     int32 Tag,
                                     int32 ElementTag,
                                     GodotVerse::FFieldStorage& OutStorage,
                                     vh_value& OutValue)
{
    const int32 Count = (int32)Array.Num();
    const FStructLayout* const Layout = FindStructLayoutByPackedTag(Tag);

    // Reserved for the array's own block plus one per struct element, so that filling it never moves
    // a block an element's vh_value already points into.
    OutStorage.Blocks.Reserve(OutStorage.Blocks.Num() + 1 + (Layout ? Count : 0));
    OutStorage.Strings.Reserve(Tag == VH_VARIANT_PACKED_STRING_ARRAY ? Count : 0);

    const int32 BlockIndex = OutStorage.Blocks.AddDefaulted();
    OutStorage.Blocks[BlockIndex].Reserve(Count);

    for (int32 Index = 0; Index < Count; ++Index)
    {
        const Verse::VValue Element = Array.GetValue((uint32)Index);
        vh_value Item{};

        if (Layout)
        {
            Verse::VValueObject* const Struct = Element.DynamicCast<Verse::VValueObject>();
            if (!Struct || !ReadStructValue(Context, *Struct, *Layout, OutStorage, Item))
            {
                return false;
            }
        }
        else if (Tag == VH_VARIANT_PACKED_INT64_ARRAY)
        {
            if (!Element.IsInt())
            {
                return false;
            }
            Item.Type = VH_TYPE_INT;
            Item.Int = Element.AsInt().AsInt64();
        }
        else if (Tag == VH_VARIANT_PACKED_FLOAT64_ARRAY)
        {
            if (!Element.IsFloat())
            {
                return false;
            }
            Item.Type = VH_TYPE_FLOAT;
            Item.Float = Element.AsFloat().AsDouble();
        }
        else if (Tag == VH_VARIANT_PACKED_STRING_ARRAY)
        {
            const Verse::VArrayBase* const Text = Element.DynamicCast<Verse::VArrayBase>();
            if (!Text)
            {
                return false;
            }
            const int32 StringIndex = OutStorage.Strings.Add(FUtf8String(Text->AsStringView()));
            Item.Type = VH_TYPE_STRING;
            Item.String.Utf8 = reinterpret_cast<const char*>(*OutStorage.Strings[StringIndex]);
            Item.String.Len = OutStorage.Strings[StringIndex].Len();
        }
        else if (Tag == VH_VARIANT_ARRAY && ElementTag == VH_VARIANT_BOOL)
        {
            if (!Element.IsLogic())
            {
                return false;
            }
            Item.Type = VH_TYPE_LOGIC;
            Item.Logic = Element.AsBool() ? 1 : 0;
        }
        else
        {
            return false;
        }

        OutStorage.Blocks[BlockIndex].Add(Item);
    }

    OutValue.Type = VH_TYPE_ARRAY;
    OutValue.VariantTag = Tag;
    OutValue.Seq.Items = OutStorage.Blocks[BlockIndex].GetData();
    OutValue.Seq.Count = Count;
    return true;
}

/// A fresh struct value of Class, with Layout's fields taken from Items.
///
/// The archetype is built from Layout's fields rather than taken from the class, and that is the
/// whole of the difficulty here. A field the class declares with an initializer -- which every field
/// of `vector2` has -- is *raised to the shape* as a `Constant`, shared by every instance, and a
/// constant has no per-instance slot to write: `VObject::SetField` reaches `VERSE_UNREACHABLE` on one
/// (`Inline/VVMObjectInline.h:73`). An archetype of `ObjectField` entries is what asks for the slots
/// instead, and `CreateField` is what marks each as present before it is written. This is the same
/// sequence `VNativeRef::FromNativeStruct` uses to hand a native struct to ordinary Verse code
/// (`VVMNativeRef.cpp:492-513`).
///
/// `NewVObject` rather than a lower-level allocation because it is what marks a struct deeply mutable
/// (`VVMClass.cpp:333-336`); an object built any other way does not compare or freeze like one.
AUTORTFM_DISABLE Verse::VClass* FindMirroredVClass(Verse::FRunningContext Context, FUtf8StringView ClassName);

/// Builds one mirrored struct from Count of the components at Items, consuming them in order.
///
/// The cursor is threaded through rather than indexed from zero per field, because a nested field
/// takes as many components as its own layout says -- a basis takes nine of a transform3d's twelve
/// and the origin takes the rest.
AUTORTFM_DISABLE Verse::VValue NewStructFrom(Verse::FRunningContext Context,
                                             const FStructLayout& Layout,
                                             const vh_value* Items,
                                             int32 ItemCount,
                                             int32& Cursor)
{
    Verse::VClass* const Class =
        FindMirroredVClass(Context, FUtf8StringView(reinterpret_cast<const UTF8CHAR*>(Layout.verse_name)));
    if (!Class)
    {
        return Verse::VValue();
    }

    TArray<Verse::VUniqueString*> Keys;
    TArray<Verse::VArchetype::VEntry> Entries;
    Keys.Reserve(Layout.field_count);
    Entries.Reserve(Layout.field_count);
    for (int32 Index = 0; Index < Layout.field_count; ++Index)
    {
        Verse::VUniqueString& Key =
            Verse::VUniqueString::New(Context, FUtf8StringView(StructFieldKey(Layout, Layout.fields[Index].name)));
        Keys.Add(&Key);
        Entries.Add(Verse::VArchetype::VEntry::ObjectField(Context, Key));
    }

    Verse::VArchetype& Archetype = Verse::VArchetype::New(Context, Verse::VValue(), Entries);
    Verse::VValueObject& Struct = Class->NewVObject(Context, Archetype);

    for (int32 Index = 0; Index < Layout.field_count; ++Index)
    {
        const FStructField& Field = Layout.fields[Index];
        Verse::VValue FieldValue;

        if (Field.nested_tag != 0)
        {
            const FStructLayout* const Nested = FindStructLayoutByTag(Field.nested_tag);
            if (!Nested)
            {
                return Verse::VValue();
            }
            FieldValue = NewStructFrom(Context, *Nested, Items, ItemCount, Cursor);
            if (FieldValue.IsUninitialized())
            {
                return Verse::VValue();
            }
        }
        else
        {
            if (Cursor >= ItemCount)
            {
                return Verse::VValue();
            }
            const vh_value& Item = Items[Cursor++];
            const double Number = Item.Type == VH_TYPE_FLOAT
                ? Item.Float
                : (Item.Type == VH_TYPE_INT ? (double)Item.Int : 0.0);
            FieldValue = Field.is_int
                ? Verse::VValue(Verse::VInt(Context, (int64)Number))
                : Verse::VValue(Verse::VFloat(Number));
        }

        if (!Struct.CreateField(Context, *Keys[Index])
            || !Struct.SetField(Context, *Keys[Index], FieldValue).IsReturn())
        {
            return Verse::VValue();
        }
    }
    return Verse::VValue(Struct);
}

AUTORTFM_DISABLE Verse::VValue NewStructValue(Verse::FRunningContext Context,
                                              Verse::VClass& Class,
                                              const FStructLayout& Layout,
                                              const vh_value* Items,
                                              int32 ItemCount)
{
    if (ItemCount != StructFieldCount(Layout))
    {
        return Verse::VValue();
    }
    int32 Cursor = 0;
    return NewStructFrom(Context, Layout, Items, ItemCount, Cursor);
}

AUTORTFM_DISABLE int64 HandleOf(Verse::VValue Value)
{
    UObject* Wrapper = Value.ExtractUObject();
    verse::vh_object* Shadow = Wrapper ? Cast<verse::vh_object>(Wrapper) : nullptr;
    return Shadow ? Shadow->Handle.Get() : 0;
}

/// The module-qualified name of a script class: `player` at the package root, `gameplay/player`
/// in a module. This is what every ClassNameUtf8 in the ABI carries.
///
/// Read back out of the UClass's own name, which is the Verse name mangled by
/// VNamedType::AppendMangledName: the module path with each `/` replaced by a separator, then the
/// class's own name. The separator is `-` for a Source package like this one (`_` is for VNI), and
/// no Verse identifier can contain either one, so undoing it is a substitution rather than a parse.
///
/// UVerseClass::PackageRelativeVersePath would be the direct answer and is not usable: the line
/// that sets it from the AST is commented out in VVMClass.cpp, so under VerseVM it is empty.
AUTORTFM_DISABLE FUtf8String QualifiedClassName(const UClass* Class)
{
    if (!Class)
    {
        return FUtf8String();
    }
    FString Name = Class->GetName();
    Name.ReplaceCharInline(TEXT('-'), TEXT('/'));
    return FUtf8String(Name);
}

/// The decorated shape key for a member of Object's own class. Factored out because the read and
/// write paths must agree on it exactly.
AUTORTFM_DISABLE FUtf8String ShapeKeyFor(FUtf8StringView ClassName, FUtf8StringView FieldName)
{
    return FUtf8String(UTF8TEXT("(")) + ScriptVersePath + UTF8TEXT("/")
        + FUtf8String(ClassName) + UTF8TEXT(":)") + FUtf8String(FieldName);
}

/// The shape entry for a member, and the class that declares it.
///
/// A shape keys a data member by its *declaring* class -- `(/user@localhost/base_entity:)Health`,
/// never the class the object happens to be. One script class deriving from another is what makes
/// those differ, and before that was tested the object's own name was always the right qualifier.
/// The walk stops where the names stop resolving, which is the first class outside the script
/// package: a mirrored class' members are Godot's own properties and are not in this shape at all.
AUTORTFM_DISABLE const Verse::VShape::VEntry* FindShapeField(Verse::FRunningContext Context,
                                                             UObject* Object,
                                                             FUtf8StringView FieldName,
                                                             FUtf8String& OutDeclaringClass)
{
    Verse::VShape& Shape = UVerseClass::GetShapeForLoadField(Context, Object->GetClass());
    for (const UClass* Cursor = Object->GetClass(); Cursor != nullptr; Cursor = Cursor->GetSuperClass())
    {
        const FUtf8String ClassName = QualifiedClassName(Cursor);
        Verse::VUniqueString& Name =
            Verse::VUniqueString::New(Context, FUtf8StringView(ShapeKeyFor(ClassName, FieldName)));
        if (const Verse::VShape::VEntry* Field = Shape.GetField(Name))
        {
            OutDeclaringClass = ClassName;
            return Field;
        }
    }
    return nullptr;
}

/// Builds a vh_value from a Verse value, given what its declaration says it is.
///
/// The declaration is not optional context. A read cannot tell a `?node2d` holding nothing from a
/// `logic` holding false, because Verse spells an empty option and false with the same cell; an
/// empty array carries no element type; and a struct's field order is nowhere in the value. Taking
/// the description rather than a member name is what lets a method's return value and a member
/// read share one conversion.
AUTORTFM_DISABLE bool ValueToWire(Verse::FRunningContext Context,
                                  Verse::VValue Value,
                                  const FMemberType& Declared,
                                  GodotVerse::FFieldStorage& OutStorage,
                                  vh_value& OutValue)
{
    // A reference, before the logic test rather than after it, because Verse's two spellings
    // collide: `true` is an option around `false`, and an empty option *is* `false`. A set
    // option wrapping a wrapper object is the one of the three the value alone identifies; an
    // empty one has to be told what the author declared, and anything else falls through to
    // the logic the cell equally well is.
    int64 ReferenceHandle = 0;
    bool bIsReference = false;
    if (const Verse::VOption* Option = Value.DynamicCast<Verse::VOption>())
    {
        ReferenceHandle = HandleOf(Option->GetValue());
        bIsReference = ReferenceHandle != 0;
    }
    else if (Value.IsFalse())
    {
        bIsReference = Declared.ReferenceClass != nullptr;
    }

    if (bIsReference)
    {
        // A handle the consumer rebuilds an object from, and for the empty case the empty
        // option -- which is a reference holding nothing, not a member that failed to read.
        OutValue.VariantTag = VH_VARIANT_OBJECT;
        if (ReferenceHandle != 0)
        {
            OutValue.Type = VH_TYPE_INT;
            OutValue.Int = ReferenceHandle;
        }
        else
        {
            OutValue.Type = VH_TYPE_OPTION;
            OutValue.Option = nullptr;
        }
    }
    else if (Value.IsLogic())
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
    else if (const Verse::VEnumerator* Enumerator = Value.DynamicCast<Verse::VEnumerator>())
    {
        // An enum member holds an enumerator, which is a cell and not a number -- so this is the
        // read, and `IsInt` above never sees one. The ordinal is what crosses, because that is
        // what Godot stores; the names reach only the inspector's dropdown, which is also why
        // reordering a Verse enum silently reinterprets every scene already saved.
        OutValue.Type = VH_TYPE_INT;
        OutValue.VariantTag = VH_VARIANT_INT;
        OutValue.Int = Enumerator->GetIntValue();
    }
    else if (Declared.Described.Type == VH_TYPE_REF)
    {
        // A reference wrapper the script is handing back. Its id is what crosses; the table entry
        // it names is still claimed by whatever Verse object holds it.
        const verse::godot_ref* Wrapper = Cast<verse::godot_ref>(Value.ExtractUObject());
        if (!Wrapper)
        {
            return false;
        }
        OutValue.Type = VH_TYPE_REF;
        OutValue.VariantTag = Declared.Described.VariantTag;
        OutValue.Ref = Wrapper->Ref.Get();
    }
    else if (Verse::VValueObject* Struct = Value.DynamicCast<Verse::VValueObject>())
    {
        // A mirrored struct: vector2, color. The fields are read by name, and the names come
        // from the declared type -- the value carries its field keys but not which order a
        // Godot Vector2 wants them in, and positions are the whole of what crosses.
        if (!Declared.Struct || !ReadStructValue(Context, *Struct, *Declared.Struct, OutStorage, OutValue))
        {
            return false;
        }
    }
    else if (const Verse::VArrayBase* Array = Value.DynamicCast<Verse::VArrayBase>())
    {
        // Verse `string` is `[]char`, so a string arrives as an array of char8 -- and so does
        // every other array, which is why the char case is settled first. VArrayBase rather than
        // VArray because a `var` of a container type holds a VMutableArray: the mutability lives
        // in the container itself, not in a reference around it.
        //
        // An *empty* array cannot be told apart this way, since it carries no element type, so
        // that one case goes back to what the author declared.
        const Verse::EArrayType ArrayType = Array->GetArrayType();
        const GodotVerse::FExportDesc& Desc = Declared.Described;
        const bool bIsString = ArrayType == Verse::EArrayType::Char8
            || ArrayType == Verse::EArrayType::Char32
            || Desc.Type == VH_TYPE_STRING;

        if (bIsString)
        {
            OutStorage.Text = FUtf8String(Array->AsStringView());
            OutValue.Type = VH_TYPE_STRING;
            OutValue.String.Utf8 = reinterpret_cast<const char*>(*OutStorage.Text);
            OutValue.String.Len = OutStorage.Text.Len();
        }
        else if (Desc.Type != VH_TYPE_ARRAY
                 || !ReadArrayValue(Context, *Array, Desc.VariantTag, Desc.ElementVariantTag, OutStorage, OutValue))
        {
            return false;
        }
    }
    else
    {
        return false;
    }

    return true;
}

/// Reads FieldName off Object. Returns false for a field the shape does not carry, and for any
/// Verse type with no vh_value counterpart.
AUTORTFM_DISABLE bool ReadFieldOf(UObject* Object, FUtf8StringView FieldName, vh_value& OutValue, GodotVerse::FFieldStorage& OutStorage)
{
    if (!Object)
    {
        return false;
    }

    OutValue = vh_value{};
    OutValue.VariantTag = VH_VARIANT_NIL;
    OutStorage.Text.Reset();
    OutStorage.Blocks.Reset();
    OutStorage.Strings.Reset();

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
        FUtf8String DeclaringClass;
        const Verse::VShape::VEntry* Field = FindShapeField(Context, Object, FieldName, DeclaringClass);
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

        bRead = ValueToWire(Context, Value, DescribeMemberType(DeclaringClass, FieldName),
                            OutStorage, OutValue);
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
    const uLang::CDataDefinition* Member = DescribeMemberType(ClassName, FieldName).Member;
    return Member != nullptr && Member->IsVar();
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

/// The UClass behind a mirrored Godot class -- node2d, texture2d -- which is what a reference to
/// one has to be built from.
///
/// Looked up across every package in the program rather than in a named one. A script's class lives
/// in the package the host itself compiles, whose name FindGodotClass can spell; a mirrored class
/// comes from the package VNI built alongside the module, whose VM name is assembled out of the
/// mount point and the C++ module name -- two things this file would be guessing at. The decorated
/// name identifies the class on its own, so the package it is found in does not need predicting.
AUTORTFM_DISABLE Verse::VClass* FindMirroredVClass(Verse::FRunningContext Context, FUtf8StringView ClassName)
{
    if (!Verse::GlobalProgram)
    {
        return nullptr;
    }
    const FUtf8String Decorated = FUtf8String(UTF8TEXT("(")) + GodotVersePath + UTF8TEXT(":)") + FUtf8String(ClassName);
    for (uint32 Index = 0; Index < Verse::GlobalProgram->NumPackages(); ++Index)
    {
        if (Verse::VClass* Class = Verse::GlobalProgram->GetPackage(Index).LookupDefinition<Verse::VClass>(FUtf8StringView(Decorated)))
        {
            return Class;
        }
    }
    return nullptr;
}

/// The VM's enumeration of that decorated name, looked up the way FindMirroredVClass looks up a
/// class: by walking the published packages, since nothing indexes them together.
AUTORTFM_DISABLE Verse::VEnumeration* FindVEnumeration(FUtf8StringView DecoratedName)
{
    if (!Verse::GlobalProgram || DecoratedName.IsEmpty())
    {
        return nullptr;
    }
    for (uint32 Index = 0; Index < Verse::GlobalProgram->NumPackages(); ++Index)
    {
        if (Verse::VEnumeration* Enumeration =
                Verse::GlobalProgram->GetPackage(Index).LookupDefinition<Verse::VEnumeration>(DecoratedName))
        {
            return Enumeration;
        }
    }
    return nullptr;
}

AUTORTFM_DISABLE UClass* FindMirroredClass(FUtf8StringView ClassName)
{
    UClass* Found = nullptr;
    Verse::FRunningContext Context = Verse::FRunningContextPromise{};
    Context.EnterVM([&] {
        if (Verse::VClass* Class = FindMirroredVClass(Context, ClassName))
        {
            Found = Cast<UClass>(Class->GetOrCreateNativeType(Context));
        }
    });
    return Found;
}

/// A fresh Verse wrapper around a Godot handle, which is what a mirrored-class member holds.
///
/// Built the way Instantiate builds a script's own object, and buildable that way for the same
/// reason: a mirrored class is ordinary Verse over the one native `object`, so its instance *is* a
/// UObject and none of the VM's own object allocation comes into it.
/// UVerseClass::PostInitInstance has run the class's Verse constructor by the time NewObject
/// returns, which leaves only the field C++ owns to fill in.
AUTORTFM_DISABLE UObject* NewMirroredWrapper(UClass* NativeClass, int64 Handle)
{
    UObject* Wrapper = NativeClass ? NewObject<UObject>(GetTransientPackage(), NativeClass) : nullptr;
    verse::vh_object* Shadow = Cast<verse::vh_object>(Wrapper);
    if (!Shadow)
    {
        return nullptr;
    }
    Shadow->Handle.Init(Handle, Shadow);
    return Wrapper;
}

/// What an optional reference member holds: an option around the object, or Verse's `false` for one
/// holding nothing.
/// The Verse class that wraps a reference of this Godot type.
AUTORTFM_DISABLE UClass* FindReferenceClass(int32 VariantTag)
{
    for (const verse_math::reference_type& Reference : verse_math::reference_types)
    {
        if (Reference.variant_tag == VariantTag)
        {
            return FindMirroredClass(FUtf8StringView(reinterpret_cast<const UTF8CHAR*>(Reference.verse_name)));
        }
    }
    return nullptr;
}

/// One reference wrapper, holding the id and owning it from here on.
///
/// A UObject rather than a VM cell: the id has to be released when Verse drops the value, and only
/// a UObject is told when that happens. godot_ref::BeginDestroy is the other half.
AUTORTFM_DISABLE UObject* NewReferenceWrapper(UClass* NativeClass, int64 Id)
{
    UObject* Wrapper = NativeClass ? NewObject<UObject>(GetTransientPackage(), NativeClass) : nullptr;
    verse::godot_ref* Shadow = Cast<verse::godot_ref>(Wrapper);
    if (!Shadow)
    {
        return nullptr;
    }
    Shadow->Ref.Init(Id, Shadow);
    return Wrapper;
}

AUTORTFM_DISABLE Verse::VValue ReferenceOption(Verse::FRunningContext Context, UObject* Referenced)
{
    return Referenced ? Verse::VValue(Verse::VOption::New(Context, Verse::VValue(Referenced)))
                      : Verse::VValue(Verse::GlobalFalse());
}

/// A Verse array holding Items, built as the Godot container Tag names it.
///
/// Mutability is taken from the array already in the slot, for the same reason a string's is: Verse
/// hangs it off the container, so a `var` holds a VMutableArray where a plain member holds a VArray,
/// and writing the wrong one leaves storage the interpreter later dies on. The element storage kind
/// is always VValue -- the narrower EArrayType cases are an optimisation the VM reads back through
/// GetValue either way, and an empty array in the slot has no kind to copy.
AUTORTFM_DISABLE Verse::VValue NewArrayValue(Verse::FRunningContext Context,
                                            bool bMutable,
                                            int32 Tag,
                                            int32 ElementTag,
                                            const vh_value* Items,
                                            int32 ItemCount)
{
    const FStructLayout* const Layout = FindStructLayoutByPackedTag(Tag);
    Verse::VClass* const StructClass =
        Layout ? FindMirroredVClass(Context, FUtf8StringView(reinterpret_cast<const UTF8CHAR*>(Layout->verse_name))) : nullptr;
    if (Layout && !StructClass)
    {
        return Verse::VValue();
    }

    TArray<Verse::VValue> Elements;
    Elements.Reserve(ItemCount);
    for (int32 Index = 0; Index < ItemCount; ++Index)
    {
        const vh_value& Item = Items[Index];
        if (Layout)
        {
            const Verse::VValue Element = NewStructValue(Context, *StructClass, *Layout, Item.Seq.Items, Item.Seq.Count);
            if (Element.IsUninitialized())
            {
                return Verse::VValue();
            }
            Elements.Add(Element);
        }
        else if (Tag == VH_VARIANT_PACKED_INT64_ARRAY)
        {
            Elements.Add(Verse::VValue(Verse::VInt(Context, Item.Type == VH_TYPE_FLOAT ? (int64)Item.Float : Item.Int)));
        }
        else if (Tag == VH_VARIANT_PACKED_FLOAT64_ARRAY)
        {
            Elements.Add(Verse::VValue(Verse::VFloat(Item.Type == VH_TYPE_INT ? (double)Item.Int : Item.Float)));
        }
        else if (Tag == VH_VARIANT_PACKED_STRING_ARRAY)
        {
            if (Item.Type != VH_TYPE_STRING)
            {
                return Verse::VValue();
            }
            const FUtf8StringView Utf8(reinterpret_cast<const UTF8CHAR*>(Item.String.Utf8), Item.String.Len);
            // An element's mutability follows its container's, which is not obvious and is load
            // bearing. Reading a `var` container hands out an immutable snapshot, and
            // VMutableArray::FreezeImpl makes one by freezing each element in turn -- so an element
            // has to be freezable. A VArray is not: every VArrayBase constructor sets the
            // deeply-mutable flag (VVMArrayBase.h:288-370) and nothing ever clears it, while VArray
            // has no FreezeImpl, so freezing one is the fatal "VCell subtype 'VArray' without
            // FreezeImpl override" rather than the no-op it looks like it should be.
            Elements.Add(bMutable ? Verse::VValue(Verse::VMutableArray::New(Context, Utf8))
                                  : Verse::VValue(Verse::VArray::New(Context, Utf8)));
        }
        else if (Tag == VH_VARIANT_ARRAY && ElementTag == VH_VARIANT_BOOL)
        {
            Elements.Add(Verse::VValue::FromBool(Item.Type == VH_TYPE_LOGIC ? Item.Logic != 0 : Item.Int != 0));
        }
        else
        {
            return Verse::VValue();
        }
    }

    const auto Init = [&Elements](uint32 Index) { return Elements[(int32)Index]; };
    if (bMutable)
    {
        Verse::VMutableArray& Array =
            Verse::VMutableArray::New(Context, 0, (uint32)ItemCount, Verse::EArrayType::VValue);
        for (const Verse::VValue& Element : Elements)
        {
            Array.AddValue(Context, Element);
        }
        return Verse::VValue(Array);
    }
    return Verse::VValue(Verse::VArray::New(Context, (uint32)ItemCount, Init));
}

/// Builds the value to write, given the one already in the slot. An uninitialized return means the
/// value has no representation in this member and nothing is written.
/// Builds a Verse value of the declared type from the wire.
///
/// The write path for *members* takes the class and the mutability off the value already in the
/// slot, which is the safest source when there is one. A method argument has no slot: nothing has
/// been assigned yet, so everything -- the struct's class, the array's element kind, the enum an
/// ordinal belongs to -- has to come from the declaration. That is the whole difference between
/// this and WriteFieldOf's builders, and it is why they are not one function.
///
/// Returns false for a wire value the declared type cannot accept, which is VH_ERR_ARGUMENT to the
/// caller rather than a runtime error: the script is not at fault for how it was called.
AUTORTFM_DISABLE bool WireToValue(Verse::FRunningContext Context,
                                  const vh_value& Value,
                                  const FMemberType& Declared,
                                  Verse::VValue& OutValue)
{
    const GodotVerse::FExportDesc& Desc = Declared.Described;

    // A reference arrives as a handle naming a Godot object, so the Verse wrapper has to be built
    // here -- and only a mirrored class can be built from a handle alone. A parameter typed as one
    // of the project's own classes has no spelling on this wire: the object it should receive
    // already exists as some node's instance, and there is nothing in a handle to find it by.
    if (Declared.ReferenceClass != nullptr)
    {
        if (Declared.ReferenceOrigin != EClassOrigin::Mirrored)
        {
            return false;
        }
        const int64 Handle = Value.Type == VH_TYPE_INT ? Value.Int : 0;
        if (Handle == 0)
        {
            // Only an optional parameter has a value to stand for nothing. A bare `node2d` refuses,
            // which reaches the caller as VH_ERR_ARGUMENT rather than as a runtime error: passing
            // null where the signature does not allow it is the caller's mistake.
            if (!Declared.bReferenceIsOption)
            {
                return false;
            }
            OutValue = ReferenceOption(Context, nullptr);
            return true;
        }
        UObject* const Referenced =
            NewMirroredWrapper(FindMirroredClass(FUtf8StringView(Declared.ReferenceClass->AsNameCString())), Handle);
        if (!Referenced)
        {
            return false;
        }
        // Wrapped only where the declaration asked for an option. Handing a `?node2d` to a parameter
        // declared `node2d` is what made every method on it unreachable: the script had an option
        // where it had written a node, and the first `.GetName()` died inside the interpreter rather
        // than failing to compile.
        OutValue = Declared.bReferenceIsOption ? ReferenceOption(Context, Referenced) : Verse::VValue(Referenced);
        return true;
    }

    // A reference wrapper: the id is the whole of the value, and the Verse object exists to hold
    // it and to release it when collected. Built through the UObject path rather than as a VM cell
    // for exactly that reason -- a cell has no destructor, and an id nobody releases is a leak.
    if (Desc.Type == VH_TYPE_REF)
    {
        const int64 Id = Value.Type == VH_TYPE_REF ? Value.Ref : (Value.Type == VH_TYPE_INT ? Value.Int : 0);
        UObject* const Wrapper = NewReferenceWrapper(FindReferenceClass(Desc.VariantTag), Id);
        if (!Wrapper)
        {
            return false;
        }
        OutValue = Verse::VValue(Wrapper);
        return true;
    }

    if (Declared.Struct != nullptr)
    {
        Verse::VClass* const StructClass =
            FindMirroredVClass(Context, FUtf8StringView(reinterpret_cast<const UTF8CHAR*>(Declared.Struct->verse_name)));
        if (!StructClass || Value.Type != VH_TYPE_TUPLE)
        {
            return false;
        }
        const Verse::VValue Built = NewStructValue(Context, *StructClass, *Declared.Struct, Value.Seq.Items, Value.Seq.Count);
        if (Built.IsUninitialized())
        {
            return false;
        }
        OutValue = Built;
        return true;
    }

    if (Desc.Type == VH_TYPE_ARRAY)
    {
        // A Godot Array or packed array arrives as a reference id, because that is what every
        // container is on this wire now. A Verse array is a value, so the contents have to be read
        // out before one can be built -- which is the whole difference between the two, and the
        // reason a script sees `[]float` where Godot has a PackedFloat32Array.
        GodotVerse::FCallArena ContentsArena;
        vh_value Contents{};
        const vh_value* Source = &Value;
        if (Value.Type == VH_TYPE_REF)
        {
            GodotVerse::FHostState& Host = GodotVerse::GetHost();
            if (!Host.Godot.RefContents
                || Host.Godot.RefContents(Host.Godot.Ctx, Value.Ref, &ContentsArena, &Contents) != VH_CALL_OK)
            {
                return false;
            }
            Source = &Contents;
        }
        if (Source->Type != VH_TYPE_ARRAY)
        {
            return false;
        }
        // Immutable: a parameter is a fresh binding the callee cannot assign through, so there is
        // no `var` container to match the way a member write has to.
        const Verse::VValue Built = NewArrayValue(Context, /*bMutable*/ false, Desc.VariantTag, Desc.ElementVariantTag,
                                                  Source->Seq.Items, Source->Seq.Count);
        if (Built.IsUninitialized())
        {
            return false;
        }
        OutValue = Built;
        return true;
    }

    // An enum parameter takes its ordinal, bounded by the enum the author declared rather than
    // clamped into it -- an ordinal with no enumerator is a caller that disagrees with the script
    // about the enum, which is worth reporting rather than silently reinterpreting.
    if (Declared.EnumeratorCount > 0)
    {
        if (Value.Type != VH_TYPE_INT || Value.Int < 0 || Value.Int >= Declared.EnumeratorCount)
        {
            return false;
        }
        Verse::VEnumeration* const Enumeration = FindVEnumeration(FUtf8StringView(Declared.EnumerationName));
        if (!Enumeration || Value.Int >= Enumeration->NumEnumerators)
        {
            return false;
        }
        OutValue = Verse::VValue(Enumeration->GetEnumeratorChecked((int32)Value.Int));
        return true;
    }

    switch (Value.Type)
    {
    case VH_TYPE_LOGIC:
        OutValue = Verse::VValue::FromBool(Value.Logic != 0);
        return true;
    case VH_TYPE_INT:
        // A float parameter handed an int is widened rather than refused: Godot spells 0 as an
        // integer Variant whatever the receiving type, so refusing would make `Process(0)`
        // unreachable from GDScript.
        if (Desc.Type == VH_TYPE_FLOAT)
        {
            OutValue = Verse::VValue(Verse::VFloat((double)Value.Int));
        }
        else
        {
            OutValue = Verse::VValue(Verse::VInt(Context, Value.Int));
        }
        return true;
    case VH_TYPE_FLOAT:
        OutValue = Verse::VValue(Verse::VFloat(Value.Float));
        return true;
    case VH_TYPE_STRING:
        OutValue = Verse::VValue(Verse::VArray::New(
            Context, FUtf8StringView(reinterpret_cast<const UTF8CHAR*>(Value.String.Utf8), Value.String.Len)));
        return true;
    default:
        return false;
    }
}

using FFieldValueBuilder = TFunctionRef<Verse::VValue(Verse::FRunningContext Context, Verse::VValue Current)>;

AUTORTFM_DISABLE bool WriteFieldWith(UObject* Object, FUtf8StringView FieldName, EFieldWrite Mode, FFieldValueBuilder MakeValue)
{
    if (!Object)
    {
        return false;
    }

    if (Mode == EFieldWrite::Assign && !IsVarMember(QualifiedClassName(Object->GetClass()), FieldName))
    {
        return false;
    }

    bool bWrote = false;
    Verse::FRunningContext Context = Verse::FRunningContextPromise{};
    Context.EnterVM([&] {
        FUtf8String DeclaringClass;
        const Verse::VShape::VEntry* Field = FindShapeField(Context, Object, FieldName, DeclaringClass);

        // A Constant entry lives in the shape itself rather than in the object, so it is shared by
        // every instance and cannot be assigned to.
        if (Field == nullptr || !Field->IsProperty())
        {
            return;
        }

        // A member's storage already holds a value in the exact representation the compiled code
        // expects, and matching it is the whole job: the VM does not re-check a slot it is told
        // holds a string, so a plausible-looking value of the wrong cell type reads back fine and
        // dies later inside the interpreter. That is why the builder is handed Current.
        Verse::VRestValue* const Slot = Field->Type == Verse::EFieldType::FVerseProperty
            ? Field->UProperty->ContainerPtrToValuePtr<Verse::VRestValue>(Object)
            : nullptr;
        const Verse::VValue Current = Slot ? Slot->Get(Context)
                                           : Verse::VNativeRef::Peek(Context, Object, Field->UProperty);

        const Verse::VValue NewValue = MakeValue(Context, Current);
        if (NewValue.IsUninitialized())
        {
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

/// Writes the object an optional reference member should hold, or nothing for null.
///
/// The caller is the one that has checked Referenced against the member's declared class. Neither
/// the shape nor the slot will: a slot told it holds a `?sprite2d` takes whatever object is put in
/// it, and the mistake surfaces the first time compiled code calls a method that is not there.
AUTORTFM_DISABLE bool WriteReferenceField(UObject* Object, FUtf8StringView FieldName, EFieldWrite Mode, UObject* Referenced)
{
    return WriteFieldWith(Object, FieldName, Mode, [Referenced](Verse::FRunningContext Context, Verse::VValue) {
        return ReferenceOption(Context, Referenced);
    });
}

AUTORTFM_DISABLE bool WriteFieldOf(UObject* Object, FUtf8StringView FieldName, const vh_value& Value, EFieldWrite Mode)
{
    if (!Object)
    {
        return false;
    }

    // A reference arrives as a handle, which names a Godot object and not a Verse one -- so the
    // wrapper has to be built here, and only the declared type says what to build. A member typed
    // as one of the project's own classes is refused: the object it should hold is the one that
    // node's own instance already is, and WriteInstanceFieldInstance is the way to hand that over.
    if (Value.VariantTag == VH_VARIANT_OBJECT)
    {
        const FMemberType Declared = DescribeMemberType(QualifiedClassName(Object->GetClass()), FieldName);
        if (Declared.ReferenceOrigin != EClassOrigin::Mirrored || !Declared.bReferenceIsOption)
        {
            return false;
        }
        // Built before the VM scope is entered, because constructing it runs the class's Verse
        // constructor through UVerseClass::PostInitInstance, which takes a context of its own.
        const int64 Handle = Value.Type == VH_TYPE_INT ? Value.Int : 0;
        UObject* Referenced = Handle != 0
            ? NewMirroredWrapper(FindMirroredClass(FUtf8StringView(Declared.ReferenceClass->AsNameCString())), Handle)
            : nullptr;
        if (Handle != 0 && !Referenced)
        {
            return false;
        }
        return WriteReferenceField(Object, FieldName, Mode, Referenced);
    }

    // An enum arrives as its ordinal, which is indistinguishable from any other int. The bound is
    // taken from the enum the author declared rather than from the VM's own enumerator count, which
    // is not the same number. An ordinal outside it is refused rather than clamped: a scene saved
    // against a longer version of the enum will carry one, and it has no enumerator to become.
    if (Value.Type == VH_TYPE_INT)
    {
        const int32 EnumeratorCount = DescribeMemberType(QualifiedClassName(Object->GetClass()), FieldName).EnumeratorCount;
        if (EnumeratorCount > 0 && (Value.Int < 0 || Value.Int >= EnumeratorCount))
        {
            return false;
        }
    }

    // A struct and an array both arrive as a sequence of numbers that says nothing about what it is:
    // the field names of the one and the element type of the other are carried only by the declared
    // type, which is where DescribeExportType already worked them out for the export list.
    if (Value.Type == VH_TYPE_TUPLE || Value.Type == VH_TYPE_ARRAY)
    {
        const FMemberType Declared = DescribeMemberType(QualifiedClassName(Object->GetClass()), FieldName);
        if (Declared.Described.Type != Value.Type)
        {
            return false;
        }

        if (Value.Type == VH_TYPE_TUPLE)
        {
            const FStructLayout* const Layout = Declared.Struct;
            if (!Layout)
            {
                return false;
            }
            return WriteFieldWith(Object, FieldName, Mode, [&Value, Layout](Verse::FRunningContext Context, Verse::VValue Current) {
                // The class is taken from the struct already in the slot, which is the discipline
                // every write here follows: the new value cannot be of a class the compiled code was
                // not already expecting.
                Verse::VRef* const Box = Current.DynamicCast<Verse::VRef>();
                const Verse::VValue Inner = Box ? Box->Get(Context) : Current;
                Verse::VValueObject* const Struct = Inner.DynamicCast<Verse::VValueObject>();
                return Struct ? NewStructValue(Context, Struct->GetClass(), *Layout, Value.Seq.Items, Value.Seq.Count)
                              : Verse::VValue();
            });
        }

        const int32 Tag = Declared.Described.VariantTag;
        const int32 ElementTag = Declared.Described.ElementVariantTag;
        return WriteFieldWith(Object, FieldName, Mode, [&Value, Tag, ElementTag](Verse::FRunningContext Context, Verse::VValue Current) {
            Verse::VRef* const Box = Current.DynamicCast<Verse::VRef>();
            const Verse::VValue Inner = Box ? Box->Get(Context) : Current;
            return NewArrayValue(Context, Inner.IsCellOfType<Verse::VMutableArray>(), Tag, ElementTag,
                                 Value.Seq.Items, Value.Seq.Count);
        });
    }

    return WriteFieldWith(Object, FieldName, Mode, [&Value](Verse::FRunningContext Context, Verse::VValue Current) {
        switch (Value.Type)
        {
        case VH_TYPE_LOGIC:
            return Verse::VValue::FromBool(Value.Logic != 0);
        case VH_TYPE_INT:
        {
            // An enum member holds an enumerator rather than a number, and the enumeration it belongs
            // to is reachable only from the enumerator already in the slot -- the usual rule here,
            // that the new value's kind comes from the one it replaces. WriteFieldOf has already
            // bounded the ordinal against the enum the author declared.
            Verse::VRef* const Box = Current.DynamicCast<Verse::VRef>();
            const Verse::VValue Inner = Box ? Box->Get(Context) : Current;
            if (Verse::VEnumerator* Enumerator = Inner.DynamicCast<Verse::VEnumerator>())
            {
                Verse::VEnumeration* const Enumeration = Enumerator->GetEnumeration();
                if (!Enumeration || Value.Int < 0 || Value.Int >= Enumeration->NumEnumerators)
                {
                    return Verse::VValue();
                }
                return Verse::VValue(Enumeration->GetEnumeratorChecked((int32)Value.Int));
            }
            return Verse::VValue(Verse::VInt(Context, Value.Int));
        }
        case VH_TYPE_FLOAT:
            return Verse::VValue(Verse::VFloat(Value.Float));
        case VH_TYPE_STRING:
        {
            // Verse hangs the mutability of a container off the container, not off a reference
            // around it: `var Label:string` holds a VMutableArray where a plain one holds a VArray.
            const FUtf8StringView Utf8(reinterpret_cast<const UTF8CHAR*>(Value.String.Utf8), Value.String.Len);
            Verse::VRef* const Box = Current.DynamicCast<Verse::VRef>();
            const Verse::VValue Inner = Box ? Box->Get(Context) : Current;
            return Inner.IsCellOfType<Verse::VMutableArray>()
                ? Verse::VValue(Verse::VMutableArray::New(Context, Utf8))
                : Verse::VValue(Verse::VArray::New(Context, Utf8));
        }
        default:
            return Verse::VValue();
        }
    });
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

AUTORTFM_DISABLE bool GodotVerse::WriteInstanceFieldInstance(FInstance* Instance, FUtf8StringView FieldName, const FInstance* Value)
{
    if (!Instance || !Instance->Object.IsValid())
    {
        return false;
    }

    UObject* Referenced = Value && Value->Object.IsValid() ? Value->Object.Get() : nullptr;
    const FMemberType Declared = DescribeMemberType(QualifiedClassName(Instance->Object->GetClass()), FieldName);
    if (!Declared.ReferenceClass)
    {
        return false;
    }

    // The class check the slot will not do. A mirrored member accepts an instance too, and should:
    // a `?node2d` assigned a node that carries a script is better off holding that script's own
    // object than a second wrapper around the same handle, which would give one node two identities.
    if (Referenced)
    {
        UClass* MemberClass = Declared.ReferenceOrigin == EClassOrigin::Script
            ? FindGodotClass(FUtf8StringView(Declared.ReferenceClass->AsNameCString()))
            : FindMirroredClass(FUtf8StringView(Declared.ReferenceClass->AsNameCString()));
        if (!MemberClass || !Referenced->GetClass()->IsChildOf(MemberClass))
        {
            return false;
        }
    }

    return WriteReferenceField(Instance->Object.Get(), FieldName,
                               Instance->bSealed ? EFieldWrite::Assign : EFieldWrite::Initialize, Referenced);
}

AUTORTFM_DISABLE bool GodotVerse::ReadInstanceField(const FInstance* Instance, FUtf8StringView FieldName, vh_value& OutValue, FFieldStorage& OutStorage)
{
    if (!Instance || !Instance->Object.IsValid())
    {
        return false;
    }
    return ReadFieldOf(Instance->Object.Get(), FieldName, OutValue, OutStorage);
}

AUTORTFM_DISABLE bool GodotVerse::ReadClassDefaultField(FUtf8StringView ClassName, FUtf8StringView FieldName, vh_value& OutValue, FFieldStorage& OutStorage)
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
            Result.AppendChar(UTF8CHAR('_'));
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

AUTORTFM_DISABLE bool GodotVerse::GetClassMethods(FUtf8StringView ClassName, TArray<FMethodDesc>& OutMethods)
{
    OutMethods.Reset();

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
            Out.bHasDefault = Param->HasInitializer();
            if (!Out.bHasDefault)
            {
                Desc.RequiredParamCount = Desc.Params.Num();
            }
        }

        const FMemberType ResultType = DescribeType(&Type->GetReturnType(), *Program);
        Desc.ResultType = ResultType.Described.Type;
        Desc.ResultVariantTag = ResultType.Described.VariantTag;

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
    TArray<const uLang::CClass*> Chain;
    for (const uLang::CClass* Cursor = Class;
         Cursor != nullptr && ClassOriginOf(*Cursor, *Program) == EClassOrigin::Script;
         Cursor = Cursor->GetSuperClass())
    {
        Chain.Insert(Cursor, 0);
    }

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

        FUtf8String DeclaredIn;
        FillLocation(*Member, DeclaredIn, Desc.Line, Desc.Column);

        OutExports.Add(MoveTemp(Desc));
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
/// not enough -- for `PhysicsProcess<override>(Delta:float):void` that child still spans the
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

        // The file's own scope, which is where a cursor inside no definition at all completes --
        // an attribute written above a class, most of all. A `using` is added to whatever scope
        // was open when it was analysed, and at the top of a file that is the snippet, so a
        // position defaulted to the package's root module instead sees none of the Godot API:
        // not `node2d`, not `Print`, and not the `global_class` the line is reaching for.
        //
        // Set before the narrowing below rather than beside it: a class or function containing
        // the cursor still wins, because the walk reaches it after its enclosing snippet.
        if (AstNode.GetNodeType() == EAstNodeType::Context_Snippet)
        {
            const CExprSnippet& Snippet = static_cast<const CExprSnippet&>(AstNode);
            if (Snippet._SemanticSnippet
                && FULangConversionUtils::ULangStrToFUtf8String(Snippet._Path).Equals(Path, ESearchCase::IgnoreCase))
            {
                bSawPath = true;
                Scope = Snippet._SemanticSnippet;
            }
        }

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

/// Everything a function's declaration spells after its name, as Verse source:
/// "(Delta:float)<transacts>:void". The function type cannot stand in for it -- the parameter
/// names live on the signature, and the type spells the same definition "float->void".
///
/// Effects come out relative to the function default, which is why an ordinary method's
/// signature carries no specifier at all: BuildEffectAttributeCode emits only what an author
/// would have had to write.
AUTORTFM_DISABLE FUtf8String SpellSignature(const uLang::CFunction& Function)
{
    const uLang::CFunctionType* Type = Function._Signature.GetFunctionType();
    if (!Type)
    {
        return FUtf8String();
    }

    uLang::CUTF8StringBuilder Builder;
    Builder.Append('(');
    const char* Separator = "";
    for (const uLang::CDataDefinition* Param : Function._Signature.GetParams())
    {
        if (!Param || !Param->GetType())
        {
            continue;
        }
        Builder.Append(Separator);
        Separator = ", ";
        Builder.Append(Param->AsNameCString());
        Builder.Append(':');
        Builder.Append(Param->GetType()->AsCode());
    }
    Builder.Append(')');
    Type->BuildEffectAttributeCode(Builder);
    Builder.Append(':');
    Builder.Append(Type->GetReturnType().AsCode());
    return FULangConversionUtils::ULangStrToFUtf8String(Builder.MoveToString());
}

/// Whether a subclass could redeclare a function with <override>, which is the three things the
/// analyzer refuses one for: it has to be a class member -- a module-level function has nothing to
/// override it from -- it must not be <final>, and it must not be a class var's <getter>/<setter>,
/// which DetectIncorrectOverrideAttribute rejects by name. The generated Godot mirror is built out
/// of those accessors, so leaving the last one out offers a few hundred overrides that do not
/// compile.
AUTORTFM_DISABLE bool IsOverridable(const uLang::CFunction& Function)
{
    if (Function._EnclosingScope.GetKind() != uLang::CScope::EKind::Class || Function._bIsAccessorOfSomeClassVar)
    {
        return false;
    }
    const uLang::CSemanticProgram& Program = Function._EnclosingScope.GetProgram();
    return !Function.GetPrototypeDefinition()->HasAttributeClass(Program._finalClass, Program);
}

/// Which of a scope's definitions the walk keeps.
enum class ECompleteFilter : uint8
{
    /// Every name the scope admits.
    Any,
    /// Only what may follow an `@`.
    Attributes,
};

/// Whether a definition is a name an `@` could be followed by.
///
/// Two shapes, because Verse spells a payload-carrying attribute as a call: `editable` is the
/// attribute class itself, while `@clamp_min("0.0")` names the `<constructor>` function beside
/// clamp_min_attribute, which is what makes the class out of the argument. `attribute` itself is
/// excluded -- it is the base every attribute derives from, and applying it means nothing.
AUTORTFM_DISABLE bool IsAttributeName(const uLang::CDefinition& Definition)
{
    using namespace uLang;

    const CClass* AttributeClass = Definition._EnclosingScope.GetProgram()._attributeClass;
    if (!AttributeClass)
    {
        return false;
    }

    if (const CClass* Class = Definition.AsNullable<CClass>())
    {
        return Class != AttributeClass && Class->IsSubtypeOf(*AttributeClass);
    }
    if (const CFunction* Function = Definition.AsNullable<CFunction>())
    {
        if (!Function->IsConstructor())
        {
            return false;
        }
        const CFunctionType* Type = Function->_Signature.GetFunctionType();
        const CClass* Result = Type ? Type->GetReturnType().GetNormalType().AsNullable<CClass>() : nullptr;
        return Result && Result->IsSubtypeOf(*AttributeClass);
    }
    return false;
}

/// Fills one item from a definition, or returns false for a definition that is not a name the
/// author could have written: the compiler generates a constructor and an archetype per class,
/// and neither is spellable.
AUTORTFM_DISABLE bool DescribeCompletion(const uLang::CDefinition& Definition, ECompleteFilter Filter, GodotVerse::FCompleteItem& OutItem)
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
        // An attribute's `<constructor>` is the one the author writes -- `@clamp_min("0.0")` --
        // and IsAttributeName has already established that this is one. Everywhere else a
        // constructor is the copy the compiler generated per class, which has no spelling.
        if (Function->IsConstructor() && Filter != ECompleteFilter::Attributes)
        {
            return false;
        }
        OutItem.Kind = VH_LOOKUP_FUNCTION;
        OutItem.ParamCount = Function->_Signature.NumParams();
        OutItem.Signature = SpellSignature(*Function);
        OutItem.bIsOverridable = IsOverridable(*Function);
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
                                   ECompleteFilter Filter,
                                   TArray<GodotVerse::FCompleteItem>& OutItems)
{
    for (const uLang::TSRef<uLang::CDefinition>& Definition : From.GetDefinitions())
    {
        if (AccessFrom && !Definition->IsAccessibleFrom(*AccessFrom))
        {
            continue;
        }
        if (Filter == ECompleteFilter::Attributes && !IsAttributeName(*Definition))
        {
            continue;
        }
        GodotVerse::FCompleteItem Item;
        if (DescribeCompletion(*Definition, Filter, Item))
        {
            OutItems.Add(MoveTemp(Item));
        }
    }
}

/// A class and everything it inherits. An override is declared in both, so the subclass' copy
/// wins by arriving first and the duplicate is dropped when the results are deduplicated.
AUTORTFM_DISABLE void CollectClassAndSupers(const uLang::CClass& Class,
                                            const uLang::CScope* AccessFrom,
                                            ECompleteFilter Filter,
                                            TArray<GodotVerse::FCompleteItem>& OutItems)
{
    // An interface is a CClass too, so the same walk covers `class(a, b)` as well as a superclass
    // chain; a diamond is dropped by the deduplication downstream rather than tracked here.
    for (const uLang::CClass* Current = &Class; Current; Current = Current->GetSuperClass())
    {
        CollectScope(*Current, AccessFrom, Filter, OutItems);
        for (const uLang::CClass* Interface : Current->_SuperInterfaces)
        {
            if (Interface)
            {
                CollectScope(*Interface, AccessFrom, Filter, OutItems);
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
                    CollectClassAndSupers(*Class, Visitor.Scope, ECompleteFilter::Any, OutItems);
                }
                else if (const uLang::CEnumeration* Enumeration = Type->AsNullable<uLang::CEnumeration>())
                {
                    CollectScope(*Enumeration, Visitor.Scope, ECompleteFilter::Any, OutItems);
                }
                else if (const uLang::CModule* Module = Type->AsNullable<uLang::CModule>())
                {
                    CollectScope(*Module, Visitor.Scope, ECompleteFilter::Any, OutItems);
                }
            }
            else
            {
                const ECompleteFilter Filter = Mode == VH_COMPLETE_ATTRIBUTES
                    ? ECompleteFilter::Attributes
                    : ECompleteFilter::Any;

                // A local is a value, and an attribute is a type applied to a declaration: no
                // local is ever what follows an `@`.
                if (Filter == ECompleteFilter::Any)
                {
                    for (const uLang::CDataDefinition* Local : Visitor.Locals)
                    {
                        FCompleteItem Item;
                        if (DescribeCompletion(*Local, Filter, Item))
                        {
                            OutItems.Add(MoveTemp(Item));
                        }
                    }
                }

                // Out through the enclosing class and its superclasses, then the modules above it,
                // picking up each scope's `using` along the way -- which is where the whole
                // mirrored Godot API enters, since a script reaches it through `using {/Godot.org/Godot}`.
                for (const uLang::CScope* Current = Visitor.Scope; Current; Current = Current->GetParentScope())
                {
                    if (Current->GetKind() == uLang::CScope::EKind::Class)
                    {
                        CollectClassAndSupers(static_cast<const uLang::CClass&>(*Current), Visitor.Scope, Filter, OutItems);
                    }
                    else
                    {
                        CollectScope(Current->GetLogicalScope(), Visitor.Scope, Filter, OutItems);
                    }
                    for (const uLang::CLogicalScope* Using : Current->GetUsingScopes())
                    {
                        if (Using)
                        {
                            CollectScope(*Using, Visitor.Scope, Filter, OutItems);
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
    CollectScope(*Class, nullptr, ECompleteFilter::Any, OutItems);
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
        if (Param && DescribeCompletion(*Param, ECompleteFilter::Any, Item))
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

    verse::vh_object* Shadow = CastChecked<verse::vh_object>(Instance);
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
/// `object` gives Ready, Process and PhysicsProcess empty bodies so a script can <override>
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
    FVerseFunction Base(Context, verse::vh_object::StaticClass()->GetDefaultObject(), DecoratedName);
    return !Base.IsValid() || Base.Function.Get() != Resolved.Function.Get();
}

AUTORTFM_DISABLE int32 GodotVerse::InstanceCall(FInstance* Instance,
                                                FUtf8StringView DecoratedName,
                                                const vh_value* Args,
                                                int32 ArgCount,
                                                vh_value& OutResult,
                                                FFieldStorage& OutStorage)
{
    OutResult = vh_value{};
    OutStorage.Text.Reset();
    OutStorage.Blocks.Reset();
    OutStorage.Strings.Reset();

    if (!Instance || !Instance->Object.IsValid())
    {
        return VH_ERR_STATE;
    }

    // The signature, for the parameter and result types. Asked of the semantic program rather than
    // of the VM because that is the only view that carries declared types -- the bytecode has
    // erased them by the time a VValue exists.
    const FUtf8String ClassName = QualifiedClassName(Instance->Object->GetClass());
    TArray<FMethodDesc> Methods;
    if (!GetClassMethods(FUtf8StringView(ClassName), Methods))
    {
        return VH_ERR_NOT_FOUND;
    }
    const FMethodDesc* Method = Methods.FindByPredicate(
        [DecoratedName](const FMethodDesc& Candidate) { return FUtf8StringView(Candidate.DecoratedName).Equals(DecoratedName); });
    if (!Method)
    {
        return VH_ERR_NOT_FOUND;
    }

    if (ArgCount < Method->RequiredParamCount || ArgCount > Method->Params.Num())
    {
        return VH_ERR_ARGUMENT;
    }

    FVerseFunction Resolved = LookupMethod(Instance, DecoratedName);
    if (!Resolved.IsValid())
    {
        return VH_ERR_NOT_FOUND;
    }

    Instance->bSealed = true;

    const uLang::TSPtr<uLang::CProgramBuildManager> BuildManager = GIde->GetBuildManager();
    const uLang::TSRef<uLang::CSemanticProgram>& Program = BuildManager->GetProgramContext()._Program;
    const uLang::CClass* const Class = Program->FindDefinitionByVersePath<uLang::CClass>(
        FULangConversionUtils::FUtf8StringViewToULangStringView(FUtf8String(ScriptVersePath) + UTF8TEXT("/") + ClassName));

    // Parameter descriptions are rebuilt here rather than carried on FMethodDesc, which holds only
    // what crosses the ABI. A description carries uLang pointers, and those are owned by a semantic
    // program the next analysis replaces.
    TArray<FMemberType> ParamTypes;
    FMemberType ResultTypeDesc;
    if (Class)
    {
        for (const uLang::TSRef<uLang::CFunction>& Function : Class->GetDefinitionsOfKind<uLang::CFunction>())
        {
            if (!FULangConversionUtils::ULangStrToFUtf8String(Function->GetDecoratedName()).Equals(FUtf8String(DecoratedName)))
            {
                continue;
            }
            for (const uLang::CDataDefinition* Param : Function->_Signature.GetParams())
            {
                ParamTypes.Add(Param ? DescribeType(Param->GetType(), *Program) : FMemberType{});
            }
            if (const uLang::CFunctionType* const Type = Function->_Signature.GetFunctionType())
            {
                ResultTypeDesc = DescribeType(&Type->GetReturnType(), *Program);
            }
            break;
        }
    }
    if (ParamTypes.Num() != Method->Params.Num())
    {
        return VH_ERR_NOT_FOUND;
    }

    int32 Status = VH_OK;
    Verse::FRunningContext Context = Verse::FRunningContextPromise{};
    const verse::FExecutionContext ExecContext = verse::FExecutionContext::GetActiveContext();

    const AutoRTFM::ETransactionResult TransactionResult = AutoRTFM::Transact([&] {
        // Open, inside the transaction. TVerseFunction's own operator() is AUTORTFM_OPEN and this
        // is the same reason: raising a Verse runtime error from closed code trips
        // AutoRTFM::UnreachableIfClosed in FContext::RaiseVerseRuntimeError and takes the process
        // down, rather than unwinding the way a raise is supposed to.
        AutoRTFM::Open([&] {
        Context.EnterVM([&] {
            Verse::VFunction::Args Converted;
            Converted.Reserve(ArgCount);
            for (int32 Index = 0; Index < ArgCount; ++Index)
            {
                Verse::VValue Value;
                if (!WireToValue(Context, Args[Index], ParamTypes[Index], Value))
                {
                    Status = VH_ERR_ARGUMENT;
                    return;
                }
                Converted.Add(Value);
            }

            const Verse::FOpResult OpResult = Resolved.Function->Invoke(Context, MoveTemp(Converted));
            switch (OpResult.Kind)
            {
            case Verse::FOpResult::Return:
                // A void method still returns -- of false, which is Verse's empty tuple -- so the
                // declared result type is what decides whether there is a value to read, not the
                // presence of one.
                if (ResultTypeDesc.Described.Type != VH_TYPE_VOID
                    && !ValueToWire(Context, OpResult.Value, ResultTypeDesc, OutStorage, OutResult))
                {
                    Status = VH_ERR_ARGUMENT;
                }
                break;

            case Verse::FOpResult::Fail:
                // A <decides> method that ran and declined. Distinct from VH_ERR_NOT_FOUND, which
                // would say there had been nothing to call.
                Status = VH_ERR_FAILED;
                break;

            case Verse::FOpResult::Yield:
                // A <suspends> method started a task instead of completing. Nothing is wrong and
                // there is no value; the task runs on under vh_tick.
                break;

            default:
                Status = VH_ERR_RUNTIME;
                break;
            }
        });
        });
    });

    // A raise unwinds to the root failure context and aborts the transaction, which is what drops
    // the Godot writes this call had deferred. The transaction result is the only place that is
    // visible from here: the raise itself does not return through us.
    if (TransactionResult != AutoRTFM::ETransactionResult::Committed)
    {
        return VH_ERR_RUNTIME;
    }
    return Status;
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
                                   ? FVerseFunction(Context, GScriptPackageName, ScriptVersePath, MainFunctionName)
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
