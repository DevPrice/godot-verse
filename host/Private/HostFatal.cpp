// Copyright Epic Games, Inc. All Rights Reserved.

#include "HostFatal.h"

#include "Containers/StringConv.h"
#include "Containers/UnrealString.h"
#include "CoreGlobals.h"
#include "HAL/PlatformMisc.h"
#include "Misc/AssertionMacros.h"
#include "Misc/CoreDelegates.h"
#include "Misc/FileHelper.h"

namespace GodotVerse {

namespace {

FString GFatalLogPath;
bool GShowFatalDialog = false;

enum class ETestFatal
{
    None,
    Check,
    AccessViolation,
};

ETestFatal GArmedTestFatal = ETestFatal::None;

/// Runs on whichever thread failed, after FWindowsErrorOutputDevice::HandleError has cleared
/// GIsGuarded and before it terminates the process. GErrorHist carries the message and, for a failed
/// check, the stack CheckVerifyFailedImpl walked. Nothing here calls into Godot: the failure may be
/// on a worker thread, and Godot's state is not something to trust at this point.
void RecordFatal()
{
    const FString Text = FString::Printf(
        TEXT("Verse host fatal error. This is a failure inside the Verse bridge or Unreal Engine, not in a ")
        TEXT("script, and the process ended.\n\n%s"),
        GErrorHist).TrimEnd();
    if (!GFatalLogPath.IsEmpty())
    {
        FFileHelper::SaveStringToFile(Text, *GFatalLogPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
    }
    if (GShowFatalDialog)
    {
        FPlatformMisc::MessageBoxExt(EAppMsgType::Ok, *Text, TEXT("Verse"));
    }
}

} // namespace

void InstallFatalRecorder(const char* LogPathUtf8, bool bShowDialog)
{
    GFatalLogPath = LogPathUtf8
        ? FString(StringCast<TCHAR>(reinterpret_cast<const UTF8CHAR*>(LogPathUtf8)).Get())
        : FString();
    GShowFatalDialog = bShowDialog;
    FCoreDelegates::OnHandleSystemError.AddStatic(&RecordFatal);
}

void ArmTestFatal()
{
    const FString Value = FPlatformMisc::GetEnvironmentVariable(TEXT("VERSE_HOST_TEST_FATAL"));
    if (Value == TEXT("check"))
    {
        GArmedTestFatal = ETestFatal::Check;
    }
    else if (Value == TEXT("access_violation"))
    {
        GArmedTestFatal = ETestFatal::AccessViolation;
    }
}

void FireTestFatal()
{
    const ETestFatal Armed = GArmedTestFatal;
    GArmedTestFatal = ETestFatal::None;
    switch (Armed)
    {
    case ETestFatal::Check:
        checkf(false, TEXT("VERSE_HOST_TEST_FATAL=check asked for a failed check."));
        break;
    case ETestFatal::AccessViolation:
    {
        // A volatile pointer, so the compiler cannot prove the write undefined and drop it.
        volatile int32* volatile Null = nullptr;
        *Null = 0;
        break;
    }
    case ETestFatal::None:
        break;
    }
}

} // namespace GodotVerse
