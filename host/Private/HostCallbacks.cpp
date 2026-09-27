// Copyright Epic Games, Inc. All Rights Reserved.

#include "HostCallbacks.h"

#include "Containers/Map.h"
#include "HAL/CriticalSection.h"
#include "HAL/PlatformTLS.h"
#include "Misc/ScopeLock.h"

#include <atomic>

namespace GodotVerse {
namespace {

int64 GNextCallbackId = 1;

/// The lock, held for one operation on the map, and the only way to reach the map at all.
///
/// Why the table needs one: vh_callback_release is deliberately unguarded (R-ASYNC-8's exception,
/// argued at its definition), because a Godot Callable is destroyed on whatever thread dropped its
/// last reference and refusing that would leak the row instead. Releasing never enters the VM, so
/// allowing it is safe -- but a TMap::Remove racing a Find on the game thread is not, and that is
/// what this closes.
///
/// The map is a private static rather than a file global so that nothing in this file can name it
/// without holding one of these; Map() asserts the rest in a development host.
class FLockedCallbacks
{
public:
    AUTORTFM_DISABLE FLockedCallbacks()
        : Lock(&Mutex)
    {
        Holder.store(FPlatformTLS::GetCurrentThreadId(), std::memory_order_relaxed);
    }

    AUTORTFM_DISABLE ~FLockedCallbacks() { Holder.store(0, std::memory_order_relaxed); }

    FLockedCallbacks(const FLockedCallbacks&) = delete;
    FLockedCallbacks& operator=(const FLockedCallbacks&) = delete;

    AUTORTFM_DISABLE TMap<int64, FCallbackTarget>& Map()
    {
        check(Holder.load(std::memory_order_relaxed) == FPlatformTLS::GetCurrentThreadId());
        return Callbacks;
    }

private:
    static FCriticalSection Mutex;
    static TMap<int64, FCallbackTarget> Callbacks;
    /// The thread inside the lock, or 0. Written only by that thread, after acquiring and before
    /// releasing, so the lock itself orders every read of it that matters.
    static std::atomic<uint32> Holder;

    FScopeLock Lock;
};

FCriticalSection FLockedCallbacks::Mutex;
TMap<int64, FCallbackTarget> FLockedCallbacks::Callbacks;
std::atomic<uint32> FLockedCallbacks::Holder{0};

} // namespace

AUTORTFM_DISABLE int64 AddCallback(FCallbackTarget Target)
{
    const int64 CallbackId = GNextCallbackId++;
    FLockedCallbacks Locked;
    Locked.Map().Add(CallbackId, MoveTemp(Target));
    return CallbackId;
}

AUTORTFM_DISABLE void RemoveCallback(int64 CallbackId)
{
    FLockedCallbacks Locked;
    Locked.Map().Remove(CallbackId);
}

AUTORTFM_DISABLE TResult<FCallbackTarget> FindCallback(int64 CallbackId)
{
    FLockedCallbacks Locked;
    const FCallbackTarget* const Found = Locked.Map().Find(CallbackId);
    if (!Found)
    {
        return EHostFailure::UnknownId;
    }
    return *Found;
}

} // namespace GodotVerse
