// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "AutoRTFM.h"
#include "Containers/Array.h"
#include "Containers/StringView.h"
#include "Containers/Utf8String.h"
#include "HostScript.h"
#include "verse_host_abi.h"

namespace Verse {
struct VFunction;
}

namespace GodotVerse {

/// Installs or removes FGodotDebugger, and sets HasDebuggerBit on every live context so that a
/// debugger attached mid-run takes effect (VVMContextImpl.cpp only samples the bit when a context
/// is claimed).
///
/// False when Epic's own socket debugger holds the VM's single debugger slot. `SetDebugger` is one
/// global pointer, so the two cannot coexist and the one asked for at vh_init wins.
AUTORTFM_DISABLE bool SetDebugEnabled(bool bEnabled);
AUTORTFM_DISABLE bool IsDebugEnabled();

/// Whether a stop is on the stack right now -- which is to say whether Godot's debug loop is
/// running inside our Notify. Every entry point that would execute Verse is refused while it is
/// (VH_ERR_STOPPED); the three vh_debug_* reads are the only ones that answer.
AUTORTFM_DISABLE bool IsDebugStopped();

/// One named value of a stopped frame. Exactly one of the two halves is filled: Value when the
/// bridge carries the type, Rendered otherwise.
struct FDebugValue
{
    FUtf8String Name;
    bool bHasValue{false};
    vh_value Value{};
    /// Backs Value's own bytes -- a string's, a math struct's components. Held per value rather
    /// than shared, because the descriptor array the ABI hands back points straight into these,
    /// and it survives the array around it growing: what Value names is a heap allocation, and
    /// moving an FDebugValue moves only the headers that own one.
    FFieldStorage Storage;
    FUtf8String Rendered;
};

/// The three reads, all defined only while stopped. Each answers false when nothing is, or when
/// Level names no frame.
AUTORTFM_DISABLE bool DebugStackCount(int32& OutCount);
AUTORTFM_DISABLE bool DebugStackFrame(int32 Level, FUtf8String& OutPath, FUtf8String& OutName, int32& OutLine);
/// Borrowed, not copied: a marshalled string's vh_value points into the FDebugValue that owns it,
/// so a copy would leave the copy's vh_value naming the original's bytes. The array lives as long
/// as the stop does, which is longer than any caller of this.
AUTORTFM_DISABLE const TArray<FDebugValue>* DebugStackValues(int32 Level, bool bMembers);

/// ---------------------------------------------------------------------- the profiler --

AUTORTFM_DISABLE void SetProfilingEnabled(bool bEnabled);
AUTORTFM_DISABLE bool IsProfilingEnabled();

/// One accumulated row, keyed by its GDScript-shaped signature.
struct FProfileRow
{
    FUtf8String Signature;
    int64 CallCount{0};
    double TotalSeconds{0.0};
    double SelfSeconds{0.0};
};

/// Reads the rows. FrameOnly answers what this frame accumulated and resets it; otherwise the
/// whole run's.
AUTORTFM_DISABLE void ReadProfile(bool bFrameOnly, TArray<FProfileRow>& OutRows);

/// The GDScript-shaped signature for one Verse function -- `<file>::<line>::<owner>.<name>` --
/// cached by procedure, so a method called sixty times a second builds its string once.
///
/// Empty when profiling is off, when the function has no procedure, or when its procedure carries
/// no file path (which is every function of the generated mirror: the codegen only fills FilePath
/// for a package with source behind it).
///
/// The line is the procedure's *first located op* rather than the declaration's own row. The
/// declaration's row lives in the semantic program, and no call may reach for that: reading it is
/// what the snapshot rule exists to prevent. The two differ by the signature line and nothing
/// else, and the editor's profiler uses the number only to jump.
AUTORTFM_DISABLE FUtf8StringView ProfileSignature(Verse::VFunction* Function, FUtf8StringView Owner);

/// Times one boundary crossing, and subtracts its total from whatever crossing encloses it -- so a
/// Verse method that emits a signal that calls another Verse method attributes correctly.
///
/// Costs one relaxed load and a predicted branch when profiling is off, which is the whole of D15:
/// a caller builds nothing until ProfileSignature has said profiling is on, and an empty signature
/// makes this scope inert.
class FProfileScope
{
public:
    AUTORTFM_DISABLE explicit FProfileScope(FUtf8StringView Signature);
    AUTORTFM_DISABLE ~FProfileScope();

    FProfileScope(const FProfileScope&) = delete;
    FProfileScope& operator=(const FProfileScope&) = delete;

private:
    bool bActive{false};
    double Started{0.0};
    /// Filled by nested scopes as they pop, and subtracted from this one's total.
    double ChildSeconds{0.0};
    FProfileScope* Parent{nullptr};
    FUtf8StringView Signature;
};

/// Resets both accumulators and forgets every row. vh_shutdown's, and _profiling_start's.
AUTORTFM_DISABLE void ResetProfile();

} // namespace GodotVerse
