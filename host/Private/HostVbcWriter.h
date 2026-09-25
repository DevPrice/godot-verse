// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "AutoRTFM.h"
#include "Containers/UnrealString.h"
#include "verse_host_abi.h"

#if VH_HOST_KIND == VH_HOST_KIND_COOKER

namespace GodotVerse {

/// Writes <OutDir>/program.vbc, docs/web-vm/format.md's container, from the program CompileProject
/// linked and initialized, and <OutDir>/program.vbc.report.txt beside it: the may-park op counts
/// per procedure, which is T2.4's input and is not meant to ship.
///
/// False with OutError listing every cell or value the format cannot carry and what reached it; no
/// file is written then. OutSummary is one line for the cook's log.
AUTORTFM_DISABLE bool WriteProgramVbc(const FString& OutDir, int32 Generation, FString& OutSummary, FUtf8String& OutError);

} // namespace GodotVerse

#endif // VH_HOST_KIND == VH_HOST_KIND_COOKER
