// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "AutoRTFM.h"
#include "Containers/Array.h"
#include "HostScript.h"
#include "HostSnapshot.h"
#include "Templates/SharedPointer.h"
#include "Containers/UnrealString.h"

class FJsonObject;

namespace GodotVerse {

/// The class shape an exported game ships instead of its sources (D9).
///
/// It is the analysis snapshot, serialised: the same struct the seven class-describing entry
/// points already answer from, so a runtime host loads one and those bodies are unchanged. The
/// writer and the reader are two functions over one struct in one file, which is the whole reason
/// the sidecar is the snapshot rather than a format of its own.
///
/// What it deliberately does not carry: `@export` defaults, completion items and override
/// candidates. Defaults come from the initializer a cooked class still runs -- which is what both
/// shipping Godot languages do outside TOOLS_ENABLED -- and the completion halves have no reader
/// without a compiler.
///
/// JSON, not a packed format, because a cook you can open is worth more than the milliseconds:
/// when an exported game does not see a class, the first question is whether the cook wrote one.

/// Writes the current analysis snapshot to Path, plus the UPackage paths the cook wrote.
///
/// The package list is not part of the snapshot and is here because this is the one file an
/// exported game reads before it has loaded anything: a container carries package *ids*, which
/// are hashes, so the runtime host cannot recover a name from what it mounted.
///
/// False with OutError set when there is no snapshot to write or the file cannot be written.
AUTORTFM_DISABLE bool WriteClassSidecar(const FString& Path, const TArray<FString>& CookedPackages,
    int32 Generation, FUtf8String& OutError);

/// Reads one back and publishes it as *the* snapshot, and installs the binding table beside it.
///
/// BindingsPackage is the package the cook published the bindings as, which only the caller knows:
/// it comes off the mount points, where a runtime host learns every other package name too. Empty
/// when the cook carried no bindings, and then the table has nothing to look up anyway.
///
/// False with OutError set when the file is missing, unparseable, or written by a different
/// sidecar version.
AUTORTFM_DISABLE bool LoadClassSidecar(const FString& Path, FUtf8StringView BindingsPackage,
    FUtf8String& OutError);

/// Reads the sidecar's header -- the stamp and the package list -- and nothing else.
///
/// This is what an exported game reads before it has mounted or loaded anything, so it is also
/// where the three refusals live (D6): the file is missing, the file is unreadable or written by
/// a sidecar version this host does not read, or the stamp says a different build of godot-verse
/// cooked it. Each is a different sentence, and the consumer prints OutError verbatim.
AUTORTFM_DISABLE bool ReadCookedManifest(const FString& Path, TArray<FString>& OutPackages,
    int32& OutGeneration, FUtf8String& OutError);

/// One `@export` description as JSON, and back.
///
/// Exposed because a declared type carries an FExportDesc of its own (FMemberType::Described), and
/// two writers for one struct is two chances to drop a field from one of them.
AUTORTFM_DISABLE TSharedPtr<FJsonObject> WriteExportDesc(const FExportDesc& Export);
AUTORTFM_DISABLE FExportDesc ReadExportDesc(const TSharedPtr<FJsonObject>& Object);

/// The declared-type table of one class, as JSON, and back. Null in, null out: a class the analysis
/// could not describe carries no table rather than an empty one, and the two mean different things
/// -- an empty table says "this class declares nothing", which would be a lie.
AUTORTFM_DISABLE TSharedPtr<FJsonObject> WriteDeclaredTypes(const FDeclaredTypes& Types);
AUTORTFM_DISABLE TSharedPtr<FDeclaredTypes> ReadDeclaredTypes(const TSharedPtr<FJsonObject>& Object);

/// The kind a description read back out of a sidecar is, recovered from the fields it carries,
/// since the sidecar has no field for one.
AUTORTFM_DISABLE EDeclaredKind RecordedKind(const FMemberType& Type);

/// What a description of Kind reads back as: itself, but for TypedContainer, which describes
/// exactly as Other does and which every converter treats alike. The cook `ensure`s
/// RecordedKind(Type) == RecordsAs(Type.Kind) for every type it writes.
AUTORTFM_DISABLE EDeclaredKind RecordsAs(EDeclaredKind Kind);

/// The origin a reference reads back with: itself, but for Binding, which the sidecar's `refOrigin`
/// (shared with the interpreter) records as Other.
AUTORTFM_DISABLE EClassOrigin RecordsAs(EClassOrigin Origin);

/// The table CollectEngineSignalTypes answers, as JSON, and back.
AUTORTFM_DISABLE TSharedPtr<FJsonObject> WriteEngineSignalTypes(const FEngineSignalTypes& Types);
AUTORTFM_DISABLE TSharedPtr<FEngineSignalTypes> ReadEngineSignalTypes(const TSharedPtr<FJsonObject>& Object);

} // namespace GodotVerse
