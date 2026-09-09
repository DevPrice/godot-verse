// Copyright Epic Games, Inc. All Rights Reserved.

#include "AutoRTFM.h"

// The ABI entry points below are AutoRTFM-disabled: they call engine boot and Solaris code that
// is itself disabled, and nothing outside ever calls them from inside a transaction.
#define VH_ATTR AUTORTFM_DISABLE

#include "Containers/StringConv.h"
#include "Containers/UnrealString.h"
#include "HostEventLoop.h"
#include "HostRuntime.h"
#include "HostScript.h"
#include "ISolarisModule.h"
#include "Misc/CommandLine.h"
#include "Misc/CoreDelegates.h"
#include "Modules/ModuleManager.h"
#include "UObject/GCObject.h"
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

} // namespace

extern "C" int32_t vh_abi_version(void)
{
    return VH_ABI_VERSION;
}

extern "C" int32_t vh_init(const vh_init_desc* Desc)
{
    if (!Desc || Desc->StructSize != static_cast<int32_t>(sizeof(vh_init_desc)) || Desc->AbiVersion != VH_ABI_VERSION)
    {
        return VH_ERR_ABI;
    }

    GodotVerse::FHostState& Host = GetHost();
    if (Host.bInitialized)
    {
        return VH_ERR_STATE;
    }

    Host.Godot = Desc->Godot;
    Host.OnDiagnostic = Desc->OnDiagnostic;
    Host.DiagnosticCtx = Desc->DiagnosticCtx;

    // We are loaded by godot.exe, so the engine directory cannot be derived from the running
    // process. GForeignEngineDir is the documented override for exactly this case.
    if (Desc->EngineDirUtf8)
    {
        GEngineDirOverride = FString(StringCast<TCHAR>(reinterpret_cast<const UTF8CHAR*>(Desc->EngineDirUtf8)).Get());
        GForeignEngineDir = *GEngineDirOverride;
    }

    // FApp::IsUnattended() - keeps the crash reporter from putting a dialog in front of the editor.
    GIsAutomationTesting = true;
    GIsEditor = true;

    if (GEngineLoop.PreInit(TEXT("-NOCONSOLE -AssetGatherAll=0 -LogCmds=\"global Warning\"")) != 0)
    {
        GodotVerse::ReportError(UTF8TEXT("Failed to initialize the engine (PreInit failed)."));
        return VH_ERR_INIT;
    }

    FGCObject::StaticInit();

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

    Host.bInitialized = true;
    return VH_OK;
}

extern "C" void vh_shutdown(void)
{
    GodotVerse::FHostState& Host = GetHost();
    if (!Host.bInitialized)
    {
        return;
    }

    GodotVerse::ResetScriptState();
    GodotVerse::ResetEventLoop();
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

extern "C" void vh_tick(double BudgetSeconds)
{
    if (!GetHost().bInitialized)
    {
        return;
    }
    GodotVerse::TickScripts(BudgetSeconds);
}

extern "C" int32_t vh_compile_file(const char* PathUtf8, vh_script** OutScript)
{
    if (!PathUtf8 || !OutScript)
    {
        return VH_ERR_ABI;
    }
    *OutScript = nullptr;

    if (!GetHost().bInitialized)
    {
        return VH_ERR_STATE;
    }

    GodotVerse::FScript* Script = GodotVerse::CompileFile(FUtf8String(Cstr(PathUtf8)));
    if (!Script)
    {
        return VH_ERR_COMPILE;
    }

    *OutScript = reinterpret_cast<vh_script*>(Script);
    return VH_OK;
}

extern "C" void vh_release_script(vh_script* Script)
{
    GodotVerse::ReleaseScript(reinterpret_cast<GodotVerse::FScript*>(Script));
}

extern "C" vh_bool vh_script_has_function(vh_script* Script, const char* DecoratedName)
{
    if (!Script || !DecoratedName || !GetHost().bInitialized)
    {
        return 0;
    }
    return GodotVerse::HasFunction(Cstr(DecoratedName)) ? 1 : 0;
}

extern "C" int32_t vh_run_main(vh_script* Script,
                                                const char* const* Args,
                                                int32_t ArgCount,
                                                int64_t* OutExitCode)
{
    if (!Script)
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

extern "C" int32_t vh_call_void(vh_script* Script, const char* DecoratedName)
{
    if (!Script || !DecoratedName)
    {
        return VH_ERR_ABI;
    }
    if (!GetHost().bInitialized)
    {
        return VH_ERR_STATE;
    }
    return GodotVerse::CallVoid(Cstr(DecoratedName));
}

extern "C" int32_t vh_call_void_float(vh_script* Script, const char* DecoratedName, double Arg)
{
    if (!Script || !DecoratedName)
    {
        return VH_ERR_ABI;
    }
    if (!GetHost().bInitialized)
    {
        return VH_ERR_STATE;
    }
    return GodotVerse::CallVoidFloat(Cstr(DecoratedName), Arg);
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
