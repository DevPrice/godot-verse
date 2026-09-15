// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "AutoRTFM.h"
#include "Containers/UnrealString.h"

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

/// Writes the current analysis snapshot to Path. False with OutError set when there is no snapshot
/// to write or the file cannot be written.
AUTORTFM_DISABLE bool WriteClassSidecar(const FString& Path, FUtf8String& OutError);

/// Reads one back and publishes it as *the* snapshot. False with OutError set when the file is
/// missing, unparseable, or written by a different sidecar version.
AUTORTFM_DISABLE bool LoadClassSidecar(const FString& Path, FUtf8String& OutError);

} // namespace GodotVerse
