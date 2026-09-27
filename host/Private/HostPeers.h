// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "AutoRTFM.h"
#include "HAL/Platform.h"

class UClass;
class UObject;

/// R-NODE-3's host half: which Godot object a vh_object is the peer of, and which Verse class a
/// Godot handle crosses as.
///
/// A vh_object's block clause asks for its peer from inside its own construction. The host builds
/// one for a node Godot already made, for a handle crossing in, and for the reading device the
/// export defaults come off, and says which peer each has through NewHostObject; one Verse builds
/// for itself mints a peer, which BeginDestroy releases because this unit recorded it. The natives
/// that reach this -- AdoptOrMintPeer, ReleaseMintedPeer, IsMintSuppressed, ObjectForHandle,
/// MirroredClassFor, ClassBaseType -- are declared in HostScript.h with the rest of the host's
/// public surface.
namespace GodotVerse {

/// Which Godot object a vh_object the host builds is the peer of. Chosen before the build starts,
/// because the object's block clause asks for it from inside NewObject; the private constructor
/// leaves the two named ones as the only ways to say.
class FHostPeer
{
public:
    /// Handle -- 0 for deliberately none -- is the peer of the object being built and of nothing
    /// else, so a member its initializers construct still mints its own. FAdoptPeerScope.
    static FHostPeer Adopt(int64 Handle) { return FHostPeer(Handle, false); }

    /// Nothing built while the construction runs gets a Godot object, however deep: the object is
    /// a reading device rather than something an author asked for. FSuppressMintScope.
    static FHostPeer Suppressed() { return FHostPeer(0, true); }

    int64 GetHandle() const { return Handle; }
    bool IsSuppressed() const { return bSuppressed; }

private:
    FHostPeer(int64 InHandle, bool bInSuppressed)
        : Handle(InHandle)
        , bSuppressed(bInSuppressed)
    {
    }

    int64 Handle;
    bool bSuppressed;
};

/// The one host-side construction of a vh_object, with the scope Peer names open around it. Null
/// for a null class.
///
/// A NewObject of a vh_object anywhere else is refused by tools/check_host_constructions.py, which
/// the units layer and build_host.py both run: a construction nobody said the peer of mints one,
/// and leaks it, while a working scene looks entirely normal.
AUTORTFM_DISABLE UObject* NewHostObject(UClass* Class, FHostPeer Peer);

/// Empties the two caches that answer "what does this cross as" and "what does this mint", both of
/// which a new binding roster changes.
AUTORTFM_DISABLE void ForgetCachedClasses();

} // namespace GodotVerse
