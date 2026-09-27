// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "AutoRTFM.h"
#include "Templates/UnrealTemplate.h"
#include "VerseVM/Inline/VVMEnterVMInline.h"
#include "VerseVM/VVMContext.h"

namespace GodotVerse {

/// Counts one entry into the VM, and replaces a terminated project scope at the outermost one.
///
/// Defined in HostScript.cpp, beside the project scope and the depth counter it maintains.
struct FVerseEntry
{
    AUTORTFM_DISABLE FVerseEntry();
    AUTORTFM_DISABLE ~FVerseEntry();

    FVerseEntry(const FVerseEntry&) = delete;
    FVerseEntry& operator=(const FVerseEntry&) = delete;
};

/// Every entry into the VM goes through here rather than calling Context.EnterVM directly, so that
/// a seventh entry point cannot be added that forgets the scope handling FVerseEntry does. One
/// implementation for every unit: a copy would be a place that handling could drift.
template <typename TBody>
AUTORTFM_DISABLE void EnterVerse(Verse::FRunningContext& Context, TBody&& Body)
{
    FVerseEntry Entry;
    Context.EnterVM(Forward<TBody>(Body));
}

} // namespace GodotVerse
