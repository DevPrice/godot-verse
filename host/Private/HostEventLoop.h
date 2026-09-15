// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "AutoRTFM.h"
#include "HAL/Platform.h"
#include "Templates/Function.h"
#include "verse_host_abi.h"

namespace verse {
struct FExecutionContext;
}

namespace GodotVerse {

struct FRunExit
{
    enum class EReason
    {
        Completed,
        ExitCalled,
        Error
    };
    EReason Reason{EReason::Error};
    int64 ExitCode{1};

    static constexpr FRunExit Completed() { return {EReason::Completed, 0}; }
    static constexpr FRunExit ExitCalled(int64 Code) { return {EReason::ExitCalled, Code}; }
    static constexpr FRunExit Error() { return {EReason::Error, 1}; }
};

void EnqueueAsyncJob(TFunction<void(const verse::FExecutionContext&)>&& JobThunk);

/// Runs Resume at the first PumpEventLoop whose clock has passed DelaySeconds from now, which is
/// how `Sleep` suspends (R-ASYNC-1, phase-5-design.md D6).
///
/// The clock is `FPlatformTime::Seconds()` and not a `SceneTreeTimer`, because this has to work
/// where there is no scene tree: the smoke test, the probe, a `@tool` script. What it costs is on
/// the Verse declaration -- real time, so it ignores `Engine.time_scale` and keeps counting under
/// `--fixed-fps` and in a paused game.
///
/// A delay at or below zero resumes at the next pump rather than immediately: a native coroutine
/// that returns before it suspends is a different control flow, and "resume next frame" is the
/// cheapest useful meaning for `Sleep(0.0)`.
///
/// Unlike EnqueueAsyncJob this does **not** defer to transaction commit. The caller is a suspended
/// task, so there is no commit to wait for -- if the transaction the sleep started in aborts, the
/// task is unwound and the resumption finds nothing to resume.
AUTORTFM_DISABLE void EnqueueSleep(double DelaySeconds, TFunction<void()>&& Resume);

/// Records why the current run should stop. Unlike VerseCmd's event loop this may be called
/// many times over the life of the host - once per script run.
void RequestExit(FRunExit Exit);
bool HasPendingExit();
FRunExit ConsumeExit();

/// Drains queued jobs until the queue empties, an exit is requested, or the budget expires.
/// A budget <= 0 means run to completion.
///
/// OutStats may be null; when it is not, it is filled with what this pump did (R-ASYNC-6). It is
/// the ABI's own struct rather than one of ours because nothing in between would add anything, and
/// a second shape is a second thing to keep in step.
AUTORTFM_DISABLE void PumpEventLoop(const verse::FExecutionContext& ExecContext,
                                    double BudgetSeconds,
                                    struct vh_tick_stats* OutStats = nullptr);

/// Drops queued work while the allocator is still alive. Nothing here may outlive AppExit.
AUTORTFM_DISABLE void ResetEventLoop();

/// A whole collection, rather than the incremental slice PumpEventLoop takes when the object array
/// runs short. What vh_collect_garbage is: the only way to make "Verse has dropped this" happen,
/// which a reference table entry and a Verse-minted Godot object both wait on.
AUTORTFM_DISABLE void CollectGarbageNow();

} // namespace GodotVerse
