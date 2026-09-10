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
        Item.bIsOverridable ? 1 : 0};
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
    GodotVerse::WaitForBackgroundCheck();
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

    // Ticking runs Verse, and VerseVM blocks execution for the length of a build. Waiting here
    // would hand back the stall vh_check_project_begin exists to remove, so a frame that lands
    // mid-analysis simply does not tick; the next one will.
    if (GodotVerse::IsBackgroundCheckRunning())
    {
        return;
    }

    GodotVerse::TickScripts(BudgetSeconds);
}

extern "C" int32_t vh_compile_project(const char* const* PathsUtf8, int32_t Count)
{
    GodotVerse::WaitForBackgroundCheck();
    if (!PathsUtf8 || Count < 0)
    {
        return VH_ERR_ABI;
    }

    if (!GetHost().bInitialized)
    {
        return VH_ERR_STATE;
    }

    TArray<FUtf8String> Paths;
    Paths.Reserve(Count);
    for (int32_t Index = 0; Index < Count; ++Index)
    {
        if (!PathsUtf8[Index])
        {
            return VH_ERR_ABI;
        }
        Paths.Add(FUtf8String(Cstr(PathsUtf8[Index])));
    }

    return GodotVerse::CompileProject(Paths) ? VH_OK : VH_ERR_COMPILE;
}

extern "C" int32_t vh_check_project(const char* PathUtf8, const char* SourceUtf8)
{
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
    return GetHost().bInitialized && GodotVerse::IsBackgroundCheckRunning() ? 1 : 0;
}

extern "C" int32_t vh_run_main(const char* const* Args, int32_t ArgCount, int64_t* OutExitCode)
{
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
    GodotVerse::WaitForBackgroundCheck();
    if (!ClassNameUtf8 || !GetHost().bInitialized)
    {
        return 0;
    }
    return GodotVerse::HasClass(Cstr(ClassNameUtf8)) ? 1 : 0;
}

extern "C" int32_t vh_instantiate(const char* ClassNameUtf8, vh_handle Handle, vh_instance** OutInstance)
{
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
    GodotVerse::WaitForBackgroundCheck();
    GodotVerse::ReleaseInstance(reinterpret_cast<GodotVerse::FInstance*>(Instance));
}

extern "C" vh_bool vh_instance_has_function(vh_instance* Instance, const char* DecoratedName)
{
    GodotVerse::WaitForBackgroundCheck();
    if (!Instance || !DecoratedName || !GetHost().bInitialized)
    {
        return 0;
    }
    return GodotVerse::InstanceHasFunction(reinterpret_cast<GodotVerse::FInstance*>(Instance), Cstr(DecoratedName)) ? 1 : 0;
}

extern "C" int32_t vh_instance_call_void(vh_instance* Instance, const char* DecoratedName)
{
    GodotVerse::WaitForBackgroundCheck();
    if (!Instance || !DecoratedName)
    {
        return VH_ERR_ABI;
    }
    if (!GetHost().bInitialized)
    {
        return VH_ERR_STATE;
    }
    return GodotVerse::InstanceCallVoid(reinterpret_cast<GodotVerse::FInstance*>(Instance), Cstr(DecoratedName));
}

extern "C" int32_t vh_instance_call_void_float(vh_instance* Instance, const char* DecoratedName, double Arg)
{
    GodotVerse::WaitForBackgroundCheck();
    if (!Instance || !DecoratedName)
    {
        return VH_ERR_ABI;
    }
    if (!GetHost().bInitialized)
    {
        return VH_ERR_STATE;
    }
    return GodotVerse::InstanceCallVoidFloat(reinterpret_cast<GodotVerse::FInstance*>(Instance), Cstr(DecoratedName), Arg);
}

extern "C" int32_t vh_class_export_list(const char* ClassNameUtf8, const vh_export_desc** OutExports, int32_t* OutCount)
{
    GodotVerse::WaitForBackgroundCheck();
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
            Export.bIsVar ? 1 : 0,
            reinterpret_cast<const char*>(*Export.ClampMin),
            Export.ClampMin.Len(),
            reinterpret_cast<const char*>(*Export.ClampMax),
            Export.ClampMax.Len(),
            reinterpret_cast<const char*>(*Export.Category),
            Export.Category.Len()});
    }

    *OutExports = Descs.GetData();
    *OutCount = Descs.Num();
    return VH_OK;
}

namespace {
/// Backing store for both field readers. The ABI promises the value -- and any string it points
/// at -- stays valid until the next read, so neither can live on the stack.
vh_value GFieldValue{};
FUtf8String GFieldStorage;
} // namespace

extern "C" int32_t vh_instance_get_field(vh_instance* Instance, const char* NameUtf8, const vh_value** OutValue)
{
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

extern "C" int32_t vh_class_default_field(const char* ClassNameUtf8, const char* NameUtf8, const vh_value** OutValue)
{
    GodotVerse::WaitForBackgroundCheck();
    if (!ClassNameUtf8 || !NameUtf8 || !OutValue)
    {
        return VH_ERR_ABI;
    }
    *OutValue = nullptr;

    if (!GetHost().bInitialized)
    {
        return VH_ERR_STATE;
    }

    if (!GodotVerse::ReadClassDefaultField(Cstr(ClassNameUtf8), Cstr(NameUtf8), GFieldValue, GFieldStorage))
    {
        return VH_ERR_NOT_FOUND;
    }

    *OutValue = &GFieldValue;
    return VH_OK;
}

extern "C" int32_t vh_lookup_symbol(const char* PathUtf8, int32_t Line, int32_t Column, const vh_lookup_desc** OutResult)
{
    GodotVerse::WaitForBackgroundCheck();
    if (!PathUtf8 || !OutResult)
    {
        return VH_ERR_ABI;
    }
    *OutResult = nullptr;

    if (!GetHost().bInitialized)
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
    if (Mode != VH_COMPLETE_MEMBERS && Mode != VH_COMPLETE_SCOPE)
    {
        return VH_ERR_ABI;
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

extern "C" int32_t vh_signature_at(const char* PathUtf8,
                                   const char* SourceUtf8,
                                   int32_t Line,
                                   int32_t Column,
                                   const vh_signature_desc** OutResult)
{
    if (!PathUtf8 || !SourceUtf8 || !OutResult)
    {
        return VH_ERR_ABI;
    }
    *OutResult = nullptr;

    if (!GetHost().bInitialized)
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
