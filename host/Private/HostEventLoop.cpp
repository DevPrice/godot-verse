// Copyright Epic Games, Inc. All Rights Reserved.

#include "HostEventLoop.h"
#include "AutoRTFM.h"
#include "Containers/Queue.h"
#include "HAL/PlatformTime.h"
#include "Math/UnrealMathUtility.h"
#include "Containers/Array.h"
#include "Misc/Optional.h"
#include "Templates/UnrealTemplate.h"
#include "HAL/PlatformProcess.h"
#include "UObject/GarbageCollection.h"
#include "UObject/ReachabilityAnalysis.h"
#include "UObject/UObjectArray.h"
#include "VerseVM/VVMCollectionCycleRequest.h"
#include "VerseVM/VVMHeap.h"

namespace {
TQueue<TFunction<void(const verse::FExecutionContext&)>> GEnqueuedAsyncJobs;
/// What is in the queue. Counted rather than asked: TQueue has no size and no iterator, and
/// R-ASYNC-6's whole point is that the depth be readable from outside.
int32 GQueueDepth = 0;
TOptional<GodotVerse::FRunExit> GPendingExit;

/// One suspended `Sleep`. Kept in an array rather than a heap: a project has a handful of these at
/// a time, and the pump already walks the whole list to find what is due.
struct FSleeper
{
    double Deadline = 0.0;
    TFunction<void()> Resume;
};
TArray<FSleeper> GSleepers;
}

void GodotVerse::EnqueueAsyncJob(TFunction<void(const verse::FExecutionContext&)>&& JobThunk)
{
    AutoRTFM::OnCommit(
        [JobThunk = MoveTemp(JobThunk)]() mutable {
            verify(GEnqueuedAsyncJobs.Enqueue(MoveTemp(JobThunk)));
            ++GQueueDepth;
        });
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

AUTORTFM_DISABLE void GodotVerse::EnqueueSleep(double DelaySeconds, TFunction<void()>&& Resume)
{
    GSleepers.Add(FSleeper{FPlatformTime::Seconds() + FMath::Max(DelaySeconds, 0.0), MoveTemp(Resume)});
}

AUTORTFM_DISABLE void GodotVerse::ResetEventLoop()
{
    GEnqueuedAsyncJobs.Empty();
    GQueueDepth = 0;
    GSleepers.Empty();
    GPendingExit.Reset();
}

/// Resumes every sleep whose deadline has passed, oldest deadline first.
///
/// The due set is taken out of the list *before* any of it runs, because a resumed task may sleep
/// again -- `loop { Sleep(0.0) }` is a frame-yielding loop, and one that re-entered the list it was
/// being walked would spin the whole frame away.
///
/// Not budgeted. The budget governs the job queue below, where a job is arbitrary queued work; a
/// sleep that is due is due, and holding one over is a frame of drift an author cannot see. See
/// vh_tick.
///
/// That survives the pump stopping for a whole background analysis, which it does in the editor,
/// because a deadline is stamped in EnqueueSleep rather than accrued: a gap of any length leaves at
/// most one due deadline per sleeping task, so the burst is bounded by GSleepers.Num() and not by
/// the gap. Drained rather than spread, by decision -- spec R-ASYNC-6.
AUTORTFM_DISABLE static void WakeSleepers()
{
    if (GSleepers.IsEmpty())
    {
        return;
    }
    const double Now = FPlatformTime::Seconds();

    TArray<FSleeper> Due;
    for (int32 Index = GSleepers.Num() - 1; Index >= 0; --Index)
    {
        if (GSleepers[Index].Deadline <= Now)
        {
            Due.Add(MoveTemp(GSleepers[Index]));
            GSleepers.RemoveAt(Index);
        }
    }
    // RemoveAt from the back reversed them; a deterministic order is R-ASYNC-3's and the earliest
    // deadline has the better claim to it.
    Due.Sort([](const FSleeper& A, const FSleeper& B) { return A.Deadline < B.Deadline; });
    for (FSleeper& Sleeper : Due)
    {
        Sleeper.Resume();
    }
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

AUTORTFM_DISABLE void GodotVerse::CollectGarbageNow()
{
    // There are two collectors and only one of them can free a Verse object's UObject shadow.
    // UE's own pass treats every cell the VM still holds as a root, so on its own it destroys
    // nothing a script let go of -- which is what made "a peer is released when Verse drops it"
    // read as false when it was only unasked. The pass that can is UE's *coupled* to the VM's
    // ("FrankenGC"), and `CollectGarbage` takes that shape only while the VM's collector is
    // signalling that it wants to start.
    //
    // So: ask the VM for a fresh cycle -- the documented basis of a synchronous "GC now", and
    // deliberately **not waited on**, because waiting for the VM's collector from a thread holding
    // heap access is the deadlock docs/abi-v2-design.md 1a found -- then give its collector the
    // moment it needs to raise the signal, and collect while it is up.
    Verse::FHeap::RequestFreshCollectionCycle();
    for (int32 Spin = 0; Spin < 1000 && !UE::GC::ShouldFrankenGCRun(); ++Spin)
    {
        FPlatformProcess::Sleep(0.001f);
    }

    // A full purge, unlike TickGC's: an incremental one leaves BeginDestroy for a later slice, and
    // the whole point of asking is to make the releases that hang off it happen now. An incremental
    // pass already in flight is finished first, because CollectGarbage asserts on one otherwise.
    if (IsIncrementalReachabilityAnalysisPending())
    {
        PerformIncrementalReachabilityAnalysis(0.0);
    }
    if (IsIncrementalPurgePending())
    {
        IncrementalPurgeGarbage(false);
    }
    CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS, /*bPerformFullPurge*/ true);
}

AUTORTFM_DISABLE void GodotVerse::PumpEventLoop(const verse::FExecutionContext& ExecContext,
                                                double BudgetSeconds,
                                                vh_tick_stats* OutStats)
{
    const double StartTime = FPlatformTime::Seconds();

    TickGC();
    WakeSleepers();

    int32 JobsRun = 0;
    bool bOverran = false;
    TFunction<void(const verse::FExecutionContext&)> JobThunk;
    while (!GPendingExit.IsSet() && GEnqueuedAsyncJobs.Dequeue(JobThunk))
    {
        --GQueueDepth;
        JobThunk(ExecContext);
        ++JobsRun;

        if (BudgetSeconds > 0.0 && FPlatformTime::Seconds() - StartTime >= BudgetSeconds)
        {
            // Only an overrun if something was left behind. A pump that spent its whole budget and
            // emptied the queue did exactly what it was for, and reporting that would make the
            // number useless for finding the frames that matter.
            bOverran = GQueueDepth > 0;
            break;
        }
    }

    if (OutStats)
    {
        OutStats->JobsRun = JobsRun;
        OutStats->JobsPending = GQueueDepth;
        OutStats->Sleeping = GSleepers.Num();
        OutStats->ElapsedSeconds = FPlatformTime::Seconds() - StartTime;
        OutStats->Overran = bOverran ? 1 : 0;
    }
}
