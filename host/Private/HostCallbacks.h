// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "AutoRTFM.h"
#include "Containers/UnrealString.h"
#include "HostResult.h"

/// The callback registry: every Callable the host has handed Godot, by the id Godot calls it back
/// with. The one table in the host that is touched off the game thread, and so the one behind a
/// lock -- which this unit owns, along with the map, so no caller can reach one without the other.
namespace GodotVerse {

/// A Verse function Godot holds as a Callable, as the pair that can name it again later.
///
/// Not the function *value*: 4a accepts only a method bound to a script instance (OQ-16 carries
/// the unbound case), and for one of those the owner's handle and the method's decorated name say
/// everything -- which means nothing here has to keep a VM cell alive, and invoking is the same
/// InstanceCall path Godot's own dispatch takes, argument conversion and all.
struct FCallbackTarget
{
    int64 OwnerHandle = 0;
    FUtf8String DecoratedName;

    /// Non-zero for a Callable the host minted to feed a suspended task rather than to call a
    /// script method: the token of the row in GAwaiters. Nothing a script hands to
    /// `MakeCallable` ever carries one.
    int64 AwaitToken = 0;

    /// Pack the emission's arguments into one Godot Array before dispatching. What a subscriber to
    /// a *foreign* signal receives, because nothing declares that signal's payload and there is no
    /// per-argument shape to convert against.
    bool bArgsAsArray = false;

    /// Non-zero for the permanent connection an `@export_signal` event member holds: the binding
    /// whose `Event` this emission is signalled into. The event-member analogue of AwaitToken, and
    /// exclusive with it -- an await is one wait, this is every emission for the instance's life.
    int64 EventSignalId = 0;
};

/// Registers Target under a fresh id and answers the id. Game thread only: the id counter is not
/// under the lock, because only the game thread mints one.
AUTORTFM_DISABLE int64 AddCallback(FCallbackTarget Target);

/// Drops a row, if there is one. Safe from any thread: a Godot Callable is destroyed on whatever
/// thread dropped its last reference, and vh_callback_release is deliberately unguarded for it.
AUTORTFM_DISABLE void RemoveCallback(int64 CallbackId);

/// A copy of the row, taken under the lock, or UnknownId. A copy rather than a pointer, because the
/// caller goes on to run Verse and a Callable released on another thread meanwhile would take the
/// row, and the pointer, with it.
AUTORTFM_DISABLE TResult<FCallbackTarget> FindCallback(int64 CallbackId);

} // namespace GodotVerse
