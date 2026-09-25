// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "AutoRTFM.h"

namespace GodotVerse {

/// Makes a fatal error inside the host leave a record before the process ends.
///
/// A failed check reaches FWindowsErrorOutputDevice, which logs to Unreal's own output devices and
/// terminates with code 3. This host has no log file (ALLOW_LOG_FILE=0) and no crash reporter
/// (NOINITCRASHREPORTER=1), so that report reached console output and nothing else, and Godot's
/// log ended mid-run with no reason. A native crash does not need this: Godot's own crash handler
/// catches it, because NOINITCRASHREPORTER keeps Unreal from installing an exception filter.
///
/// Null LogPathUtf8 writes no file. Both arguments are copied.
AUTORTFM_DISABLE void InstallFatalRecorder(const char* LogPathUtf8, bool bShowDialog);

/// Reads VERSE_HOST_TEST_FATAL once, at vh_init. `check` or `access_violation` arms a deliberate
/// failure of that kind for the first vh_tick, which is the only way a test can produce a host
/// fatal error: no script can, by design.
AUTORTFM_DISABLE void ArmTestFatal();

/// Performs the armed failure, if any. Called from vh_tick.
AUTORTFM_DISABLE void FireTestFatal();

} // namespace GodotVerse
