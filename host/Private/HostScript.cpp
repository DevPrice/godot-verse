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
#include "SolBuildDiagnostic.h"
#include "TestUtils/PlaceholderObjectForContentScope.h"
#include "ULangUEUtils.h"
#include "VerseComputationLimitControl.h"
#include "VerseContentScope.h"
#include "VerseString.h"
#include "VerseTask.h"
#include "UObject/StrongObjectPtr.h"
#include "VerseVM/VVMClass.h"
#include "VerseVM/VVMCoroutine.h"
#include "VerseVM/VVMGlobalProgram.h"
#include "VerseVM/VVMNativeFunction.h"
#include "VerseVM/VVMPackage.h"
#include "VerseVM/VVMProgram.h"
#include "VerseVM/VVMContext.h"
#include "VerseVM/VVMUniqueString.h"
#include "uLang/SourceProject/VerseVersion.h"
#include "uLang/Toolchain/ProgramBuildManager.h"

namespace {

constexpr const char* ScriptPackageName = "SolIdeDataSources";
constexpr const char* ScriptVersePath = "/user@localhost";
constexpr const char* MainFunctionName = "Main(:[][]char,:[[]char][]char)";

using FMainFunction = TVerseFunction<FVerseResult(
    TVerseCall<void>, const TArray<verse::string>&, const TMap<verse::string, verse::string>&)>;

TSharedPtr<ISolarisIde> GIde;
bool GProjectBuilt = false;
TArray<TSharedRef<ISolIdeDataSource>> GDataSources;
TSharedPtr<verse::FContentScope> GContentScope;
TOptional<verse::FContentScopeGuard> GContentScopeGuard;

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

    return true;
}

AUTORTFM_DISABLE bool GodotVerse::CheckProject(const FUtf8String& Path, const FUtf8String& SourceText)
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

    return GIde->BuildAll(Settings, MakeIdeDiagnostics(ForwardSolDiagnostic));
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
};

namespace {
/// The UClass behind a script's top-level Verse class, or null if there is no such class or it
/// does not derive from godot_object. A class that does not derive from godot_object has no
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
        if (NativeClass && NativeClass->IsChildOf(verse::godot_object::StaticClass()))
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

AUTORTFM_DISABLE GodotVerse::FInstance* GodotVerse::Instantiate(FUtf8StringView ClassName, int64 Handle)
{
    UClass* NativeClass = FindGodotClass(ClassName);
    if (!NativeClass)
    {
        ReportError(FUtf8String(UTF8TEXT("Could not instantiate ")) + FUtf8String(ClassName)
                    + UTF8TEXT(": no such class deriving from godot_object at ") + ScriptVersePath);
        return nullptr;
    }

    // UVerseClass::PostInitInstance runs the Verse constructor from inside NewObject, so fields
    // are initialised by the time this returns.
    UObject* Instance = NewObject<UObject>(GetTransientPackage(), NativeClass);
    if (!Instance)
    {
        return nullptr;
    }

    verse::godot_object* Shadow = CastChecked<verse::godot_object>(Instance);
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
/// `godot_object` gives Ready, Update and PhysicsUpdate empty bodies so a script can <override>
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
    FVerseFunction Base(Context, verse::godot_object::StaticClass()->GetDefaultObject(), DecoratedName);
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

AUTORTFM_DISABLE int32 GodotVerse::InstanceCallVoid(const FInstance* Instance, FUtf8StringView DecoratedName)
{
    return CallMethod<TVerseFunction<void()>>(Instance, DecoratedName);
}

AUTORTFM_DISABLE int32 GodotVerse::InstanceCallVoidFloat(const FInstance* Instance, FUtf8StringView DecoratedName, double Arg)
{
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
