// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "AutoRTFM.h"
#include "Containers/StringView.h"
#include "Containers/UnrealString.h"
#include "Containers/Utf8String.h"
#include "Templates/SharedPointer.h"

class UObject;

namespace Verse {
struct VFunction;
}

/// Instance dispatch: a script's class bound to one Godot object -- its construction and release,
/// calls into it, reads and writes of its members, its declared defaults -- and the Callables that
/// name one of its methods.
///
/// Instantiate, InstanceCall, the field reads and writes and the callback entry points are declared
/// in HostScript.h, which is the surface VerseHost.cpp and GodotBindings.cpp reach the host
/// through. What is here is what the rest of the host calls.
namespace GodotVerse {

struct FFieldValue;

/// The Verse object of the script instance bound to Handle, or null for a handle no live instance
/// is bound to.
AUTORTFM_DISABLE UObject* InstanceObjectForHandle(int64 Handle);

/// The largest number of live tasks any one instance's scope holds.
AUTORTFM_DISABLE int32 PeakTasksOfAnyInstance();

/// One class's declared defaults, read off a transient instance of the published class.
///
/// The instance is built once per class rather than once per member, which is what makes reading
/// every `@export` into the snapshot cost about what reading one used to: UVerseClass runs the
/// Verse constructor from PostInitInstance, which NewObject drives and class-default-object
/// construction does not, so a CDO's members read back uninitialized and an instance is the only
/// place a declared default actually exists. Handle is left unset: reading a plain data member
/// never consults it.
AUTORTFM_DISABLE UObject* NewDefaultsObject(FUtf8StringView ClassName);

/// One member read off what NewDefaultsObject built, or null.
AUTORTFM_DISABLE TSharedPtr<const FFieldValue> ReadDefaultFieldOf(UObject* Defaults, FUtf8StringView FieldName);

/// The UObject a class-typed member holds, or null.
///
/// The read half of WriteFieldOf, narrowed to the one case signal binding needs: a `signal`
/// member's own object, so the host can write the id into it.
AUTORTFM_DISABLE UObject* PeekFieldObject(UObject* Object, FUtf8StringView FieldName);

/// The decorated name of a bound Verse method, found by asking the object for each method its
/// class declares and comparing the function that comes back.
///
/// There is no reading the semantic program's spelling back off a VFunction -- the bytecode has
/// erased it -- so the comparison is the lookup, which is the same thing InstanceHasFunction does
/// to tell an override from an inherited body. Once per Subscribe, never per emission.
AUTORTFM_DISABLE bool DescribeBoundFunction(Verse::VFunction* Function, int64& OutHandle, FUtf8String& OutDecorated);

} // namespace GodotVerse
