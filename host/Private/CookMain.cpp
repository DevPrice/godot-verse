// Copyright Epic Games, Inc. All Rights Reserved.

// The cooker's entry point. Each target sets VH_HOST_KIND, and this file is the cooker's alone:
// the two DLL hosts have no main and are entered through the ABI. Phase 7 stage 1
// (phase-7-design.md §5) gives this a body; the S-1 spike needed only an executable that links
// and boots, because a monolithic editor-class DLL exports every module's API symbols and
// lld-link stops at 65535 of them.

#include "verse_host_abi.h"

#if VH_HOST_KIND == VH_HOST_KIND_COOKER

#include "CoreMinimal.h"
#include "LaunchEngineLoop.h"

DEFINE_LOG_CATEGORY_STATIC(LogVerseCook, Log, All);

// AUTORTFM_DISABLE on the entry point itself, the way AutoRTFMTests.cpp:188 writes its `main`:
// this target is built by the AutoRTFM clang (bUseAutoRTFMCompiler), GEngineLoop.Exit() carries
// the attribute, and instrumented code may not call an uninstrumented function -- so the whole
// chain from the entry point down has to be disabled, exactly as the ABI entry points in
// VerseHost.cpp are. On Windows the macro is `int32 wmain(...)` and the attribute lands on it;
// a non-Windows cooker expands to a `tchar_main` forward declaration plus a `main`, and would
// need the attribute moved onto the former.
AUTORTFM_DISABLE INT32_MAIN_INT32_ARGC_TCHAR_ARGV()
{
	FTaskTagScope Scope(ETaskTag::EGameThread);

	// The editor host's own boot line (VerseHost.cpp), plus four things an engine-class boot
	// needs that a DLL host never met. -NoPreviewPlatforms: WITH_EDITOR would otherwise load
	// every platform's config (UnrealAssetStringify). The EDITOR token: under WITH_EDITOR &&
	// WITH_ENGINE, PreInit refuses to run without a project file unless it is running as the
	// editor or as a commandlet (LaunchEngineLoop.cpp:2715-2725); ChaosVisualDebugger appends
	// the same word. -NoShaderCompile: PreInit constructs FShaderCompilingManager either way,
	// and without this it launches ShaderCompileWorker.exe, which nothing here builds.
	// -nullrhi: there is nothing to draw.
	if (const int32 Result = GEngineLoop.PreInit(ArgC, ArgV, TEXT(" EDITOR -NOCONSOLE -nullrhi -NoShaderCompile -AssetGatherAll=0 -NoPreviewPlatforms")))
	{
		return Result;
	}

	UE_LOG(LogVerseCook, Display, TEXT("verse_cook: engine booted (WITH_EDITOR=%d, WITH_ENGINE=%d)"), WITH_EDITOR, WITH_ENGINE);

	// GEngineLoop.Exit(), not the editor host's hand-rolled AppPreExit/UnloadModules/AppExit:
	// that sequence is for a target with no Engine, and ChaosVisualDebugger's `-RUN=` path is
	// this exact shape -- PreInit, work, Exit, with no Init and no Tick. Not in an ON_SCOPE_EXIT
	// because Exit() is AUTORTFM_DISABLE and an instrumented lambda may not call one.
	//
	// THIS DOES NOT SHUT DOWN CLEANLY. The process segfaults at the end of teardown, after
	// "Destroying PakPlatformFile" and past anything a cook would have written, so the exit code
	// is 139 on every run and means nothing. Exit() gets further than the hand-rolled sequence
	// did and FIoDispatcher::Shutdown() made no difference; phase-7-design.md 2 S-1 records both.
	// Settling how this program reports success is Phase 7 stage 1's first job, before the cook.
	RequestEngineExit(TEXT("verse_cook exiting"));
	GEngineLoop.Exit();
	return 0;
}

#endif // VH_HOST_KIND == VH_HOST_KIND_COOKER
