// Copyright Epic Games, Inc. All Rights Reserved.

#include "HostEventLoop.h"
#include "AutoRTFM.h"
#include "Containers/Queue.h"
#include "HAL/PlatformTime.h"
#include "Misc/Optional.h"
#include "Templates/UnrealTemplate.h"
#include "UObject/GarbageCollection.h"
#include "UObject/ReachabilityAnalysis.h"
#include "UObject/UObjectArray.h"

namespace {
TQueue<TFunction<void(const verse::FExecutionContext&)>> GEnqueuedAsyncJobs;
TOptional<GodotVerse::FRunExit> GPendingExit;
}

void GodotVerse::EnqueueAsyncJob(TFunction<void(const verse::FExecutionContext&)>&& JobThunk)
{
    AutoRTFM::OnCommit(
        [JobThunk = MoveTemp(JobThunk)]() mutable { verify(GEnqueuedAsyncJobs.Enqueue(MoveTemp(JobThunk))); });
}

void GodotVerse::RequestExit(FRunExit Exit)
{
    GPendingExit.Emplace(MoveTemp(Exit));
}

bool GodotVerse::HasPendingExit()
{
    return GPendingExit.IsSet();
}

GodotVerse::FRunExit GodotVerse::ConsumeExit()
{
    FRunExit Exit = GPendingExit.IsSet() ? *GPendingExit : FRunExit::Error();
    GPendingExit.Reset();
    return Exit;
}

AUTORTFM_DISABLE static void TickGC()
{
    const int32 ObjectArrayFreeSlots = GUObjectArray.GetObjectArrayEstimatedAvailable();

    if (IsIncrementalReachabilityAnalysisPending())
    {
        PerformIncrementalReachabilityAnalysis(GetReachabilityAnalysisTimeLimit());
    }
    else if (IsIncrementalPurgePending())
    {
        IncrementalPurgeGarbage(true);
    }
    else if (ObjectArrayFreeSlots < 10000 || UE::GC::ShouldFrankenGCRun())
    {
        CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS, /*bPerformFullPurge*/ false);
    }
}

AUTORTFM_DISABLE void GodotVerse::PumpEventLoop(const verse::FExecutionContext& ExecContext, double BudgetSeconds)
{
    const double StartTime = FPlatformTime::Seconds();

    TickGC();

    TFunction<void(const verse::FExecutionContext&)> JobThunk;
    while (!GPendingExit.IsSet() && GEnqueuedAsyncJobs.Dequeue(JobThunk))
    {
        JobThunk(ExecContext);

        if (BudgetSeconds > 0.0 && FPlatformTime::Seconds() - StartTime >= BudgetSeconds)
        {
            break;
        }
    }
}
