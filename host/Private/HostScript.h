// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "AutoRTFM.h"
#include "Containers/Array.h"
#include "Containers/UnrealString.h"
#include "VerseString.h"

namespace GodotVerse {

struct FScript
{
    FUtf8String Path;
    /// Verse path of the module this file's definitions live in. Every snippet in a project
    /// shares one flat scope and Verse forbids shadowing, so two files that both define a
    /// top-level Ready() are a compile error; each file wraps its definitions in a module named
    /// after its own stem instead.
    FUtf8String ModulePath;
};

/// Creates the placeholder outer and enters the content scope Verse allocations need.
AUTORTFM_DISABLE bool EnterContentScope();
AUTORTFM_DISABLE void LeaveContentScope();

AUTORTFM_DISABLE FScript* CompileFile(const FUtf8String& Path);

/// Adds every path as a data source and builds them as one program. Callable once per process:
/// a second BuildAll re-notifies already-loaded native Verse packages and aborts in the async
/// loader, so a rebuild is refused with a diagnostic rather than attempted.
AUTORTFM_DISABLE bool CompileProject(const TArray<FUtf8String>& Paths);
AUTORTFM_DISABLE FScript* OpenScript(const FUtf8String& Path);
AUTORTFM_DISABLE void ReleaseScript(FScript* Script);

AUTORTFM_DISABLE bool HasFunction(const FScript* Script, FUtf8StringView DecoratedName);
AUTORTFM_DISABLE int32 RunMain(const TArray<verse::string>& Args, int64& OutExitCode);
AUTORTFM_DISABLE int32 CallVoid(const FScript* Script, FUtf8StringView DecoratedName);
AUTORTFM_DISABLE int32 CallVoidFloat(const FScript* Script, FUtf8StringView DecoratedName, double Arg);

AUTORTFM_DISABLE void TickScripts(double BudgetSeconds);

/// Releases the IDE, its data sources and the content scope. Must run before the engine tears
/// down: these objects free through GMalloc, which AppExit takes with it.
AUTORTFM_DISABLE void ResetScriptState();

} // namespace GodotVerse
