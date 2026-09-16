// Copyright Epic Games, Inc. All Rights Reserved.

#include "AutoRTFM.h"

// The ABI entry points below are AutoRTFM-disabled: they call engine boot and Solaris code that
// is itself disabled, and nothing outside ever calls them from inside a transaction.
#define VH_ATTR AUTORTFM_DISABLE

#include "Containers/StringConv.h"
#include "Containers/UnrealString.h"
#include "HostCooked.h"
#include "HostDebug.h"
#include "HostEventLoop.h"
#include "HostRuntime.h"
#include "HostScript.h"
#include "ISolarisModule.h"
#include "Misc/CommandLine.h"
#include "Misc/CoreDelegates.h"
#include "Modules/ModuleManager.h"
#include "UObject/GCObject.h"
#include "VerseVM/VVMRuntimeError.h"
#include "RequiredProgramMainCPPInclude.h"
#include "VerseString.h"
#include "VerseVM/VVMSocketDebugger.h"

IMPLEMENT_APPLICATION(VerseHost, "verse_host");

namespace {

FString GEngineDirOverride;
Verse::SocketDebugger::FDebuggerScope GDebuggerScope;

using GodotVerse::GetHost;

FUtf8StringView Cstr(const char* Text)
{
    return Text ? FUtf8StringView(reinterpret_cast<const UTF8CHAR*>(Text)) : FUtf8StringView();
}

/// Points a vh_complete_item at an FCompleteItem's strings. The item borrows them, so whichever
/// array holds the source must outlive the descriptor array -- which is why both are static at
/// every call site.
vh_complete_item ToCompleteItem(const GodotVerse::FCompleteItem& Item)
{
    return vh_complete_item{
        reinterpret_cast<const char*>(*Item.Name),
        Item.Name.Len(),
        reinterpret_cast<const char*>(*Item.Type),
        Item.Type.Len(),
        reinterpret_cast<const char*>(*Item.Owner),
        Item.Owner.Len(),
        reinterpret_cast<const char*>(*Item.Path),
        Item.Path.Len(),
        Item.Line,
        (int32_t)Item.Kind,
        Item.bIsVar ? 1 : 0,
        Item.ParamCount,
        reinterpret_cast<const char*>(*Item.Signature),
        Item.Signature.Len(),
        Item.bIsOverridable ? 1 : 0,
        Item.OwnerDistance,
        Item.bIsNamed ? 1 : 0};
}

} // namespace

/// Unguarded, and it is the one entry point that must be: a consumer calls this *before* vh_init to
/// decide whether to load the host at all, so there is no recorded thread to compare against and
/// nothing here that could be harmed by the answer. It reads a compile-time constant.
extern "C" int32_t vh_abi_version(void)
{
    return VH_ABI_VERSION;
}

/// Unguarded for the same reason vh_abi_version is, and asked in the same breath: the consumer
/// reads it before vh_init so it can refuse the wrong host with a sentence rather than fail inside
/// a boot. Also a compile-time constant.
extern "C" int32_t vh_host_kind(void)
{
    return VH_HOST_KIND;
}

namespace {
/// The thread vh_init ran on, which is the only one that may enter the VM (R-ASYNC-8).
///
/// VerseVM asserts it: VVMEnterVMInline.h's `ensure(IsInGameThread() && ...)`, above the comment
/// "Verse bytecode and AutoRTFM transactions must run on the game thread". It is an `ensure`, not
/// a `check`, so serving a call from a worker thread is a logged callstack followed by undefined
/// behaviour -- the worst of the available failure modes. And it is thread *identity*: serialising
/// entry would not satisfy it, and AutoRTFM's transaction state is per-thread besides.
///
/// So the guard is a comparison, not a lock. What a foreign-thread call *should* do instead of
/// being refused -- hand the work to the game thread and wait, say -- is OQ-6's, and the deadlock
/// a blocking hand-off invites is why it is not decided here.
///
/// Stage 3 is where this becomes necessary rather than theoretical: a Callable is a value, and an
/// author may hand one to a WorkerThreadPool task.
uint32 GVerseThreadId = 0;

/// Reports the refusal through the diagnostic callback, which is the only channel a call from the
/// wrong thread has -- it cannot raise, because raising is itself entering the VM.
///
/// Every entry point below carries this prologue, with three deliberate exceptions, each commented
/// at its own definition: vh_abi_version (answered before there is a thread to compare against),
/// vh_init (which records the thread) and vh_callback_release (which a Callable's last reference may
/// legitimately drop on any thread).
///
/// The guard answers VH_ERR_THREAD where it can. Four entry points return vh_bool and have no error
/// value, so they answer 0 -- which reads as "no such class" rather than "refused", and is the one
/// place this mechanism cannot say what happened. The diagnostic is what carries the difference, and
/// widening those four to int32_t is a major ABI change nobody has needed yet.
/// The eleven entry points that need a Verse compiler, in a host that has none (ABI 8.2).
///
/// A clause in the prologue rather than eleven bodies under an #if, and *before* the thread guard
/// rather than after: "this host cannot ever do that" is a better answer than "not from here", and
/// it costs nothing to give on the wrong thread too. They stay exported -- the surface is the same
/// three ways -- so a consumer gets a status it can print instead of a null it has to guess at.
#define VH_REFUSE_WITHOUT_COMPILER(Failed) \
    do { if (!WITH_VERSE_COMPILER) { return (Failed); } } while (0)

bool WrongThread(const char* What)
{
    if (GVerseThreadId == 0 || FPlatformTLS::GetCurrentThreadId() == GVerseThreadId)
    {
        return false;
    }
    const FUtf8String Message = FUtf8String(UTF8TEXT("Verse: "))
        + FUtf8String(FUtf8StringView(reinterpret_cast<const UTF8CHAR*>(What)))
        + UTF8TEXT(" was called from a thread other than the one Verse runs on, and was refused "
                   "having run nothing. Verse bytecode and its transactions are pinned to the game "
                   "thread; marshal the work back to it -- call_deferred, or a signal emitted from "
                   "the main thread -- rather than calling a Verse method from a WorkerThreadPool "
                   "task.");
    GodotVerse::ReportError(FUtf8StringView(Message));
    return true;
}
}

namespace {

/// vh_init's body, with a switch for the one caller that has already booted the engine.
///
/// The cooker is an executable, so it owns its own PreInit: an editor-class Program needs the
/// EDITOR token, -nullrhi and -NoShaderCompile on the command line, which a DLL host never does
/// (CookMain.cpp). Everything else vh_init does, it still needs.
VH_ATTR int32_t InitHost(const vh_init_desc* Desc, bool bEngineAlreadyBooted);

} // namespace

extern "C" int32_t vh_init(const vh_init_desc* Desc)
{
    return InitHost(Desc, /*bEngineAlreadyBooted*/ false);
}

#if VH_HOST_KIND == VH_HOST_KIND_COOKER
VH_ATTR int32_t GodotVerse::InitCookerAfterEngineBoot(const vh_init_desc& Desc)
{
    return InitHost(&Desc, /*bEngineAlreadyBooted*/ true);
}
#endif

namespace {

VH_ATTR int32_t InitHost(const vh_init_desc* Desc, bool bEngineAlreadyBooted)
{
    // Majors must match exactly and minors need not, which is the policy written at the top of
    // verse_host_abi.h -- until the first minor bump this compared the whole version and so refused
    // a consumer the policy says to accept. A consumer built against a lower minor simply never
    // asks about what was added; one built against a higher minor is refused here, because it would
    // expect fields this host does not write.
    // StructSize is a floor, not an equality: a consumer built against a lower minor sends a
    // shorter descriptor, and refusing it would make every minor bump a major one. Everything up
    // to EnableDebugger has been there since 8.0, so that is the floor; CookedDirUtf8 and anything
    // after it is read only when StructSize reaches it. A *longer* descriptor is fine too -- the
    // AbiVersion test below is what refuses a consumer newer than this host.
    constexpr int32_t MinInitDescSize =
        static_cast<int32_t>(offsetof(vh_init_desc, EnableDebugger) + sizeof(vh_bool));
    if (!Desc || Desc->StructSize < MinInitDescSize
        || Desc->AbiVersion / 1000 != VH_ABI_VERSION_MAJOR || Desc->AbiVersion > VH_ABI_VERSION)
    {
        return VH_ERR_ABI;
    }

    GodotVerse::FHostState& Host = GetHost();
    if (Host.bInitialized)
    {
        return VH_ERR_STATE;
    }

    GVerseThreadId = FPlatformTLS::GetCurrentThreadId();

    Host.Godot = Desc->Godot;

    // Everything past the consumer's own StructSize is memory it never wrote, and the copy above
    // read it anyway. Harmless while every field predated 8.0; from the first callback added at a
    // minor -- InstantiateClass, at 8.3 -- a consumer built against a lower minor would hand this
    // host a stack-shaped bit pattern where a function pointer goes, and the null check every
    // caller makes would pass. Clearing is what makes "check the pointer" mean "the consumer
    // supplied it".
    if (Host.Godot.StructSize > 0 && Host.Godot.StructSize < static_cast<int32_t>(sizeof(vh_godot_api)))
    {
        FMemory::Memzero(reinterpret_cast<uint8*>(&Host.Godot) + Host.Godot.StructSize,
                         sizeof(vh_godot_api) - static_cast<size_t>(Host.Godot.StructSize));
    }

    Host.OnDiagnostic = Desc->OnDiagnostic;
    Host.DiagnosticCtx = Desc->DiagnosticCtx;
    Host.OnRuntimeError = Desc->OnRuntimeError;
    Host.RuntimeErrorCtx = Desc->RuntimeErrorCtx;

    // R-DIAG-2, in two halves, because no single hook carries both the stack and the report.
    //
    // RuntimeErrorTextProvider is a text *formatter*: FContext::RaiseVerseRuntimeError calls it
    // while the Verse stack is still standing and hands it the rendered callstack, but returning
    // from it is not the moment to report anything -- the cascading abort that follows has not run
    // yet. OnVerseRuntimeError is broadcast after that abort, and carries only the string the
    // provider returned.
    //
    // So the provider appends the callstack to the message in a shape the reporter can split back
    // apart, and the reporter does the splitting. Carrying it in a variable between the two looked
    // simpler and was not: the provider does not run for every raise the reporter sees, and a stale
    // stack attached to the wrong error is worse than no stack at all.
    FVerseRuntimeErrorDelegates::RuntimeErrorTextProvider.BindLambda(
        [](const Verse::ERuntimeDiagnostic Diagnostic, const FText& MessageText, const FString& Callstack) {
            const FString Formatted = Verse::AsFormattedString(Diagnostic, MessageText);
            return Callstack.IsEmpty() ? Formatted : Formatted + TEXT("\n") + Callstack;
        });

    FVerseRuntimeErrorDelegates::OnVerseRuntimeError.AddLambda(
        [](const Verse::ERuntimeDiagnostic Diagnostic, const FText& MessageText, const FString& RuntimeErrorText) {
            // UE terminates the active content scope immediately after this delegate returns,
            // which cancels that scope's suspended work -- since Phase 5 the raising instance's
            // and nobody else's. Noted here because this is the last moment the task group can be
            // asked what is about to be cancelled.
            GodotVerse::NoteRuntimeErrorRaised();

            const FUtf8String Message(Verse::AsFormattedString(Diagnostic, MessageText));

            // Everything after the first line is the callstack the provider appended; a raise with
            // no frames renders as the message alone and splits to nothing, which is the right
            // answer rather than a missing one.
            FString Callstack;
            int32 FirstBreak = INDEX_NONE;
            if (RuntimeErrorText.FindChar(TEXT('\n'), FirstBreak))
            {
                Callstack = RuntimeErrorText.Mid(FirstBreak + 1);
            }
            GodotVerse::ReportRuntimeError(FUtf8StringView(Message), Callstack);
        });

    // We are loaded by godot.exe, so the engine directory cannot be derived from the running
    // process. GForeignEngineDir is the documented override for exactly this case.
    if (Desc->EngineDirUtf8)
    {
        GEngineDirOverride = FString(StringCast<TCHAR>(reinterpret_cast<const UTF8CHAR*>(Desc->EngineDirUtf8)).Get());
        GForeignEngineDir = *GEngineDirOverride;
    }

    // FApp::IsUnattended() - keeps the crash reporter from putting a dialog in front of the editor.
    GIsAutomationTesting = true;
#if WITH_EDITOR
    // Only a variable under WITH_EDITOR; without editor-only data GIsEditor is the literal false,
    // and the runtime host is not an editor in any case.
    GIsEditor = true;
#endif

    if (!bEngineAlreadyBooted
        && GEngineLoop.PreInit(TEXT("-NOCONSOLE -AssetGatherAll=0 -LogCmds=\"global Warning\"")) != 0)
    {
        GodotVerse::ReportError(UTF8TEXT("Failed to initialize the engine (PreInit failed)."));
        return VH_ERR_INIT;
    }

    FGCObject::StaticInit();

    // Before Solaris, not after. Loading Solaris is what loads the VNI packages -- the mirror and
    // the standard library -- and JitVniPackages looks each one up by package path, so the mount
    // points the cook wrote have to be down already.
    const char* const CookedDir =
        Desc->StructSize >= static_cast<int32_t>(offsetof(vh_init_desc, CookedDirUtf8) + sizeof(const char*))
            ? Desc->CookedDirUtf8
            : nullptr;
    const FString CookedDirPath =
        CookedDir ? FString(StringCast<TCHAR>(reinterpret_cast<const UTF8CHAR*>(CookedDir)).Get()) : FString();
    if (CookedDir)
    {
        FUtf8String Error;
        if (!GodotVerse::RegisterCookedMountPoints(CookedDirPath, Error))
        {
            GodotVerse::ReportError(FUtf8StringView(Error));
            return VH_ERR_INIT;
        }
    }
    else if (VH_HOST_KIND == VH_HOST_KIND_RUNTIME)
    {
        // With no compiler, a project this host was not handed is a project it can never have.
        GodotVerse::ReportError(UTF8TEXT(
            "This build of the Verse host has no compiler and was given no cooked project."));
        return VH_ERR_INIT;
    }

    // Loading Solaris initializes the uLang system params for UE integration.
    ISolarisModule::Get();

    if (Desc->EnableDebugger)
    {
        GDebuggerScope = Verse::SocketDebugger::Listen();
    }

    if (!GodotVerse::EnterContentScope())
    {
        return VH_ERR_INIT;
    }

    // The other half: the project's own packages, and the class shape it ships in place of a
    // semantic program. After the content scope, because loading one allocates Verse cells.
    if (CookedDir)
    {
        FUtf8String Error;
        if (!GodotVerse::LoadCookedProject(CookedDirPath, Error))
        {
            GodotVerse::ReportError(FUtf8StringView(Error));
            return VH_ERR_INIT;
        }
    }

    Host.bInitialized = true;
    return VH_OK;
}

} // namespace

extern "C" void vh_shutdown(void)
{
    if (WrongThread("vh_shutdown"))
    {
        return;
    }
    // The callbacks first: tearing the engine down collects, and a collected godot_ref would
    // otherwise call back into a GDExtension that is already unloading.
    GetHost().Godot = vh_godot_api{};

    FVerseRuntimeErrorDelegates::RuntimeErrorTextProvider.Unbind();
    FVerseRuntimeErrorDelegates::OnVerseRuntimeError.Clear();
    GodotVerse::WaitForBackgroundCheck();
    GodotVerse::FHostState& Host = GetHost();
    if (!Host.bInitialized)
    {
        return;
    }

    GodotVerse::ResetScriptState();
    GodotVerse::ResetEventLoop();
    GodotVerse::ReleaseCookedContainers();
    GDebuggerScope = Verse::SocketDebugger::FDebuggerScope{};

    FCoreDelegates::OnEnginePreExit.Broadcast();
    FCoreDelegates::OnPreExit.Broadcast();
    FCoreDelegates::OnExit.Broadcast();
    FModuleManager::Get().UnloadModulesAtShutdown();
    FEngineLoop::AppPreExit();
    RequestEngineExit(TEXT("verse_host shutting down."));
    FEngineLoop::AppExit();

    // Nothing allocated through GMalloc may survive AppExit into static destruction.
    GEngineDirOverride.Empty();
    GForeignEngineDir = nullptr;
    Host = GodotVerse::FHostState{};
}

extern "C" void vh_tick(double BudgetSeconds, vh_tick_stats* OutStats)
{
    // Fields are appended and never reordered, so a consumer built against a lower minor reserved a
    // *prefix* of this struct: zero and fill that much and no further, which is the fallback the
    // header's compatibility policy asks the host for. The wait fields arrived at v6.1 and
    // PeakInstanceTasks at v8.1, so each group is tested against its own end -- comparing against
    // sizeof() would have made every earlier group disappear the moment a later one was added.
    const int32_t StatsSize = OutStats ? OutStats->StructSize : 0;
    const bool bHasWaitFields =
        StatsSize >= (int32_t)(offsetof(vh_tick_stats, AnalysisWaitSeconds) + sizeof(double));
    if (StatsSize >= (int32_t)offsetof(vh_tick_stats, AnalysisWaits))
    {
        vh_tick_stats Zeroed{};
        Zeroed.StructSize = StatsSize;
        FMemory::Memcpy(OutStats, &Zeroed, FMath::Min<int32_t>(StatsSize, (int32_t)sizeof(vh_tick_stats)));
    }
    else
    {
        // Smaller than anything this header has ever described. Answering nothing beats writing
        // fields it did not reserve room for.
        OutStats = nullptr;
    }

    if (WrongThread("vh_tick"))
    {
        return;
    }
    if (!GetHost().bInitialized)
    {
        return;
    }

    // Reachable: Godot's debug loop keeps servicing the editor while stopped, and its flush_output
    // reaches _frame. Resuming a slept task inside a VM stopped mid-op is not something any of
    // this is designed for, and unlike the reads and the calls -- which S-3 measured as safe from
    // inside a stop -- there is nothing a resumed task could sensibly do.
    if (GodotVerse::IsDebugStopped())
    {
        return;
    }

    // Taken before the early return below, so a frame skipped for an analysis still hands over the
    // accounting of the waits that happened during it rather than folding them into the next one.
    if (bHasWaitFields)
    {
        int32 Waits = 0;
        double WaitSeconds = 0.0;
        GodotVerse::TakeAnalysisWaitStats(Waits, WaitSeconds);
        OutStats->AnalysisWaits = Waits;
        OutStats->AnalysisWaitSeconds = WaitSeconds;
    }

    // Ticking runs Verse, and VerseVM blocks execution for the length of a build. Waiting here
    // would hand back the stall vh_check_project_begin exists to remove, so a frame that lands
    // mid-analysis simply does not tick; the next one will. Reported as a tick that ran no jobs
    // rather than as one that did nothing, which is what it is.
    if (GodotVerse::IsBackgroundCheckRunning())
    {
        return;
    }

    GodotVerse::TickScripts(BudgetSeconds, OutStats);
}

extern "C" void vh_collect_garbage(void)
{
    if (WrongThread("vh_collect_garbage") || !GetHost().bInitialized)
    {
        return;
    }
    GodotVerse::CollectGarbageNow();
}

extern "C" int32_t vh_compile_project(const vh_source_file* Files, int32_t Count, int32_t* OutGeneration)
{
    VH_REFUSE_WITHOUT_COMPILER(VH_ERR_UNSUPPORTED);
    if (WrongThread("vh_compile_project"))
    {
        return VH_ERR_THREAD;
    }
    if (GodotVerse::IsDebugStopped())
    {
        // A build while a frame of the retiring generation is on the stack is incoherent for a
        // reason that has nothing to do with re-entrancy: it would publish a generation underneath
        // a frame belonging to the old one.
        return VH_ERR_STOPPED;
    }
    GodotVerse::WaitForBackgroundCheck();
    if (!Files || Count < 0 || !OutGeneration)
    {
        return VH_ERR_ABI;
    }

    if (!GetHost().bInitialized)
    {
        return VH_ERR_STATE;
    }

    TArray<GodotVerse::FScriptSource> Sources;
    Sources.Reserve(Count);
    for (int32_t Index = 0; Index < Count; ++Index)
    {
        if (!Files[Index].PathUtf8)
        {
            return VH_ERR_ABI;
        }
        Sources.Add({FUtf8String(Cstr(Files[Index].PathUtf8)),
                     Files[Index].ModulePathUtf8 ? FUtf8String(Cstr(Files[Index].ModulePathUtf8)) : FUtf8String()});
    }

    int32 Generation = 0;
    const bool bBuilt = GodotVerse::CompileProject(Sources, Generation);
    if (bBuilt)
    {
        *OutGeneration = Generation;
    }
    return bBuilt ? VH_OK : VH_ERR_COMPILE;
}

extern "C" int32_t vh_check_project(const char* PathUtf8, const char* SourceUtf8)
{
    VH_REFUSE_WITHOUT_COMPILER(VH_ERR_UNSUPPORTED);
    if (WrongThread("vh_check_project"))
    {
        return VH_ERR_THREAD;
    }
    if (GodotVerse::IsDebugStopped())
    {
        // See vh_check_project_begin: an analysis is an analysis whichever thread runs it.
        return VH_ERR_STOPPED;
    }
    if (!PathUtf8 || !SourceUtf8)
    {
        return VH_ERR_ABI;
    }
    if (!GetHost().bInitialized)
    {
        return VH_ERR_STATE;
    }
    return GodotVerse::CheckProject(FUtf8String(Cstr(PathUtf8)), FUtf8String(Cstr(SourceUtf8))) ? VH_OK : VH_ERR_COMPILE;
}

extern "C" int32_t vh_check_project_begin(const char* PathUtf8, const char* SourceUtf8)
{
    VH_REFUSE_WITHOUT_COMPILER(VH_ERR_UNSUPPORTED);
    if (WrongThread("vh_check_project_begin"))
    {
        return VH_ERR_THREAD;
    }
    if (GodotVerse::IsDebugStopped())
    {
        // Not re-entrancy -- S-3 found that safe. An analysis resets the semantic program and
        // blocks execution for its whole length, and the frame on the stack is going to resume
        // into whatever it leaves behind.
        return VH_ERR_STOPPED;
    }
    if (!PathUtf8 || !SourceUtf8)
    {
        return VH_ERR_ABI;
    }
    if (!GetHost().bInitialized)
    {
        return VH_ERR_STATE;
    }
    return GodotVerse::BeginBackgroundCheck(FUtf8String(Cstr(PathUtf8)), FUtf8String(Cstr(SourceUtf8)))
        ? VH_OK
        : VH_ERR_STATE;
}

extern "C" int32_t vh_check_project_poll(vh_bool* OutFinished)
{
    VH_REFUSE_WITHOUT_COMPILER(VH_ERR_UNSUPPORTED);
    if (WrongThread("vh_check_project_poll"))
    {
        return VH_ERR_THREAD;
    }
    if (!OutFinished)
    {
        return VH_ERR_ABI;
    }
    *OutFinished = 0;
    if (!GetHost().bInitialized)
    {
        return VH_ERR_STATE;
    }

    bool bFinished = false;
    const bool bResult = GodotVerse::PollBackgroundCheck(bFinished);
    *OutFinished = bFinished ? 1 : 0;
    return bResult ? VH_OK : VH_ERR_COMPILE;
}

extern "C" vh_bool vh_check_project_busy(void)
{
    VH_REFUSE_WITHOUT_COMPILER(0);
    if (WrongThread("vh_check_project_busy"))
    {
        return 0;
    }
    return GetHost().bInitialized && GodotVerse::IsBackgroundCheckRunning() ? 1 : 0;
}

extern "C" int32_t vh_run_main(const char* const* Args, int32_t ArgCount, int64_t* OutExitCode)
{
    if (WrongThread("vh_run_main"))
    {
        return VH_ERR_THREAD;
    }
    GodotVerse::WaitForBackgroundCheck();
    if (!Args && ArgCount > 0)
    {
        return VH_ERR_ABI;
    }
    if (!GetHost().bInitialized)
    {
        return VH_ERR_STATE;
    }

    TArray<verse::string> ScriptArgs;
    ScriptArgs.Reserve(ArgCount);
    for (int32_t Index = 0; Index < ArgCount; ++Index)
    {
        ScriptArgs.Add(verse::string(Cstr(Args[Index])));
    }

    int64 ExitCode = 0;
    const int32 Status = GodotVerse::RunMain(ScriptArgs, ExitCode);
    if (OutExitCode)
    {
        *OutExitCode = ExitCode;
    }
    return Status;
}

extern "C" vh_bool vh_has_class(const char* ClassNameUtf8)
{
    if (WrongThread("vh_has_class"))
    {
        return 0;
    }
    if (!ClassNameUtf8 || !GetHost().bInitialized)
    {
        return 0;
    }
    return GodotVerse::HasClass(Cstr(ClassNameUtf8)) ? 1 : 0;
}

extern "C" int32_t vh_instantiate(const char* ClassNameUtf8, vh_handle Handle, vh_instance** OutInstance)
{
    if (WrongThread("vh_instantiate"))
    {
        return VH_ERR_THREAD;
    }
    GodotVerse::WaitForBackgroundCheck();
    if (!ClassNameUtf8 || !OutInstance)
    {
        return VH_ERR_ABI;
    }
    *OutInstance = nullptr;

    if (!GetHost().bInitialized)
    {
        return VH_ERR_STATE;
    }

    GodotVerse::FInstance* Instance = GodotVerse::Instantiate(Cstr(ClassNameUtf8), Handle);
    if (!Instance)
    {
        return VH_ERR_NOT_FOUND;
    }
    *OutInstance = reinterpret_cast<vh_instance*>(Instance);
    return VH_OK;
}

extern "C" void vh_release_instance(vh_instance* Instance)
{
    if (WrongThread("vh_release_instance"))
    {
        return;
    }
    GodotVerse::WaitForBackgroundCheck();
    GodotVerse::ReleaseInstance(reinterpret_cast<GodotVerse::FInstance*>(Instance));
}

extern "C" vh_bool vh_instance_has_function(vh_instance* Instance, const char* DecoratedName)
{
    if (WrongThread("vh_instance_has_function"))
    {
        return 0;
    }
    GodotVerse::WaitForBackgroundCheck();
    if (!Instance || !DecoratedName || !GetHost().bInitialized)
    {
        return 0;
    }
    return GodotVerse::InstanceHasFunction(reinterpret_cast<GodotVerse::FInstance*>(Instance), Cstr(DecoratedName)) ? 1 : 0;
}

extern "C" int32_t vh_instance_call(vh_instance* Instance,
                                   const char* DecoratedName,
                                   const vh_value* Args,
                                   int32_t ArgCount,
                                   vh_arena* Arena,
                                   vh_value* OutResult)
{
    if (WrongThread("vh_instance_call"))
    {
        return VH_ERR_THREAD;
    }
    GodotVerse::WaitForBackgroundCheck();
    if (!Instance || !DecoratedName || ArgCount < 0 || (ArgCount > 0 && !Args))
    {
        return VH_ERR_ABI;
    }
    if (!GetHost().bInitialized)
    {
        return VH_ERR_STATE;
    }

    // The storage is static for the same reason every other descriptor here is: the value points at
    // bytes the host owns, and the caller is told they live until the next call.
    static vh_value Result;
    static GodotVerse::FFieldStorage Storage;
    const int32_t Status = GodotVerse::InstanceCall(
        reinterpret_cast<GodotVerse::FInstance*>(Instance), Cstr(DecoratedName), Args, ArgCount, Result, Storage);

    if (OutResult)
    {
        *OutResult = Status == VH_OK ? Result : vh_value{};
    }
    (void)Arena;
    return Status;
}

extern "C" int32_t vh_callback_invoke(int64_t CallbackId,
                                     const vh_value* Args,
                                     int32_t ArgCount,
                                     vh_arena* Arena,
                                     vh_value* OutResult)
{
    if (WrongThread("vh_callback_invoke"))
    {
        return VH_ERR_THREAD;
    }
    GodotVerse::WaitForBackgroundCheck();
    if (ArgCount < 0 || (ArgCount > 0 && !Args))
    {
        return VH_ERR_ABI;
    }
    if (!GetHost().bInitialized)
    {
        return VH_ERR_STATE;
    }

    static vh_value Result;
    static GodotVerse::FFieldStorage Storage;
    const int32_t Status = GodotVerse::InvokeCallback(CallbackId, Args, ArgCount, Result, Storage);
    if (OutResult)
    {
        *OutResult = Status == VH_OK ? Result : vh_value{};
    }
    (void)Arena;
    return Status;
}

extern "C" void vh_callback_release(int64_t CallbackId)
{
    if (!GetHost().bInitialized)
    {
        return;
    }
    // No thread guard: releasing touches a map and never enters the VM, and a Callable can be
    // destroyed on whatever thread dropped the last reference to it.
    GodotVerse::ReleaseCallback(CallbackId);
}

extern "C" int32_t vh_class_method_list(const char* ClassNameUtf8, const vh_method_desc** OutMethods, int32_t* OutCount)
{
    if (WrongThread("vh_class_method_list"))
    {
        return VH_ERR_THREAD;
    }
    if (!ClassNameUtf8 || !OutMethods || !OutCount)
    {
        return VH_ERR_ABI;
    }
    *OutMethods = nullptr;
    *OutCount = 0;

    if (!GetHost().bInitialized)
    {
        return VH_ERR_STATE;
    }

    // Both arrays are static and rebuilt per call: the descriptors point into the FMethodDesc
    // strings, so the two have to live exactly as long as each other, and the header promises only
    // until the next call.
    static TArray<GodotVerse::FMethodDesc> Methods;
    static TArray<vh_param_desc> Params;
    static TArray<vh_method_desc> Descs;
    if (!GodotVerse::GetClassMethods(Cstr(ClassNameUtf8), Methods))
    {
        return VH_ERR_NOT_FOUND;
    }

    // Filled before the descriptors, and reserved to its final size first, because a descriptor
    // holds a bare pointer into it -- growing it afterwards would leave those dangling.
    int32 TotalParams = 0;
    for (const GodotVerse::FMethodDesc& Method : Methods)
    {
        TotalParams += Method.Params.Num();
    }
    Params.Reset();
    Params.Reserve(TotalParams);
    for (const GodotVerse::FMethodDesc& Method : Methods)
    {
        for (const GodotVerse::FParamDesc& Param : Method.Params)
        {
            vh_param_desc& Out = Params.AddDefaulted_GetRef();
            Out.NameUtf8 = reinterpret_cast<const char*>(*Param.Name);
            Out.NameLen = Param.Name.Len();
            Out.Type = Param.Type;
            Out.VariantTag = Param.VariantTag;
            Out.HasDefault = Param.bHasDefault ? 1 : 0;
        }
    }

    Descs.Reset();
    Descs.Reserve(Methods.Num());
    int32 ParamCursor = 0;
    for (const GodotVerse::FMethodDesc& Method : Methods)
    {
        vh_method_desc& Out = Descs.AddDefaulted_GetRef();
        Out.NameUtf8 = reinterpret_cast<const char*>(*Method.Name);
        Out.NameLen = Method.Name.Len();
        Out.DecoratedUtf8 = reinterpret_cast<const char*>(*Method.DecoratedName);
        Out.DecoratedLen = Method.DecoratedName.Len();
        Out.Params = Method.Params.Num() > 0 ? Params.GetData() + ParamCursor : nullptr;
        Out.ParamCount = Method.Params.Num();
        Out.RequiredParamCount = Method.RequiredParamCount;
        Out.ResultType = Method.ResultType;
        Out.ResultVariantTag = Method.ResultVariantTag;
        Out.CanFail = Method.bCanFail ? 1 : 0;
        Out.Suspends = Method.bSuspends ? 1 : 0;
        Out.GodotVirtualUtf8 = reinterpret_cast<const char*>(*Method.GodotVirtual);
        Out.GodotVirtualLen = Method.GodotVirtual.Len();
        Out.Line = Method.Line;
        Out.Column = Method.Column;
        ParamCursor += Method.Params.Num();
    }

    *OutMethods = Descs.GetData();
    *OutCount = Descs.Num();
    return VH_OK;
}

extern "C" int32_t vh_class_signal_list(const char* ClassNameUtf8, const vh_signal_desc** OutSignals, int32_t* OutCount)
{
    if (WrongThread("vh_class_signal_list"))
    {
        return VH_ERR_THREAD;
    }
    if (!ClassNameUtf8 || !OutSignals || !OutCount)
    {
        return VH_ERR_ABI;
    }
    *OutSignals = nullptr;
    *OutCount = 0;

    if (!GetHost().bInitialized)
    {
        return VH_ERR_STATE;
    }

    // Static and rebuilt per call, the same discipline vh_class_method_list follows: the
    // descriptors point into the FSignalDesc strings, so the two have to live exactly as long as
    // each other, and the header promises only until the next call.
    static TArray<GodotVerse::FSignalDesc> Signals;
    static TArray<vh_param_desc> Args;
    static TArray<vh_signal_desc> Descs;
    if (!GodotVerse::GetClassSignals(Cstr(ClassNameUtf8), Signals))
    {
        return VH_ERR_NOT_FOUND;
    }

    // Filled and sized before the descriptors, because a descriptor holds a bare pointer into it.
    int32 TotalArgs = 0;
    for (const GodotVerse::FSignalDesc& Signal : Signals)
    {
        TotalArgs += Signal.Args.Num();
    }
    Args.Reset();
    Args.Reserve(TotalArgs);
    for (const GodotVerse::FSignalDesc& Signal : Signals)
    {
        for (const GodotVerse::FParamDesc& Arg : Signal.Args)
        {
            vh_param_desc& Out = Args.AddDefaulted_GetRef();
            Out.NameUtf8 = reinterpret_cast<const char*>(*Arg.Name);
            Out.NameLen = Arg.Name.Len();
            Out.Type = Arg.Type;
            Out.VariantTag = Arg.VariantTag;
            Out.HasDefault = 0;
        }
    }

    Descs.Reset();
    Descs.Reserve(Signals.Num());
    int32 ArgCursor = 0;
    for (const GodotVerse::FSignalDesc& Signal : Signals)
    {
        vh_signal_desc& Out = Descs.AddDefaulted_GetRef();
        Out.NameUtf8 = reinterpret_cast<const char*>(*Signal.Name);
        Out.NameLen = Signal.Name.Len();
        Out.Args = Signal.Args.Num() > 0 ? Args.GetData() + ArgCursor : nullptr;
        Out.ArgCount = Signal.Args.Num();
        Out.Line = Signal.Line;
        Out.Column = Signal.Column;
        Out.Reject = Signal.Reject;
        Out.RejectDetailUtf8 = reinterpret_cast<const char*>(*Signal.RejectDetail);
        Out.RejectDetailLen = Signal.RejectDetail.Len();
        ArgCursor += Signal.Args.Num();
    }

    *OutSignals = Descs.GetData();
    *OutCount = Descs.Num();
    return VH_OK;
}

extern "C" int32_t vh_class_rpc_list(const char* ClassNameUtf8, const vh_rpc_desc** OutRpcs, int32_t* OutCount)
{
    if (WrongThread("vh_class_rpc_list"))
    {
        return VH_ERR_THREAD;
    }
    if (!ClassNameUtf8 || !OutRpcs || !OutCount)
    {
        return VH_ERR_ABI;
    }
    *OutRpcs = nullptr;
    *OutCount = 0;

    if (!GetHost().bInitialized)
    {
        return VH_ERR_STATE;
    }

    // Static and rebuilt per call, the same discipline the method and signal lists follow: the
    // descriptors hold bare pointers into the FRpcDesc strings, so the two have to live exactly as
    // long as each other and the header promises only until the next call.
    static TArray<GodotVerse::FRpcDesc> Rpcs;
    static TArray<vh_rpc_desc> Descs;
    if (!GodotVerse::GetClassRpcs(Cstr(ClassNameUtf8), Rpcs))
    {
        return VH_ERR_NOT_FOUND;
    }

    Descs.Reset();
    Descs.Reserve(Rpcs.Num());
    for (const GodotVerse::FRpcDesc& Rpc : Rpcs)
    {
        vh_rpc_desc& Out = Descs.AddDefaulted_GetRef();
        Out.NameUtf8 = reinterpret_cast<const char*>(*Rpc.Name);
        Out.NameLen = Rpc.Name.Len();
        Out.RpcMode = Rpc.RpcMode;
        Out.CallLocal = Rpc.bCallLocal ? 1 : 0;
        Out.TransferMode = Rpc.TransferMode;
        Out.Channel = Rpc.Channel;
        Out.Line = Rpc.Line;
        Out.Column = Rpc.Column;
        Out.Reject = Rpc.Reject;
        Out.RejectDetailUtf8 = reinterpret_cast<const char*>(*Rpc.RejectDetail);
        Out.RejectDetailLen = Rpc.RejectDetail.Len();
    }

    *OutRpcs = Descs.GetData();
    *OutCount = Descs.Num();
    return VH_OK;
}

extern "C" int32_t vh_class_static_list(const char* ClassNameUtf8, const vh_static_desc** OutStatics, int32_t* OutCount)
{
    if (WrongThread("vh_class_static_list"))
    {
        return VH_ERR_THREAD;
    }
    if (!ClassNameUtf8 || !OutStatics || !OutCount)
    {
        return VH_ERR_ABI;
    }
    *OutStatics = nullptr;
    *OutCount = 0;

    if (!GetHost().bInitialized)
    {
        return VH_ERR_STATE;
    }

    // Held rather than copied: a vh_value in the block points into the FFieldStorage beside it, so
    // the descriptors below stay valid only while the block does -- and the snapshot it came from
    // may be replaced by the next analysis. The share is what makes "valid until the next call"
    // true regardless.
    static TSharedPtr<const GodotVerse::FClassStatics> Held;
    static TArray<vh_static_desc> Descs;
    if (!GodotVerse::GetClassStatics(Cstr(ClassNameUtf8), Held))
    {
        return VH_ERR_NOT_FOUND;
    }

    const TArray<GodotVerse::FStaticDesc>& Statics = Held->Statics;
    Descs.Reset();
    Descs.Reserve(Statics.Num());
    for (int32 Index = 0; Index < Statics.Num(); ++Index)
    {
        vh_static_desc& Out = Descs.AddDefaulted_GetRef();
        Out.NameUtf8 = reinterpret_cast<const char*>(*Statics[Index].Name);
        Out.NameLen = Statics[Index].Name.Len();
        Out.IsFunction = Statics[Index].bIsFunction ? 1 : 0;
        Out.Value = Held->Values[Index];
        Out.Line = Statics[Index].Line;
        Out.Column = Statics[Index].Column;
    }

    *OutStatics = Descs.GetData();
    *OutCount = Descs.Num();
    return VH_OK;
}

extern "C" vh_bool vh_class_is_abstract(const char* ClassNameUtf8)
{
    if (WrongThread("vh_class_is_abstract"))
    {
        return 0;
    }
    if (!ClassNameUtf8 || !GetHost().bInitialized)
    {
        return 0;
    }
    return GodotVerse::IsClassAbstract(Cstr(ClassNameUtf8)) ? 1 : 0;
}

extern "C" int32_t vh_class_base_type(const char* ClassNameUtf8, const char** OutUtf8)
{
    if (WrongThread("vh_class_base_type"))
    {
        return VH_ERR_THREAD;
    }
    if (!ClassNameUtf8 || !OutUtf8 || !GetHost().bInitialized)
    {
        return VH_ERR_ARGUMENT;
    }
    // Held, not returned by value: the answer is a NUL-terminated string the caller reads before
    // its next ask, which is the bargain every other string this header hands back makes.
    static FUtf8String Held;
    Held = GodotVerse::ClassBaseType(Cstr(ClassNameUtf8));
    if (Held.IsEmpty())
    {
        *OutUtf8 = nullptr;
        return VH_ERR_NOT_FOUND;
    }
    *OutUtf8 = reinterpret_cast<const char*>(*Held);
    return VH_OK;
}

extern "C" int32_t vh_class_export_list(const char* ClassNameUtf8, const vh_export_desc** OutExports, int32_t* OutCount)
{
    if (WrongThread("vh_class_export_list"))
    {
        return VH_ERR_THREAD;
    }
    if (!ClassNameUtf8 || !OutExports || !OutCount)
    {
        return VH_ERR_ABI;
    }
    *OutExports = nullptr;
    *OutCount = 0;

    if (!GetHost().bInitialized)
    {
        return VH_ERR_STATE;
    }

    // The ABI promises the names stay valid until the next call, and the descriptors point into
    // the strings the harvest returns -- so both outlive this function rather than the stack.
    static TArray<GodotVerse::FExportDesc> Exports;
    static TArray<vh_export_desc> Descs;

    Descs.Reset();
    if (!GodotVerse::GetClassExports(Cstr(ClassNameUtf8), Exports))
    {
        return VH_ERR_NOT_FOUND;
    }

    Descs.Reserve(Exports.Num());
    for (const GodotVerse::FExportDesc& Export : Exports)
    {
        Descs.Add(vh_export_desc{
            reinterpret_cast<const char*>(*Export.Name),
            Export.Name.Len(),
            Export.Type,
            Export.VariantTag,
            Export.ElementVariantTag,
            Export.bIsVar ? 1 : 0,
            Export.Hint,
            reinterpret_cast<const char*>(*Export.HintString),
            Export.HintString.Len(),
            reinterpret_cast<const char*>(*Export.NativeClass),
            Export.NativeClass.Len(),
            Export.RangeMin,
            Export.RangeMax,
            Export.bHasRangeMin ? 1 : 0,
            Export.bHasRangeMax ? 1 : 0,
            Export.GroupKind,
            reinterpret_cast<const char*>(*Export.GroupName),
            Export.GroupName.Len(),
            Export.Line,
            Export.Column,
            Export.Reject});
    }

    *OutExports = Descs.GetData();
    *OutCount = Descs.Num();
    return VH_OK;
}

namespace {
/// Backing store for the instance field reader. The ABI promises the value -- and any string it
/// points at -- stays valid until the next read, so neither can live on the stack. The class
/// default reader keeps its own, because what it hands out is a share of the analysis snapshot.
vh_value GFieldValue{};
GodotVerse::FFieldStorage GFieldStorage;
} // namespace

extern "C" int32_t vh_instance_get_field(vh_instance* Instance, const char* NameUtf8, const vh_value** OutValue)
{
    if (WrongThread("vh_instance_get_field"))
    {
        return VH_ERR_THREAD;
    }
    GodotVerse::WaitForBackgroundCheck();
    if (!Instance || !NameUtf8 || !OutValue)
    {
        return VH_ERR_ABI;
    }
    *OutValue = nullptr;

    if (!GetHost().bInitialized)
    {
        return VH_ERR_STATE;
    }

    if (!GodotVerse::ReadInstanceField(reinterpret_cast<GodotVerse::FInstance*>(Instance), Cstr(NameUtf8), GFieldValue, GFieldStorage))
    {
        return VH_ERR_NOT_FOUND;
    }

    *OutValue = &GFieldValue;
    return VH_OK;
}

extern "C" int32_t vh_instance_set_field(vh_instance* Instance, const char* NameUtf8, const vh_value* Value)
{
    if (WrongThread("vh_instance_set_field"))
    {
        return VH_ERR_THREAD;
    }
    GodotVerse::WaitForBackgroundCheck();
    if (!Instance || !NameUtf8 || !Value)
    {
        return VH_ERR_ABI;
    }
    if (!GetHost().bInitialized)
    {
        return VH_ERR_STATE;
    }
    return GodotVerse::WriteInstanceField(reinterpret_cast<GodotVerse::FInstance*>(Instance), Cstr(NameUtf8), *Value)
        ? VH_OK
        : VH_ERR_NOT_FOUND;
}

extern "C" int32_t vh_instance_set_field_instance(vh_instance* Instance, const char* NameUtf8, vh_instance* Value)
{
    if (WrongThread("vh_instance_set_field_instance"))
    {
        return VH_ERR_THREAD;
    }
    GodotVerse::WaitForBackgroundCheck();
    if (!Instance || !NameUtf8)
    {
        return VH_ERR_ABI;
    }
    if (!GetHost().bInitialized)
    {
        return VH_ERR_STATE;
    }
    // Value is allowed to be null: that is how a reference member is cleared.
    return GodotVerse::WriteInstanceFieldInstance(reinterpret_cast<GodotVerse::FInstance*>(Instance),
                                                  Cstr(NameUtf8),
                                                  reinterpret_cast<GodotVerse::FInstance*>(Value))
        ? VH_OK
        : VH_ERR_NOT_FOUND;
}

extern "C" int32_t vh_instance_to_string(vh_instance* Instance, const vh_value** OutValue)
{
    if (WrongThread("vh_instance_to_string"))
    {
        return VH_ERR_THREAD;
    }
    GodotVerse::WaitForBackgroundCheck();
    if (!Instance || !OutValue)
    {
        return VH_ERR_ABI;
    }
    if (!GetHost().bInitialized)
    {
        return VH_ERR_STATE;
    }

    /* Its own buffer, like vh_class_default_field's: a consumer reads an object's text while it is
     * also reading fields, and the two must not invalidate each other. */
    static vh_value GToStringValue;
    static GodotVerse::FFieldStorage GToStringStorage;
    GToStringValue = vh_value{};

    const int32_t Status = GodotVerse::InstanceToString(
        reinterpret_cast<GodotVerse::FInstance*>(Instance), GToStringValue, GToStringStorage);
    if (Status != VH_OK)
    {
        return Status;
    }
    *OutValue = &GToStringValue;
    return VH_OK;
}

extern "C" int32_t vh_class_default_field(const char* ClassNameUtf8, const char* NameUtf8, const vh_value** OutValue)
{
    if (WrongThread("vh_class_default_field"))
    {
        return VH_ERR_THREAD;
    }
    if (!ClassNameUtf8 || !NameUtf8 || !OutValue)
    {
        return VH_ERR_ABI;
    }
    *OutValue = nullptr;

    if (!GetHost().bInitialized)
    {
        return VH_ERR_STATE;
    }

    // Its own holder rather than GFieldValue, because a default now comes out of the analysis
    // snapshot and is kept alive by a share of it -- see vh_class_static_list. The instance reader
    // above keeps its own buffer, so the two no longer invalidate each other.
    static TSharedPtr<const GodotVerse::FFieldValue> Held;
    if (!GodotVerse::ReadClassDefaultField(Cstr(ClassNameUtf8), Cstr(NameUtf8), Held))
    {
        return VH_ERR_NOT_FOUND;
    }

    *OutValue = &Held->Value;
    return VH_OK;
}

extern "C" int32_t vh_lookup_symbol(const char* PathUtf8, int32_t Line, int32_t Column, const vh_lookup_desc** OutResult)
{
    VH_REFUSE_WITHOUT_COMPILER(VH_ERR_UNSUPPORTED);
    if (WrongThread("vh_lookup_symbol"))
    {
        return VH_ERR_THREAD;
    }
    if (!PathUtf8 || !OutResult)
    {
        return VH_ERR_ABI;
    }
    *OutResult = nullptr;

    if (!GetHost().bInitialized)
    {
        return VH_ERR_STATE;
    }

    // A position is not a question a snapshot can answer -- the loci live in the AST, which the
    // worker is rebuilding -- so this one declines rather than waits. VH_ERR_STATE is what
    // vh_check_project_begin already answers for "an analysis is in flight", and a hover that says
    // nothing for a frame is what the consumer does with it: the GDExtension's _lookup_code already
    // declines on vh_check_project_busy for exactly this reason.
    if (GodotVerse::IsBackgroundCheckRunning())
    {
        return VH_ERR_STATE;
    }

    // And the same answer for the same reason after a build: generating code puts the AST every
    // locus lives in out of reach, and what the consumer should do about it is ask again once an
    // analysis has run. Answering VH_ERR_NOT_FOUND would say the symbol is not there.
    if (!GodotVerse::ProgramIsAnalysisOnly())
    {
        return VH_ERR_STATE;
    }

    // The ABI promises the strings outlive the call, and the descriptor only points at the
    // harvest's -- so both are static rather than stack.
    static GodotVerse::FLookupDesc Lookup;
    static vh_lookup_desc Desc;

    if (!GodotVerse::LookupSymbol(Cstr(PathUtf8), Line, Column, Lookup))
    {
        return VH_ERR_NOT_FOUND;
    }

    Desc = vh_lookup_desc{
        reinterpret_cast<const char*>(*Lookup.Name),
        Lookup.Name.Len(),
        reinterpret_cast<const char*>(*Lookup.Path),
        Lookup.Path.Len(),
        Lookup.Line,
        Lookup.Column,
        reinterpret_cast<const char*>(*Lookup.Type),
        Lookup.Type.Len(),
        reinterpret_cast<const char*>(*Lookup.Owner),
        Lookup.Owner.Len(),
        (int32_t)Lookup.Kind,
        Lookup.bIsVar ? 1 : 0,
        Lookup.bIsParameter ? 1 : 0,
        Lookup.bIsDefinition ? 1 : 0,
        reinterpret_cast<const char*>(*Lookup.OverriddenOwner),
        Lookup.OverriddenOwner.Len(),
        reinterpret_cast<const char*>(*Lookup.OverriddenPath),
        Lookup.OverriddenPath.Len(),
        Lookup.OverriddenLine,
        Lookup.OverriddenColumn};

    *OutResult = &Desc;
    return VH_OK;
}

extern "C" int32_t vh_complete_symbol(const char* PathUtf8,
                                      const char* SourceUtf8,
                                      int32_t Line,
                                      int32_t Column,
                                      int32_t Mode,
                                      const vh_complete_item** OutItems,
                                      int32_t* OutCount)
{
    VH_REFUSE_WITHOUT_COMPILER(VH_ERR_UNSUPPORTED);
    if (WrongThread("vh_complete_symbol"))
    {
        return VH_ERR_THREAD;
    }
    if (!PathUtf8 || !SourceUtf8 || !OutItems || !OutCount)
    {
        return VH_ERR_ABI;
    }
    *OutItems = nullptr;
    *OutCount = 0;

    if (!GetHost().bInitialized)
    {
        return VH_ERR_STATE;
    }
    if (Mode < VH_COMPLETE_MEMBERS || Mode > VH_COMPLETE_ARCHETYPE_FIELDS)
    {
        return VH_ERR_ABI;
    }

    // ABI v7: this neither analyses nor waits. A position is answered against the AST, which only
    // an analysis of this very buffer builds, so a buffer the program does not describe is a
    // refusal the caller acts on -- vh_check_project_begin on it, then ask again after the poll --
    // rather than a stall it never asked for. Same code and same reasoning as vh_lookup_symbol.
    if (!GodotVerse::ProgramDescribes(FUtf8String(Cstr(PathUtf8)), FUtf8String(Cstr(SourceUtf8))))
    {
        return VH_ERR_STATE;
    }

    // Static for the same reason vh_lookup_symbol's descriptor is: the ABI promises the strings
    // outlive the call, and the items only point at the harvest's.
    static TArray<GodotVerse::FCompleteItem> Items;
    static TArray<vh_complete_item> Descs;

    if (!GodotVerse::Complete(Cstr(PathUtf8), FUtf8String(Cstr(SourceUtf8)), Line, Column, (vh_complete_mode)Mode, Items))
    {
        return VH_ERR_NOT_FOUND;
    }

    Descs.Reset(Items.Num());
    for (const GodotVerse::FCompleteItem& Item : Items)
    {
        Descs.Add(ToCompleteItem(Item));
    }

    *OutItems = Descs.GetData();
    *OutCount = Descs.Num();
    return VH_OK;
}

extern "C" int32_t vh_class_members(const char* ClassNameUtf8, const vh_complete_item** OutItems, int32_t* OutCount)
{
    VH_REFUSE_WITHOUT_COMPILER(VH_ERR_UNSUPPORTED);
    if (WrongThread("vh_class_members"))
    {
        return VH_ERR_THREAD;
    }
    if (!ClassNameUtf8 || !OutItems || !OutCount)
    {
        return VH_ERR_ABI;
    }
    *OutItems = nullptr;
    *OutCount = 0;

    if (!GetHost().bInitialized)
    {
        return VH_ERR_STATE;
    }

    static TArray<GodotVerse::FCompleteItem> Members;
    static TArray<vh_complete_item> MemberDescs;

    if (!GodotVerse::ClassMembers(Cstr(ClassNameUtf8), Members))
    {
        return VH_ERR_NOT_FOUND;
    }

    MemberDescs.Reset(Members.Num());
    for (const GodotVerse::FCompleteItem& Member : Members)
    {
        MemberDescs.Add(ToCompleteItem(Member));
    }

    *OutItems = MemberDescs.GetData();
    *OutCount = MemberDescs.Num();
    return VH_OK;
}

extern "C" int32_t vh_class_override_candidates(const char* ClassNameUtf8, const vh_complete_item** OutItems, int32_t* OutCount)
{
    VH_REFUSE_WITHOUT_COMPILER(VH_ERR_UNSUPPORTED);
    if (WrongThread("vh_class_override_candidates"))
    {
        return VH_ERR_THREAD;
    }
    if (!ClassNameUtf8 || !OutItems || !OutCount)
    {
        return VH_ERR_ABI;
    }
    *OutItems = nullptr;
    *OutCount = 0;

    if (!GetHost().bInitialized)
    {
        return VH_ERR_STATE;
    }

    static TArray<GodotVerse::FCompleteItem> Candidates;
    static TArray<vh_complete_item> CandidateDescs;

    if (!GodotVerse::ClassOverrideCandidates(Cstr(ClassNameUtf8), Candidates))
    {
        return VH_ERR_NOT_FOUND;
    }

    CandidateDescs.Reset(Candidates.Num());
    for (const GodotVerse::FCompleteItem& Candidate : Candidates)
    {
        CandidateDescs.Add(ToCompleteItem(Candidate));
    }

    *OutItems = CandidateDescs.GetData();
    *OutCount = CandidateDescs.Num();
    return VH_OK;
}

extern "C" int32_t vh_resolve_unknown_name(const char* NameUtf8, const vh_module_ref** OutModules, int32_t* OutCount)
{
    VH_REFUSE_WITHOUT_COMPILER(VH_ERR_UNSUPPORTED);
    if (WrongThread("vh_resolve_unknown_name"))
    {
        return VH_ERR_THREAD;
    }
    if (!NameUtf8 || !OutModules || !OutCount)
    {
        return VH_ERR_ABI;
    }
    *OutModules = nullptr;
    *OutCount = 0;

    if (!GetHost().bInitialized)
    {
        return VH_ERR_STATE;
    }

    static TArray<FUtf8String> Modules;
    static TArray<vh_module_ref> Refs;

    if (!GodotVerse::ResolveUnknownName(Cstr(NameUtf8), Modules))
    {
        return VH_ERR_NOT_FOUND;
    }

    Refs.Reset(Modules.Num());
    for (const FUtf8String& Module : Modules)
    {
        Refs.Add(vh_module_ref{reinterpret_cast<const char*>(*Module), Module.Len()});
    }

    *OutModules = Refs.GetData();
    *OutCount = Refs.Num();
    return VH_OK;
}

extern "C" int32_t vh_signature_at(const char* PathUtf8,
                                   const char* SourceUtf8,
                                   int32_t Line,
                                   int32_t Column,
                                   const vh_signature_desc** OutResult)
{
    VH_REFUSE_WITHOUT_COMPILER(VH_ERR_UNSUPPORTED);
    if (WrongThread("vh_signature_at"))
    {
        return VH_ERR_THREAD;
    }
    if (!PathUtf8 || !SourceUtf8 || !OutResult)
    {
        return VH_ERR_ABI;
    }
    *OutResult = nullptr;

    if (!GetHost().bInitialized)
    {
        return VH_ERR_STATE;
    }

    // The same refusal vh_complete_symbol makes, for the same reason: the editor asks both about
    // one keystroke, and neither runs an analysis of its own any more.
    if (!GodotVerse::ProgramDescribes(FUtf8String(Cstr(PathUtf8)), FUtf8String(Cstr(SourceUtf8))))
    {
        return VH_ERR_STATE;
    }

    static GodotVerse::FSignatureDesc Signature;
    static TArray<vh_complete_item> ParamDescs;
    static vh_signature_desc Desc;

    if (!GodotVerse::SignatureAt(Cstr(PathUtf8), FUtf8String(Cstr(SourceUtf8)), Line, Column, Signature))
    {
        return VH_ERR_NOT_FOUND;
    }

    ParamDescs.Reset(Signature.Params.Num());
    for (const GodotVerse::FCompleteItem& Param : Signature.Params)
    {
        ParamDescs.Add(ToCompleteItem(Param));
    }

    Desc = vh_signature_desc{
        reinterpret_cast<const char*>(*Signature.Name),
        Signature.Name.Len(),
        reinterpret_cast<const char*>(*Signature.Result),
        Signature.Result.Len(),
        ParamDescs.GetData(),
        ParamDescs.Num()};

    *OutResult = &Desc;
    return VH_OK;
}

/* ------------------------------------------------------ the debugger (R-DIAG-4) -- */

extern "C" int32_t vh_debug_set_enabled(vh_bool Enabled)
{
    if (WrongThread("vh_debug_set_enabled"))
    {
        return VH_ERR_THREAD;
    }
    if (!GetHost().bInitialized)
    {
        return VH_ERR_STATE;
    }
    // VH_ERR_STATE rather than a silent no-op: Epic's socket debugger owns the VM's one debugger
    // slot when vh_init_desc::EnableDebugger asked for it, and a consumer that thought it had
    // attached and was never notified would be the worst of the available failures.
    return GodotVerse::SetDebugEnabled(Enabled != 0) ? VH_OK : VH_ERR_STATE;
}

extern "C" int32_t vh_debug_stack_count(int32_t* OutCount)
{
    if (WrongThread("vh_debug_stack_count"))
    {
        return VH_ERR_THREAD;
    }
    if (!OutCount)
    {
        return VH_ERR_ABI;
    }
    int32 Count = 0;
    if (!GodotVerse::DebugStackCount(Count))
    {
        return VH_ERR_STATE;
    }
    *OutCount = Count;
    return VH_OK;
}

extern "C" int32_t vh_debug_stack_frame(int32_t Level, const vh_debug_frame** OutFrame)
{
    if (WrongThread("vh_debug_stack_frame"))
    {
        return VH_ERR_THREAD;
    }
    if (!OutFrame)
    {
        return VH_ERR_ABI;
    }

    // Static, like every other descriptor here: the frame points at bytes the host owns, and the
    // caller is told they live until the next call to this function.
    static FUtf8String Path;
    static FUtf8String Name;
    static vh_debug_frame Frame;

    int32 Line = 0;
    if (!GodotVerse::DebugStackFrame(Level, Path, Name, Line))
    {
        return VH_ERR_STATE;
    }

    Frame = vh_debug_frame{};
    Frame.StructSize = static_cast<int32_t>(sizeof(vh_debug_frame));
    Frame.PathUtf8 = reinterpret_cast<const char*>(*Path);
    Frame.PathLen = Path.Len();
    Frame.NameUtf8 = reinterpret_cast<const char*>(*Name);
    Frame.NameLen = Name.Len();
    Frame.Line = Line;
    *OutFrame = &Frame;
    return VH_OK;
}

extern "C" int32_t vh_debug_stack_values(int32_t Level, int32_t Kind, const vh_debug_value** OutValues, int32_t* OutCount)
{
    if (WrongThread("vh_debug_stack_values"))
    {
        return VH_ERR_THREAD;
    }
    if (!OutValues || !OutCount)
    {
        return VH_ERR_ABI;
    }

    // The stop owns the values; this only points at them, and is rebuilt from scratch every call --
    // a descriptor left over from a longer previous answer would name a value that no longer exists.
    //
    // Each descriptor is zeroed by *assignment* rather than by AddDefaulted_GetRef, which
    // default-initializes a POD and therefore leaves the two lanes this fills only one of holding
    // whatever the previous answer left there (phase-6-design.md 13.3).
    static TArray<vh_debug_value> Descs;

    const TArray<GodotVerse::FDebugValue>* const Values =
        GodotVerse::DebugStackValues(Level, Kind == VH_DEBUG_MEMBERS);
    if (!Values)
    {
        return VH_ERR_STATE;
    }

    Descs.Reset(Values->Num());
    for (const GodotVerse::FDebugValue& Value : *Values)
    {
        vh_debug_value Desc{};
        Desc.StructSize = static_cast<int32_t>(sizeof(vh_debug_value));
        Desc.NameUtf8 = reinterpret_cast<const char*>(*Value.Name);
        Desc.NameLen = Value.Name.Len();
        if (Value.bHasValue)
        {
            Desc.Value = &Value.Value;
        }
        else
        {
            Desc.RenderedUtf8 = reinterpret_cast<const char*>(*Value.Rendered);
            Desc.RenderedLen = Value.Rendered.Len();
        }
        Descs.Add(Desc);
    }

    *OutValues = Descs.GetData();
    *OutCount = Descs.Num();
    return VH_OK;
}

/* ------------------------------------------------------ the profiler (R-DIAG-5) -- */

extern "C" int32_t vh_profiling_set_enabled(vh_bool Enabled)
{
    if (WrongThread("vh_profiling_set_enabled"))
    {
        return VH_ERR_THREAD;
    }
    if (!GetHost().bInitialized)
    {
        return VH_ERR_STATE;
    }
    if (Enabled)
    {
        // Starting clears what a previous session accumulated. Godot's profiler panel is started
        // and stopped by the user, and carrying the last session's totals into the next one would
        // report times from a run that is over.
        GodotVerse::ResetProfile();
    }
    GodotVerse::SetProfilingEnabled(Enabled != 0);
    return VH_OK;
}

extern "C" int32_t vh_profiling_read(vh_bool FrameOnly, const vh_profile_row** OutRows, int32_t* OutCount)
{
    if (WrongThread("vh_profiling_read"))
    {
        return VH_ERR_THREAD;
    }
    if (!OutRows || !OutCount)
    {
        return VH_ERR_ABI;
    }

    static TArray<GodotVerse::FProfileRow> Rows;
    static TArray<vh_profile_row> Descs;

    GodotVerse::ReadProfile(FrameOnly != 0, Rows);

    Descs.Reset(Rows.Num());
    for (const GodotVerse::FProfileRow& Row : Rows)
    {
        vh_profile_row Desc{};
        Desc.StructSize = static_cast<int32_t>(sizeof(vh_profile_row));
        Desc.SignatureUtf8 = reinterpret_cast<const char*>(*Row.Signature);
        Desc.SignatureLen = Row.Signature.Len();
        Desc.CallCount = Row.CallCount;
        Desc.TotalSeconds = Row.TotalSeconds;
        Desc.SelfSeconds = Row.SelfSeconds;
        Descs.Add(Desc);
    }

    *OutRows = Descs.GetData();
    *OutCount = Descs.Num();
    return VH_OK;
}

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"

BOOL WINAPI DllMain(HINSTANCE, DWORD FdwReason, LPVOID)
{
    // Static destructors in the monolithic runtime consult IsEngineExitRequested(); reaching them
    // with it unset takes down the host process on unload.
    if (FdwReason == DLL_PROCESS_DETACH && !IsEngineExitRequested())
    {
        RequestEngineExit(TEXT("DLL_PROCESS_DETACH received"));
    }
    return TRUE;
}

#include "Windows/HideWindowsPlatformTypes.h"
#endif
