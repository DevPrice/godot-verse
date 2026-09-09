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
};

/// Creates the placeholder outer and enters the content scope Verse allocations need.
AUTORTFM_DISABLE bool EnterContentScope();
AUTORTFM_DISABLE void LeaveContentScope();

AUTORTFM_DISABLE FScript* CompileFile(const FUtf8String& Path);
AUTORTFM_DISABLE void ReleaseScript(FScript* Script);

AUTORTFM_DISABLE bool HasFunction(FUtf8StringView DecoratedName);
AUTORTFM_DISABLE int32 RunMain(const TArray<verse::string>& Args, int64& OutExitCode);
AUTORTFM_DISABLE int32 CallVoid(FUtf8StringView DecoratedName);
AUTORTFM_DISABLE int32 CallVoidFloat(FUtf8StringView DecoratedName, double Arg);

AUTORTFM_DISABLE void TickScripts(double BudgetSeconds);

} // namespace GodotVerse
