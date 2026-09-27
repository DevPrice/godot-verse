// Copyright Epic Games, Inc. All Rights Reserved.

#include "HostScript.h"
#include "HostBuild.h"
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

namespace {

constexpr const char* MainFunctionName = "Main(:[][]char,:[[]char][]char)";

using FMainFunction = TVerseFunction<FVerseResult(
    TVerseCall<void>, const TArray<verse::string>&, const TMap<verse::string, verse::string>&)>;

/// The outer every content scope instantiates into. Rooted once for the process: a scope holds it
/// weakly, so nothing here roots one per instance (phase-5-design.md 13's first risk).
UPlaceholderObjectForContentScope* GScopeOuter = nullptr;

/// The scope everything that is not a call into one instance runs under: analysis, the statics
/// reader, `Main`, the pump, and every field access. Its guard is the root of the stack and is
/// pushed once, at EnterContentScope.
TSharedPtr<verse::FContentScope> GProjectScope;
TOptional<verse::FContentScopeGuard> GProjectScopeGuard;

/// How deep the VM entries are nested. Only depth 0 is a moment at which the *root* guard can be
/// swapped, which is what RefreshProjectScope needs and why this is counted rather than inferred:
/// FContentScopeGuard exposes no depth, and popping a guard that is not the active one is an
/// ensure() away from a corrupt stack.
int32 GVerseEntryDepth = 0;

/// Replaces a terminated project scope with a fresh one, rather than un-terminating it.
///
/// A raised Verse runtime error calls Terminate() on the *active* content scope
/// (VVMRuntimeError.cpp), and FRunningContext::EnterVM_Internal then returns *without running its
/// functor* for every later entry into that scope. Nothing downstream can tell that apart from a
/// call that ran and did nothing, which is why every entry point below reports what actually ran.
///
/// Phase 3 answered this with ResetTerminationState() on one process-wide scope, at the next frame
/// boundary; the comment it carried recorded the defect that made it necessary -- one script's
/// first mistake ended Verse for the process, in an editor nobody restarts. Phase 5 keeps the
/// answer and narrows the question: Epic never revives either (ContentScopeRepository hands out a
/// *fresh* scope), the replacement happens at the next entry rather than at the next tick, and
/// with a scope per instance there is nothing project-wide left to stop. See spec R-DIAG-3.
AUTORTFM_DISABLE void RefreshProjectScope()
{
    if (!GProjectScope.IsValid() || !GProjectScope->WasTerminated())
    {
        return;
    }
    GProjectScopeGuard.Reset();
    GProjectScope = verse::MakeContentScope(GScopeOuter);
    GProjectScopeGuard.Emplace(GProjectScope.ToSharedRef());
}

} // namespace

AUTORTFM_DISABLE GodotVerse::FVerseEntry::FVerseEntry()
{
    // A Verse runtime error raised from closed code trips AutoRTFM::UnreachableIfClosed in
    // FContext::RaiseVerseRuntimeError and takes the process down rather than unwinding, so every
    // entry is made open, inside whatever transaction the caller holds (InstanceCall's Open).
    check(!AutoRTFM::IsClosed());
    if (GVerseEntryDepth++ == 0)
    {
        RefreshProjectScope();
    }
}

AUTORTFM_DISABLE GodotVerse::FVerseEntry::~FVerseEntry()
{
    --GVerseEntryDepth;
}

AUTORTFM_DISABLE bool GodotVerse::EnterContentScope()
{
    if (GProjectScopeGuard.IsSet())
    {
        return true;
    }

    VerseComputationLimitControl::SetComputationLimits(false);

    // Verse needs a UObject outer to instantiate into and we have no UWorld, so synthesize one --
    // one, for the process. A content scope holds its outer weakly and every instance scope shares
    // this one, so a project with a thousand awaiting nodes roots no more UObjects than a project
    // with none.
    GScopeOuter = UPlaceholderObjectForContentScope::MakeRooted();
    GProjectScope = verse::MakeContentScope(GScopeOuter);
    GProjectScopeGuard.Emplace(GProjectScope.ToSharedRef());
    return true;
}

AUTORTFM_DISABLE const UObject* GodotVerse::ContentScopeOuter()
{
    return GScopeOuter;
}

AUTORTFM_DISABLE void GodotVerse::LeaveContentScope()
{
    GProjectScopeGuard.Reset();
    GProjectScope.Reset();
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
                                   ? FVerseFunction(Context, PublishedScriptPackageName(), ScriptVersePath, MainFunctionName)
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

AUTORTFM_DISABLE void GodotVerse::NoteRuntimeErrorRaised()
{
    // The scope UE is about to terminate is the active one, which under R-ASYNC-4 is the raising
    // instance's. Said here because this delegate is the last moment the task group can be asked
    // what is about to be thrown away; the scope is replaced at that instance's next call.
    if (!verse::FContentScopeGuard::IsActive())
    {
        return;
    }
    const TSharedRef<verse::FContentScope>& Scope = verse::FContentScopeGuard::GetActiveScope();
    if (Scope->HasActiveTasks())
    {
        ReportInfo(&Scope.Get() == GProjectScope.Get()
            ? UTF8TEXT("Suspended work that was not started by any one script instance was "
                       "cancelled by the runtime error above.")
            : UTF8TEXT("This script instance's suspended work was cancelled by the runtime error "
                       "above. Other instances are unaffected (R-ASYNC-4)."));
    }
}

AUTORTFM_DISABLE void GodotVerse::TickScripts(double BudgetSeconds, vh_tick_stats* OutStats)
{
    // The pump's own row (R-DIAG-5). One synthetic signature rather than a row per resumed task,
    // because a `Sleep` resumption has no Godot event behind it to name it after -- what the
    // profiler can honestly say about queued work is how much of the frame it took.
    const GodotVerse::FProfileScope ProfileScope(
        GodotVerse::IsProfilingEnabled() ? FUtf8StringView(UTF8TEXT("<verse>::0::vh_tick"))
                                         : FUtf8StringView());

    PumpEventLoop(verse::FExecutionContext::GetActiveContext(), BudgetSeconds, OutStats);

    // OQ-13 chose observability over a cap, and this is the observation: the largest number of
    // live tasks any one instance's scope holds, which is what a `spawn` in `_Process` runs away
    // with. Nothing else in these numbers separates that from many instances with one task each --
    // a suspended task is not queued work, so JobsPending never sees it.
    //
    // GetNumActive is documented as an implementation detail meant for unit tests. It is used here
    // anyway, and only as a number to *show*: nothing branches on it, so the cost of Epic changing
    // what it counts is a monitor that reads differently, not behaviour that changes.
    if (OutStats
        && OutStats->StructSize >= (int32_t)(offsetof(vh_tick_stats, PeakInstanceTasks) + sizeof(int32_t)))
    {
        OutStats->PeakInstanceTasks = PeakTasksOfAnyInstance();
    }
}
