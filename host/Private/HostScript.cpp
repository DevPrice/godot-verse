// Copyright Epic Games, Inc. All Rights Reserved.

#include "HostScript.h"
#include "AutoRTFM.h"
#include "Containers/Map.h"
#include "Containers/UnrealString.h"
#include "HostEventLoop.h"
#include "HostRuntime.h"
#include "ISolarisIde.h"
#include "ISolarisModule.h"
#include "IVerseModule.h"
#include "Misc/FileHelper.h"
#include "SolBuildDiagnostic.h"
#include "TestUtils/PlaceholderObjectForContentScope.h"
#include "ULangUEUtils.h"
#include "VerseComputationLimitControl.h"
#include "VerseContentScope.h"
#include "VerseString.h"
#include "VerseTask.h"
#include "VerseVM/VVMCoroutine.h"
#include "VerseVM/VVMNativeFunction.h"
#include "uLang/SourceProject/VerseVersion.h"
#include "uLang/Toolchain/ProgramBuildManager.h"

namespace {

constexpr const char* ScriptPackageName = "SolIdeDataSources";
constexpr const char* ScriptVersePath = "/user@localhost";
constexpr const char* MainFunctionName = "Main(:[][]char,:[[]char][]char)";

using FMainFunction = TVerseFunction<FVerseResult(
    TVerseCall<void>, const TArray<verse::string>&, const TMap<verse::string, verse::string>&)>;

TSharedPtr<ISolarisIde> GIde;
TArray<TSharedRef<ISolIdeDataSource>> GDataSources;
TSharedPtr<verse::FContentScope> GContentScope;
TOptional<verse::FContentScopeGuard> GContentScopeGuard;

void ForwardSolDiagnostic(const FSolDiagnostic& Diagnostic)
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

bool EnsureIde()
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
                                  .VerseScope = uLang::EVerseScope::PublicUser,
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

AUTORTFM_DISABLE GodotVerse::FScript* GodotVerse::CompileFile(const FUtf8String& Path)
{
    if (!EnsureIde())
    {
        return nullptr;
    }

    FString SourceText;
    if (!FFileHelper::LoadFileToString(SourceText, *FString(Path)))
    {
        ReportError(FUtf8String(TEXT("Failed to open Verse source file: ")) + Path);
        return nullptr;
    }

    GDataSources.Add(GIde->AddDataSource(FULangConversionUtils::FUtf8StringToULangStr(Path)));

    FSolIdeBuildSettings Settings{.LinkSettings = uLang::SBuildParams::ELinkParam::RequireComplete};
    if (!GIde->BuildAll(Settings, MakeIdeDiagnostics(ForwardSolDiagnostic)))
    {
        return nullptr;
    }

    IVerseModule::Get(); // Runs VerseModule::StartupModule; VerseCmd does the same before calling in.

    return new FScript{Path};
}

AUTORTFM_DISABLE void GodotVerse::ReleaseScript(FScript* Script)
{
    delete Script;
}

AUTORTFM_DISABLE bool GodotVerse::HasFunction(FUtf8StringView DecoratedName)
{
    const FVerseFunction Function(
        verse::FExecutionContext::GetActiveContext(), ScriptPackageName, ScriptVersePath, DecoratedName);
    return Function.IsValid();
}

namespace {
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

    FMainFunction MainFunction{FVerseFunction(Context, ScriptPackageName, ScriptVersePath, MainFunctionName)};
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
int32 CallFunction(FUtf8StringView DecoratedName, ArgTypes... Args)
{
    const verse::FExecutionContext Context = verse::FExecutionContext::GetActiveContext();

    FunctionType Function{FVerseFunction(Context, ScriptPackageName, ScriptVersePath, DecoratedName)};
    if (!Function.IsValid())
    {
        return VH_ERR_NOT_FOUND;
    }

    const AutoRTFM::ETransactionResult TransactionResult =
        AutoRTFM::Transact([&] { Function(Context, Args...); });

    return TransactionResult == AutoRTFM::ETransactionResult::Committed ? VH_OK : VH_ERR_RUNTIME;
}
}

AUTORTFM_DISABLE int32 GodotVerse::CallVoid(FUtf8StringView DecoratedName)
{
    return CallFunction<TVerseFunction<void()>>(DecoratedName);
}

AUTORTFM_DISABLE int32 GodotVerse::CallVoidFloat(FUtf8StringView DecoratedName, double Arg)
{
    return CallFunction<TVerseFunction<void(double)>>(DecoratedName, Arg);
}

AUTORTFM_DISABLE void GodotVerse::TickScripts(double BudgetSeconds)
{
    PumpEventLoop(verse::FExecutionContext::GetActiveContext(), BudgetSeconds);
}
