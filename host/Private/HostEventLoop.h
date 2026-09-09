// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "AutoRTFM.h"
#include "HAL/Platform.h"
#include "Templates/Function.h"

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

/// Records why the current run should stop. Unlike VerseCmd's event loop this may be called
/// many times over the life of the host - once per script run.
void RequestExit(FRunExit Exit);
bool HasPendingExit();
FRunExit ConsumeExit();

/// Drains queued jobs until the queue empties, an exit is requested, or the budget expires.
/// A budget <= 0 means run to completion.
AUTORTFM_DISABLE void PumpEventLoop(const verse::FExecutionContext& ExecContext, double BudgetSeconds);

} // namespace GodotVerse
